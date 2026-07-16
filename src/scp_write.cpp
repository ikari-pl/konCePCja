/* scp_write.cpp — see scp_write.h. Splices verbatim original flux (clean
 * tracks) with freshly synthesized MFM flux (dirty tracks) into one SCP
 * container. Reuses scp_from_mfm_tracks (src/ipf.h) for both the whole
 * New-disc path and the per-track synthesis of dirty tracks, so the flux-word
 * emitter (ipf.cpp push_flux_rev) is never re-implemented here. */
#include "scp_write.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include "hw/flux.h"     // flux_scp_probe
#include "ipf.h"         // scp_from_mfm_tracks, t_mfm_track
#include "mfm_encode.h"  // mfm_tracks_from_dsk

namespace {

constexpr std::size_t kScpTlutOff = 0x10;
constexpr std::size_t kScpTlutSlots = 168;
constexpr std::size_t kScpTlutEnd = kScpTlutOff + (4 * kScpTlutSlots);
constexpr int kScpMaxCyls = 84;  // side-0 slots (cyl*2 must stay < 168)

// Byte offset of track-lookup-table slot `slot` within an SCP buffer.
std::size_t tlut_byte(std::size_t slot) { return kScpTlutOff + (4u * slot); }

uint32_t rd32le(const uint8_t* base) {
  return static_cast<uint32_t>(base[0]) |
         (static_cast<uint32_t>(base[1]) << 8) |
         (static_cast<uint32_t>(base[2]) << 16) |
         (static_cast<uint32_t>(base[3]) << 24);
}

void put_le32(std::vector<uint8_t>& out, std::size_t offset, uint32_t value) {
  out[offset] = value & 0xFF;
  out[offset + 1] = (value >> 8) & 0xFF;
  out[offset + 2] = (value >> 16) & 0xFF;
  out[offset + 3] = (value >> 24) & 0xFF;
}

// One captured revolution's flux, ready to re-emit: the index-to-index tick
// duration and the raw big-endian flux-word bytes.
struct FluxRev {
  uint32_t duration = 0;
  std::vector<uint8_t> words;
};

// Minimal read-side view of a source SCP: enough to map a cylinder to its
// track-data offset and pull each revolution's flux out verbatim.
struct ScpReader {
  const uint8_t* base = nullptr;
  std::size_t len = 0;
  uint8_t revs = 0;
  bool legacy = false;  // old single-sided layout: slot = cyl (docs §1.2)
  int cyls = 0;         // highest present side-0 cylinder + 1

  int slot_of(int cyl) const { return legacy ? cyl : cyl * 2; }

  uint32_t track_offset(int cyl) const {
    const std::size_t slot = static_cast<std::size_t>(slot_of(cyl));
    if (slot >= kScpTlutSlots) return 0;
    return rd32le(base + tlut_byte(slot));
  }
};

// Parse an SCP header + track table well enough to copy tracks out. Mirrors
// hw/flux.cpp scp_geometry (which is file-static there). Returns false on a
// buffer that is not a usable side-0 SCP.
bool read_scp(const uint8_t* scp, std::size_t len, ScpReader& reader) {
  if (scp == nullptr || len < kScpTlutEnd || std::memcmp(scp, "SCP", 3) != 0)
    return false;
  const uint8_t width = scp[0x09];
  const uint8_t heads = scp[0x0A];
  if (width != 0 && width != 16) return false;
  if (scp[0x05] == 0) return false;  // no revolutions
  if (heads == 2) return false;      // side-1-only dump

  reader.base = scp;
  reader.len = len;
  reader.revs = scp[0x05];
  reader.legacy = false;
  if (heads != 0) {  // legacy consecutive single-sided layout?
    for (std::size_t slot = 1; slot < kScpTlutSlots; slot += 2)
      if (rd32le(scp + tlut_byte(slot)) != 0) {
        reader.legacy = true;
        break;
      }
  }
  reader.cyls = 0;
  for (int cyl = 0; cyl < kScpMaxCyls; cyl++)
    if (reader.track_offset(cyl) != 0) reader.cyls = cyl + 1;
  return reader.cyls > 0;
}

// Pull revolution `rev` of the track at `toff` out of the source SCP.
bool extract_rev(const ScpReader& reader, uint32_t toff, int rev,
                 FluxRev& out) {
  const std::size_t tdh = static_cast<std::size_t>(toff) + 4 +
                          (12u * static_cast<std::size_t>(rev));
  if (tdh + 12 > reader.len) return false;
  const uint8_t* entry = reader.base + tdh;
  const uint32_t words = rd32le(entry + 4);
  const uint32_t data_off = rd32le(entry + 8);
  const std::size_t start = static_cast<std::size_t>(toff) + data_off;
  const std::size_t span = static_cast<std::size_t>(words) * 2u;
  if (start + span > reader.len) return false;
  out.duration = rd32le(entry);
  out.words.assign(reader.base + start, reader.base + start + span);
  return true;
}

// Synthesize one revolution's flux from a bitcell rev, reusing
// scp_from_mfm_tracks' flux-word emitter by building a throwaway 1-cylinder,
// 1-revolution SCP and reading its single flux run back out.
bool synth_rev(const t_mfm_rev& rev, FluxRev& out) {
  std::vector<t_mfm_track> one(1);
  one[0].push_back(rev);
  const std::vector<uint8_t> mini = scp_from_mfm_tracks(one);
  ScpReader reader;
  if (!read_scp(mini.data(), mini.size(), reader)) return false;
  const uint32_t toff = reader.track_offset(0);
  if (toff == 0) return false;
  return extract_rev(reader, toff, 0, out);
}

// Emit the SCP container from per-cylinder revolution lists. `present[cyl]`
// gates whether that cylinder is written; each written cylinder must carry
// exactly `revs` revolutions.
std::vector<uint8_t> assemble(const std::vector<std::vector<FluxRev>>& cyl_revs,
                              const std::vector<bool>& present, uint8_t revs) {
  const int cyls = static_cast<int>(cyl_revs.size());
  std::vector<uint8_t> scp(kScpTlutEnd, 0);
  scp[0] = 'S';
  scp[1] = 'C';
  scp[2] = 'P';
  scp[3] = 0x19;
  scp[4] = 0x80;  // disk type: "other"
  scp[5] = revs;
  scp[6] = 0;                                     // start track
  scp[7] = static_cast<uint8_t>((cyls - 1) * 2);  // last side-0 slot
  scp[8] = 0x01;                                  // index-synced flag
  scp[9] = 16;                                    // 16-bit flux words
  scp[0x0A] = 0;                                  // both-heads layout
  scp[0x0B] = 0;                                  // 25 ns resolution

  for (int cyl = 0; cyl < cyls; cyl++) {
    if (!present[static_cast<std::size_t>(cyl)]) continue;
    const std::vector<FluxRev>& revlist =
        cyl_revs[static_cast<std::size_t>(cyl)];
    if (revlist.size() != revs) continue;  // geometry guard

    const uint32_t toff = static_cast<uint32_t>(scp.size());
    put_le32(scp, tlut_byte(static_cast<std::size_t>(cyl) * 2u), toff);
    scp.push_back('T');
    scp.push_back('R');
    scp.push_back('K');
    scp.push_back(static_cast<uint8_t>(cyl * 2));
    const std::size_t tdh_base = scp.size();
    scp.resize(scp.size() + (12u * revs), 0);
    for (uint8_t rev = 0; rev < revs; rev++) {
      const FluxRev& flux = revlist[rev];
      const uint32_t data_off = static_cast<uint32_t>(scp.size()) - toff;
      scp.insert(scp.end(), flux.words.begin(), flux.words.end());
      const std::size_t entry =
          tdh_base + (12u * static_cast<std::size_t>(rev));
      put_le32(scp, entry, flux.duration);
      put_le32(scp, entry + 4, static_cast<uint32_t>(flux.words.size() / 2));
      put_le32(scp, entry + 8, data_off);
    }
  }

  uint32_t sum = 0;  // container checksum: every byte from 0x10 to EOF
  for (std::size_t i = 0x10; i < scp.size(); i++) sum += scp[i];
  put_le32(scp, 0x0C, sum);
  return scp;
}

// Resolve one output cylinder: fill `out` with `revs` revolutions and return
// true when the cylinder should be written. Dirty cylinders are synthesized
// (their single revolution repeated); clean cylinders are copied verbatim.
bool resolve_cylinder(int cyl, const ScpReader& reader, uint8_t revs,
                      const std::vector<t_mfm_track>& synth,
                      const bool* track_dirty, int ntracks,
                      std::vector<FluxRev>& out) {
  const bool dirty =
      track_dirty != nullptr && cyl < ntracks && track_dirty[cyl];
  const bool has_synth = cyl < static_cast<int>(synth.size()) &&
                         !synth[static_cast<std::size_t>(cyl)].empty();

  if (dirty) {
    if (!has_synth) return false;  // erased/absent in the DSK
    FluxRev flux;
    if (!synth_rev(synth[static_cast<std::size_t>(cyl)][0], flux)) return false;
    out.assign(revs, flux);
    return true;
  }

  const uint32_t toff = reader.track_offset(cyl);
  if (toff == 0) return false;  // clean but absent in the original too
  for (uint8_t rev = 0; rev < revs; rev++) {
    FluxRev flux;
    if (!extract_rev(reader, toff, rev, flux)) return false;
    out.push_back(std::move(flux));
  }
  return true;
}

}  // namespace

std::vector<uint8_t> scp_from_disk(const uint8_t* orig_scp,
                                   std::size_t orig_len, const uint8_t* dsk,
                                   std::size_t dsk_len, const bool* track_dirty,
                                   int ntracks) {
  const bool have_orig =
      orig_scp != nullptr && orig_len > 0 && flux_scp_probe(orig_scp, orig_len);

  bool any_dirty = false;
  for (int i = 0; i < ntracks; i++)
    if (track_dirty != nullptr && track_dirty[i]) any_dirty = true;

  // Nothing changed and we hold the original capture: an exact copy.
  if (have_orig && !any_dirty) return {orig_scp, orig_scp + orig_len};

  const std::vector<t_mfm_track> synth = mfm_tracks_from_dsk(dsk, dsk_len);

  // A New flux disc (no original): synthesize every present DSK track. This is
  // exactly scp_from_mfm_tracks' single-revolution contract.
  if (!have_orig) return scp_from_mfm_tracks(synth);

  ScpReader reader;
  if (!read_scp(orig_scp, orig_len, reader)) return scp_from_mfm_tracks(synth);
  const uint8_t revs = reader.revs;

  int total_cyls = std::max(reader.cyls, ntracks);
  total_cyls = std::max(total_cyls, static_cast<int>(synth.size()));
  total_cyls = std::min(total_cyls, kScpMaxCyls);

  std::vector<std::vector<FluxRev>> cyl_revs(
      static_cast<std::size_t>(total_cyls));
  std::vector<bool> present(static_cast<std::size_t>(total_cyls), false);
  for (int cyl = 0; cyl < total_cyls; cyl++)
    present[static_cast<std::size_t>(cyl)] =
        resolve_cylinder(cyl, reader, revs, synth, track_dirty, ntracks,
                         cyl_revs[static_cast<std::size_t>(cyl)]);

  return assemble(cyl_revs, present, revs);
}
