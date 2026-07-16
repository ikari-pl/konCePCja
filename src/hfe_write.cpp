/* hfe_write.cpp — see hfe_write.h. HFE v1 encoder (inverse of hfe.cpp) plus a
 * disk-level clean/dirty compositor. Pure functions, std::vector-owned out. */
#include "hfe_write.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include "hw/flux.h"     // flux_scp_probe
#include "mfm_encode.h"  // mfm_tracks_from_dsk

namespace {

constexpr std::size_t kHeaderSize = 512;
constexpr std::size_t kBlockSize = 512;
constexpr std::size_t kSideChunk = 256;  // side-0 bytes per 512-B block
constexpr uint16_t kCpcBitRate = 250;    // kbit/s -> 2 us cells (80 ticks)
constexpr int kMaxCyls = 84;             // side-0 SCP slot space
constexpr uint32_t kTicksPerCell = 80;   // 25 ns SCP ticks per 2 us bitcell

void put16le(std::vector<uint8_t>& out, std::size_t offset, uint16_t value) {
  out[offset] = static_cast<uint8_t>(value & 0xFF);
  out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

int bit_at(const t_mfm_rev& rev, uint32_t logical) {
  return (rev.bits[logical >> 3] >> (7 - (logical & 7))) & 1;
}

// De-pack one revolution's MSb-first bitcells into HFE side-0 bytes (bit i of
// the logical stream becomes LSb-first bit (i&7) of side-0 byte (i>>3)).
std::vector<uint8_t> side0_bytes(const t_mfm_rev& rev) {
  const std::size_t count = rev.nbits / 8u;
  std::vector<uint8_t> bytes(count, 0);
  for (std::size_t i = 0; i < count; i++) {
    uint8_t value = 0;
    for (int bit = 0; bit < 8; bit++) {
      const uint32_t logical =
          (static_cast<uint32_t>(i) * 8u) + static_cast<uint32_t>(bit);
      if (bit_at(rev, logical)) value |= static_cast<uint8_t>(1u << bit);
    }
    bytes[i] = value;
  }
  return bytes;
}

// Interleave side-0 bytes with 256-byte side-1 filler blocks the way
// hfe_to_scp de-interleaves them: 256 side-0 then 256 side-1, but NO trailing
// side-1 after the final (possibly short) side-0 chunk — so the decoder's
// combined-byte walk extracts exactly these side-0 bytes and no more.
std::vector<uint8_t> interleave(const std::vector<uint8_t>& side0) {
  std::vector<uint8_t> combined;
  combined.reserve(side0.size() * 2);
  std::size_t pos = 0;
  while (pos < side0.size()) {
    const std::size_t chunk = std::min(kSideChunk, side0.size() - pos);
    combined.insert(combined.end(), side0.begin() + static_cast<long>(pos),
                    side0.begin() + static_cast<long>(pos + chunk));
    pos += chunk;
    if (pos < side0.size())  // more side-0 to come -> a side-1 filler block
      combined.insert(combined.end(), kSideChunk, 0xFF);
  }
  return combined;
}

// ---- source-SCP reconstruction (clean tracks on the disk-level path) --------

uint32_t rd32le(const uint8_t* base) {
  return static_cast<uint32_t>(base[0]) |
         (static_cast<uint32_t>(base[1]) << 8) |
         (static_cast<uint32_t>(base[2]) << 16) |
         (static_cast<uint32_t>(base[3]) << 24);
}
uint32_t rd16be(const uint8_t* base) {
  return (static_cast<uint32_t>(base[0]) << 8) | base[1];
}

constexpr std::size_t kScpTlutOff = 0x10;
constexpr std::size_t kScpTlutSlots = 168;
constexpr std::size_t kScpTlutEnd = kScpTlutOff + (4 * kScpTlutSlots);

std::size_t tlut_byte(std::size_t slot) { return kScpTlutOff + (4u * slot); }

struct ScpReader {
  const uint8_t* base = nullptr;
  std::size_t len = 0;
  bool legacy = false;
  int cyls = 0;

  uint32_t track_offset(int cyl) const {
    const std::size_t slot = static_cast<std::size_t>(legacy ? cyl : cyl * 2);
    if (slot >= kScpTlutSlots) return 0;
    return rd32le(base + tlut_byte(slot));
  }
};

bool read_scp(const uint8_t* scp, std::size_t len, ScpReader& reader) {
  if (scp == nullptr || len < kScpTlutEnd || std::memcmp(scp, "SCP", 3) != 0)
    return false;
  if (scp[0x05] == 0 || scp[0x0A] == 2) return false;
  reader.base = scp;
  reader.len = len;
  reader.legacy = false;
  if (scp[0x0A] != 0)
    for (std::size_t slot = 1; slot < kScpTlutSlots; slot += 2)
      if (rd32le(scp + tlut_byte(slot)) != 0) {
        reader.legacy = true;
        break;
      }
  reader.cyls = 0;
  for (int cyl = 0; cyl < kMaxCyls; cyl++)
    if (reader.track_offset(cyl) != 0) reader.cyls = cyl + 1;
  return reader.cyls > 0;
}

// Rebuild revolution 0's bitcells from a clean track's flux. The flux was
// emitted at exactly gap*80 ticks per cell (scp_from_mfm_tracks), so rounding
// each interval to the nearest whole cell recovers the original bitcells; the
// tail is padded to a byte boundary (HFE stores whole bytes only).
t_mfm_rev reconstruct_rev(const ScpReader& reader, uint32_t toff) {
  t_mfm_rev rev;
  const std::size_t entry = static_cast<std::size_t>(toff) + 4;
  if (entry + 12 > reader.len) return rev;
  const uint32_t words = rd32le(reader.base + entry + 4);
  const uint32_t data_off = rd32le(reader.base + entry + 8);
  const std::size_t start = static_cast<std::size_t>(toff) + data_off;
  if (start + (static_cast<std::size_t>(words) * 2u) > reader.len) return rev;

  uint32_t bit_count = 0;
  uint32_t carry = 0;
  auto emit = [&](int value) {
    if ((bit_count >> 3) >= rev.bits.size()) rev.bits.push_back(0);
    if (value)
      rev.bits[bit_count >> 3] |=
          static_cast<uint8_t>(0x80u >> (bit_count & 7));
    bit_count++;
  };
  for (uint32_t i = 0; i < words; i++) {
    const uint32_t word = rd16be(reader.base + start + (2u * i));
    if (word == 0) {  // overflow run
      carry += 0x10000u;
      continue;
    }
    const uint32_t interval = carry + word;
    carry = 0;
    uint32_t cells = (interval + (kTicksPerCell / 2)) / kTicksPerCell;
    if (cells == 0) cells = 1;
    for (uint32_t j = 1; j < cells; j++) emit(0);
    emit(1);
  }
  while ((bit_count & 7) != 0) emit(0);  // pad to a whole byte
  rev.nbits = bit_count;
  return rev;
}

}  // namespace

int hfe_from_mfm_tracks(const std::vector<t_mfm_track>& cyls,
                        std::vector<uint8_t>& out) {
  out.clear();
  const std::size_t num_tracks = cyls.size();
  if (num_tracks == 0) return HFE_E_GEOMETRY;
  if (num_tracks > static_cast<std::size_t>(kMaxCyls)) return HFE_E_UNSUPPORTED;

  bool any_present = false;
  for (const t_mfm_track& trk : cyls)
    if (!trk.empty() && trk[0].nbits != 0) any_present = true;
  if (!any_present) return HFE_E_GEOMETRY;

  // Header (512 B, unused fields = 0xFF per spec convention) + zeroed LUT
  // block.
  out.assign(kHeaderSize, 0xFF);
  std::memcpy(out.data(), "HXCPICFE", 8);
  out[0x08] = 0;  // v1
  out[0x09] = static_cast<uint8_t>(num_tracks);
  out[0x0A] = 1;  // one side
  out[0x0B] = 0;  // ISOIBM MFM encoding
  put16le(out, 0x0C, kCpcBitRate);
  put16le(out, 0x0E, 300);  // RPM (informational)
  out[0x10] = 0x06;         // CPC_DD_FLOPPYMODE
  out[0x11] = 0xFF;
  put16le(out, 0x12, 1);  // track LUT at block 1
  out[0x14] = 0xFF;
  out[0x15] = 0xFF;
  out.resize(2 * kBlockSize, 0x00);  // LUT block, zeroed

  uint16_t next_block = 2;
  for (std::size_t track = 0; track < num_tracks; track++) {
    const t_mfm_track& trk = cyls[track];
    const std::size_t lut = kBlockSize + (4u * track);
    if (trk.empty() || trk[0].nbits == 0) {
      put16le(out, lut, 0);
      put16le(out, lut + 2, 0);
      continue;
    }
    if (trk[0].nbits % 8u != 0) {  // HFE stores whole bytes only
      out.clear();
      return HFE_E_GEOMETRY;
    }
    const std::vector<uint8_t> combined = interleave(side0_bytes(trk[0]));
    const std::size_t byte_off =
        static_cast<std::size_t>(next_block) * kBlockSize;
    const std::size_t blocks = (combined.size() + kBlockSize - 1) / kBlockSize;
    if (out.size() < byte_off + (blocks * kBlockSize))
      out.resize(byte_off + (blocks * kBlockSize), 0xFF);
    std::memcpy(out.data() + byte_off, combined.data(), combined.size());
    put16le(out, lut, next_block);
    put16le(out, lut + 2, static_cast<uint16_t>(combined.size()));
    next_block = static_cast<uint16_t>(next_block + blocks);
  }
  return 0;
}

int hfe_from_disk(const uint8_t* orig_scp, std::size_t orig_len,
                  const uint8_t* dsk, std::size_t dsk_len,
                  const bool* track_dirty, int ntracks,
                  std::vector<uint8_t>& out) {
  out.clear();
  const bool have_orig =
      orig_scp != nullptr && orig_len > 0 && flux_scp_probe(orig_scp, orig_len);
  const std::vector<t_mfm_track> synth = mfm_tracks_from_dsk(dsk, dsk_len);

  if (!have_orig) return hfe_from_mfm_tracks(synth, out);

  ScpReader reader;
  if (!read_scp(orig_scp, orig_len, reader))
    return hfe_from_mfm_tracks(synth, out);

  int total = std::max(reader.cyls, ntracks);
  total = std::max(total, static_cast<int>(synth.size()));
  total = std::min(total, kMaxCyls);

  std::vector<t_mfm_track> cyls(static_cast<std::size_t>(total));
  for (int cyl = 0; cyl < total; cyl++) {
    const bool dirty =
        track_dirty != nullptr && cyl < ntracks && track_dirty[cyl];
    const bool has_synth = cyl < static_cast<int>(synth.size()) &&
                           !synth[static_cast<std::size_t>(cyl)].empty();
    t_mfm_track& slot = cyls[static_cast<std::size_t>(cyl)];

    if (dirty) {
      if (has_synth) slot = synth[static_cast<std::size_t>(cyl)];  // resynth
      continue;  // else erased
    }

    const uint32_t toff = reader.track_offset(cyl);
    if (toff != 0) {  // clean and present in the original capture
      t_mfm_rev rev = reconstruct_rev(reader, toff);
      if (rev.nbits != 0) slot.push_back(std::move(rev));
    } else if (has_synth) {  // clean but only the DSK has it
      slot = synth[static_cast<std::size_t>(cyl)];
    }
  }
  return hfe_from_mfm_tracks(cyls, out);
}
