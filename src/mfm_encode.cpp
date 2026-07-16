/* mfm_encode.cpp — see mfm_encode.h. The IBM System 34 DD MFM synthesizer
 * and a whole-DSK -> per-cylinder bitcell adapter. Pure functions, caller /
 * std::vector owned buffers, fixed-size function-local scratch. */
#include "mfm_encode.h"
#include <cstdint>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace {

// ------------------------------------------------------- MFM cell writer -----

// Appends bitcells MSb-first into a growing t_mfm_rev-style packed buffer,
// exactly as scp_from_mfm_tracks / flux.cpp expect (logical cell i lives at
// bits[i>>3], bit position 0x80>>(i&7)).
struct CellWriter {
  std::vector<uint8_t> bits;
  uint32_t count = 0;

  void cell(int bit_value) {
    if ((count >> 3) >= bits.size()) bits.push_back(0);
    if (bit_value) bits[count >> 3] |= static_cast<uint8_t>(0x80u >> (count & 7));
    count++;
  }

  // Emit a raw 16-cell pattern MSb-first (used for the A1 missing-clock mark).
  void raw16(uint16_t pattern) {
    for (int i = 15; i >= 0; i--) cell((pattern >> i) & 1);
  }
};

constexpr uint8_t kGapByte = 0x4E;       // MFM gap filler
constexpr uint8_t kSyncByte = 0x00;      // sync field (encodes to 0xAAAA cells)
constexpr uint8_t kIdMark = 0xFE;        // ID address mark
constexpr uint8_t kAoneByte = 0xA1;      // the sync byte the A1 cells encode
constexpr int kAoneCount = 3;            // three A1 marks precede every field
constexpr int kGap4aBytes = 80;          // post-index gap
constexpr int kSyncBytes = 12;           // 12x 0x00 before each A1 run
constexpr int kGap2Bytes = 22;           // ID-field -> data-field gap
constexpr uint8_t kMaxSizeCode = 5;      // 128<<5 = 4096, the DD ceiling
constexpr uint32_t kRevByteCells = 6250; // one DD revolution at 300 RPM
constexpr uint32_t kRevCells = kRevByteCells * 16u;

// Emit one data byte as 16 MFM cells through the normal clock rule.
void put_byte(CellWriter& writer, uint8_t value, int& prev_data_bit) {
  writer.raw16(mfm_expand_byte(value, prev_data_bit));
}

// Emit `repeat` copies of the same gap/sync byte.
void put_fill(CellWriter& writer, uint8_t value, int repeat, int& prev_data_bit) {
  for (int i = 0; i < repeat; i++) put_byte(writer, value, prev_data_bit);
}

// Emit the three-A1 sync run verbatim; the last data cell of 0x4489 is a 1,
// so the running previous-data bit becomes 1 for the mark byte that follows.
void put_sync_marks(CellWriter& writer, int& prev_data_bit) {
  for (int i = 0; i < kAoneCount; i++) writer.raw16(kMfmA1SyncCells);
  prev_data_bit = 1;
}

// Emit a big-endian CRC-CCITT value (high byte first — the on-disk order).
void put_crc(CellWriter& writer, uint16_t crc, int& prev_data_bit) {
  put_byte(writer, static_cast<uint8_t>(crc >> 8), prev_data_bit);
  put_byte(writer, static_cast<uint8_t>(crc & 0xFF), prev_data_bit);
}

// One sector's ID + data fields. Returns false (emits nothing) if the sector
// is malformed, so a bad descriptor cannot desynchronize the track.
bool encode_sector(CellWriter& writer, const MfmSector& sector, uint8_t gap3,
                   int& prev_data_bit) {
  if (sector.size_code > kMaxSizeCode) return false;
  const std::size_t payload_len = static_cast<std::size_t>(128) << sector.size_code;
  if (sector.payload.size() != payload_len) return false;

  // ---- ID field: sync, 3x A1, IDAM, C H R N, CRC(A1 A1 A1 FE C H R N) ----
  put_fill(writer, kSyncByte, kSyncBytes, prev_data_bit);
  put_sync_marks(writer, prev_data_bit);
  put_byte(writer, kIdMark, prev_data_bit);

  std::array<uint8_t, 8> id_crc_input = {
      kAoneByte, kAoneByte, kAoneByte, kIdMark,
      sector.cyl, sector.head, sector.sec_id, sector.size_code};
  put_byte(writer, sector.cyl, prev_data_bit);
  put_byte(writer, sector.head, prev_data_bit);
  put_byte(writer, sector.sec_id, prev_data_bit);
  put_byte(writer, sector.size_code, prev_data_bit);
  put_crc(writer, mfm_crc_ccitt(id_crc_input.data(), id_crc_input.size()),
          prev_data_bit);

  // ---- GAP2, then data field: sync, 3x A1, DAM, payload, CRC ----
  put_fill(writer, kGapByte, kGap2Bytes, prev_data_bit);
  put_fill(writer, kSyncByte, kSyncBytes, prev_data_bit);
  put_sync_marks(writer, prev_data_bit);
  put_byte(writer, sector.dam, prev_data_bit);

  std::vector<uint8_t> data_crc_input;
  data_crc_input.reserve(kAoneCount + 1 + payload_len);
  for (int i = 0; i < kAoneCount; i++) data_crc_input.push_back(kAoneByte);
  data_crc_input.push_back(sector.dam);
  for (const uint8_t byte_value : sector.payload) {
    put_byte(writer, byte_value, prev_data_bit);
    data_crc_input.push_back(byte_value);
  }
  put_crc(writer, mfm_crc_ccitt(data_crc_input.data(), data_crc_input.size()),
          prev_data_bit);

  // ---- GAP3 ----
  put_fill(writer, kGapByte, gap3, prev_data_bit);
  return true;
}

// -------------------------------------------------------- DSK adapter --------

uint16_t rd16le(const uint8_t* base) {
  return static_cast<uint16_t>(base[0] | (base[1] << 8));
}

constexpr std::size_t kDskHeaderSize = 0x100;
constexpr std::size_t kTrackInfoSize = 0x100;
constexpr std::size_t kSectorInfoBase = 0x18;
constexpr std::size_t kSectorInfoStride = 8;
constexpr int kDskMaxCyls = 84;  // matches scp_from_mfm_tracks' side-0 slots

// Read one Track-Info block (its header already validated) into a sector list.
// `off` is the block's absolute file offset; `len` bounds every access.
MfmTrackDesc parse_dsk_track(const uint8_t* dsk, std::size_t off,
                             std::size_t len, bool extended) {
  const uint8_t* info = dsk + off;
  const int num_sectors = info[0x15];

  MfmTrackDesc desc;
  const uint8_t gap3 = info[0x16];
  if (gap3 != 0) desc.gap3 = gap3;
  desc.sectors.reserve(static_cast<std::size_t>(num_sectors));

  std::size_t data_rel = kTrackInfoSize;  // payloads start after Track-Info
  for (int sec = 0; sec < num_sectors; sec++) {
    const std::size_t info_off =
        kSectorInfoBase + (static_cast<std::size_t>(sec) * kSectorInfoStride);
    if (off + info_off + kSectorInfoStride > len) break;
    const uint8_t* sinfo = info + info_off;

    MfmSector sector;
    sector.cyl = sinfo[0];
    sector.head = sinfo[1];
    sector.sec_id = sinfo[2];
    sector.size_code = sinfo[3];
    const uint8_t status2 = sinfo[5];
    sector.dam = (status2 & 0x40) ? 0xF8 : 0xFB;  // ST2 Control Mark -> deleted

    const std::size_t logical_len =
        static_cast<std::size_t>(128) << (sector.size_code & 0x07);
    const std::size_t actual_len = extended ? rd16le(sinfo + 6) : logical_len;

    if (off + data_rel + logical_len <= len && sector.size_code <= kMaxSizeCode) {
      sector.payload.assign(info + data_rel, info + data_rel + logical_len);
      desc.sectors.push_back(std::move(sector));
    }
    data_rel += (actual_len != 0 ? actual_len : logical_len);
  }
  return desc;
}

}  // namespace

uint16_t mfm_crc_ccitt(const uint8_t* data, std::size_t len) {
  uint16_t crc = 0xFFFF;
  for (std::size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(static_cast<uint16_t>(data[i]) << 8);
    for (int bit = 0; bit < 8; bit++)
      crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                            : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

uint16_t mfm_expand_byte(uint8_t value, int& prev_data_bit) {
  uint16_t cells = 0;
  for (int i = 7; i >= 0; i--) {
    const int data_bit = (value >> i) & 1;
    const int clock_bit = (prev_data_bit == 0 && data_bit == 0) ? 1 : 0;
    cells = static_cast<uint16_t>((cells << 1) | static_cast<uint16_t>(clock_bit));
    cells = static_cast<uint16_t>((cells << 1) | static_cast<uint16_t>(data_bit));
    prev_data_bit = data_bit;
  }
  return cells;
}

t_mfm_rev mfm_encode_track(const MfmTrackDesc& desc) {
  CellWriter writer;
  int prev_data_bit = 0;

  put_fill(writer, kGapByte, kGap4aBytes, prev_data_bit);
  for (const MfmSector& sector : desc.sectors)
    encode_sector(writer, sector, desc.gap3, prev_data_bit);

  // GAP4b: pad the revolution out to its nominal cell length so the flux
  // container carries a full-length index-to-index interval.
  while (writer.count < kRevCells) put_byte(writer, kGapByte, prev_data_bit);

  t_mfm_rev rev;
  rev.bits = std::move(writer.bits);
  rev.nbits = writer.count;
  return rev;
}

std::vector<t_mfm_track> mfm_tracks_from_dsk(const uint8_t* dsk,
                                             std::size_t len) {
  if (dsk == nullptr || len < kDskHeaderSize) return {};

  const bool extended = std::memcmp(dsk, "EXTENDED", 8) == 0;
  const bool standard = std::memcmp(dsk, "MV - CPC", 8) == 0;
  if (!extended && !standard) return {};

  int num_cyls = dsk[0x30];
  const int num_sides = dsk[0x31];
  if (num_sides <= 0 || num_cyls <= 0) return {};
  num_cyls = std::min(num_cyls, kDskMaxCyls);

  const uint16_t std_track_size = rd16le(dsk + 0x32);

  // Byte length of each physical track block (index = cyl*sides + side), in
  // DSK order. Absent (extended, size 0) tracks contribute 0.
  auto block_len = [&](int track_index) -> std::size_t {
    if (extended) return static_cast<std::size_t>(dsk[0x34 + track_index]) * 256u;
    return std_track_size;
  };

  std::vector<t_mfm_track> cyls(static_cast<std::size_t>(num_cyls));

  // Running file offset of each physical track block (DSK order:
  // index = cyl*sides + side, starting right after the 0x100 disc header).
  const int total_tracks = num_cyls * num_sides;
  std::vector<std::size_t> offsets(static_cast<std::size_t>(total_tracks) + 1, 0);
  offsets[0] = kDskHeaderSize;
  for (int i = 0; i < total_tracks; i++)
    offsets[i + 1] = offsets[i] + block_len(i);

  for (int cyl = 0; cyl < num_cyls; cyl++) {
    const int track_index = cyl * num_sides;  // side 0 of this cylinder
    const std::size_t off = offsets[static_cast<std::size_t>(track_index)];
    const std::size_t blk = block_len(track_index);
    if (blk == 0) continue;  // absent/unformatted -> empty slot
    if (off + kTrackInfoSize > len) continue;

    if (std::memcmp(dsk + off, "Track-Info", 10) != 0) continue;

    const MfmTrackDesc desc = parse_dsk_track(dsk, off, len, extended);
    t_mfm_rev rev = mfm_encode_track(desc);
    if (rev.nbits != 0) cyls[static_cast<std::size_t>(cyl)].push_back(std::move(rev));
  }

  return cyls;
}
