/* hfe.cpp — see hfe.h. Parse an HFE v1 image's header + track LUT, extract
 * side 0's raw bitcell stream per cylinder, and hand it to
 * scp_from_mfm_tracks() (src/ipf.h) to assemble the SCP flux buffer.
 *
 * HFE v1 header layout (Rev 3.1, all fields little-endian; see hfe.h for the
 * provenance note), verified field-by-field against the public spec text and
 * its hxc2001.com wiki mirror:
 *
 *   0x000  8   signature        "HXCPICFE" (v1/v2) / "HXCHFEV3" (v3, N/A)
 *   0x008  1   formatrevision   0 = v1 (only value this module accepts)
 *   0x009  1   number_of_track
 *   0x00A  1   number_of_side   informational only — this module derives
 *                               side-0 presence from the track LUT instead
 *                               (docs/hardware/flux-formats-feasibility.md
 *                               §2.3, "CPC nuances"), so it is never read
 *   0x00B  1   track_encoding   ignored: a `1` bitcell is a flux reversal
 *                               regardless of the FM/MFM group encoding
 *                               used to interpret it at a higher layer
 *   0x00C  2   bitRate          kbit/s
 *   0x00E  2   floppyRPM        ignored (emulator-only metadata)
 *   0x010  1   floppyinterfacemode  ignored (ditto; CPC_DD_FLOPPYMODE=0x06)
 *   0x011  1   dnu              reserved
 *   0x012  2   track_list_offset   track LUT position, in 512-byte blocks
 *   0x014  1   write_allowed    ignored
 *   0x015  1   single_step      ignored (affects head stepping only)
 *   0x016..0x019  track0 alt-encoding overrides — ignored (see track_encoding)
 *
 * Track LUT (pictrack[], at track_list_offset*512): one 4-byte entry per
 * track, `{offset: u16 LE (x512 = byte offset), track_len: u16 LE (bytes,
 * BOTH sides combined)}`.
 *
 * Track data: track_len bytes starting at offset*512, organised as
 * repeating 512-byte blocks of 256 B side 0 followed by 256 B side 1 (the
 * last block may be short — walked by combined byte count, not assumed to
 * be block-padded in the file). Bit order within each byte is LSb-first
 * ("The bits transmission order to the FDC is LSb first: Bit0->Bit1->...
 * ->Bit7->(next byte)") — the OPPOSITE of t_mfm_rev's MSb-first packing
 * convention (src/ipf.h), so every extracted bit is re-packed.
 */
#include "hfe.h"

#include <algorithm>
#include <cstring>

#include "ipf.h"

namespace {

uint16_t rd16le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

constexpr size_t kHeaderSize = 512;
constexpr size_t kBlockSize = 512;
constexpr size_t kSideChunk = 256;         // bytes of one side per 512-B block
constexpr uint32_t kCpcTicksPerCell = 80;  // 2 us / 25 ns — see hfe.h
// 168 SCP TLUT slots, side-0 uses only the even ones (slot = cyl*2) ->
// at most 84 addressable cylinders. Matches ipf.cpp's kScpMaxCyls; real CPC
// media never comes close (max ~42-43 physical tracks).
constexpr uint8_t kMaxCyls = 84;

}  // namespace

uint32_t hfe_ticks_per_cell(uint16_t bit_rate_kbit_s) {
  if (bit_rate_kbit_s == 0) return 0;
  return 20000u / bit_rate_kbit_s;
}

int hfe_to_scp(const uint8_t* hfe, size_t len, std::vector<uint8_t>& out) {
  out.clear();
  if (hfe == nullptr || len < kHeaderSize) return HFE_E_TRUNCATED;

  if (std::memcmp(hfe, "HXCPICFE", 8) != 0) {
    if (std::memcmp(hfe, "HXCHFEV3", 8) == 0)
      return HFE_E_UNSUPPORTED;  // v3 opcode stream — out of scope (hfe.h)
    return HFE_E_NOT_HFE;
  }

  const uint8_t format_revision = hfe[0x08];
  if (format_revision != 0) return HFE_E_UNSUPPORTED;  // only v1 accepted

  const uint8_t number_of_track = hfe[0x09];
  const uint16_t bit_rate = rd16le(hfe + 0x0C);
  const uint16_t track_list_offset = rd16le(hfe + 0x12);

  if (number_of_track == 0) return HFE_E_GEOMETRY;
  if (number_of_track > kMaxCyls) return HFE_E_UNSUPPORTED;

  if (hfe_ticks_per_cell(bit_rate) != kCpcTicksPerCell)
    return HFE_E_UNSUPPORTED;  // non-CPC-standard bitrate (see hfe.h)

  const size_t lut_byte_off =
      static_cast<size_t>(track_list_offset) * kBlockSize;
  const size_t lut_bytes_needed = static_cast<size_t>(number_of_track) * 4u;
  if (lut_byte_off + lut_bytes_needed > len) return HFE_E_TRUNCATED;

  std::vector<t_mfm_track> cyls(number_of_track);

  for (uint8_t t = 0; t < number_of_track; t++) {
    const size_t entry_off = lut_byte_off + (static_cast<size_t>(t) * 4u);
    const uint16_t trk_off_blocks = rd16le(hfe + entry_off);
    const uint16_t trk_len = rd16le(hfe + entry_off + 2);
    if (trk_len == 0) continue;  // absent/unformatted track -> empty slot

    const size_t trk_byte_off =
        static_cast<size_t>(trk_off_blocks) * kBlockSize;
    if (trk_byte_off + trk_len > len) return HFE_E_TRUNCATED;

    // Extract side 0's bytes: the first up-to-256 bytes of every 512-byte
    // block, walking the combined (side0+side1) track_len byte range. This
    // is correct regardless of whether a given block is short (final
    // partial block) — side 0 is always positioned first.
    std::vector<uint8_t> side0_bytes;
    side0_bytes.reserve((static_cast<size_t>(trk_len) + 1) / 2);
    size_t pos = 0;
    while (pos < trk_len) {
      const size_t chunk = std::min<size_t>(kSideChunk, trk_len - pos);
      const uint8_t* src = hfe + trk_byte_off + pos;
      side0_bytes.insert(side0_bytes.end(), src, src + chunk);
      pos += chunk;
      if (pos >= trk_len) break;
      pos += std::min<size_t>(kSideChunk, trk_len - pos);  // skip side 1
    }

    // Bit-order fixup: HFE transmits LSb-first; t_mfm_rev.bits is packed
    // MSb-first (matches scp_from_mfm_tracks / push_flux_rev's convention —
    // logical bit i lives at bits[i>>3], bit position (7-(i&7))).
    t_mfm_rev rev;
    rev.nbits = static_cast<uint32_t>(side0_bytes.size()) * 8u;
    rev.bits.assign((rev.nbits + 7) / 8, 0);
    for (size_t i = 0; i < side0_bytes.size(); i++) {
      const uint8_t byte = side0_bytes[i];
      for (int b = 0; b < 8; b++) {
        if (!((byte >> b) & 1)) continue;  // LSb-first source order
        const uint32_t logical_bit =
            (static_cast<uint32_t>(i) * 8u) + static_cast<uint32_t>(b);
        rev.bits[logical_bit >> 3] |= (0x80u >> (logical_bit & 7));
      }
    }

    cyls[t].push_back(std::move(rev));  // single revolution — see hfe.h
  }

  out = scp_from_mfm_tracks(cyls);
  return out.empty() ? HFE_E_GEOMETRY : 0;
}
