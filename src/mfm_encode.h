/* mfm_encode.h — labeled IBM System 34 double-density MFM synthesizer:
 * sectors -> MFM bitcells. The exact inverse of the track scanner in
 * src/hw/flux.cpp (scan_track): where that module slides over a bitcell
 * stream recovering 0x4489 syncs / address marks / CRC-checked ID and data
 * fields, this one lays those same structures down.
 *
 * One standard revolution per track (6250 byte-cells at 300 RPM double
 * density) is emitted, packed MSb-first into a t_mfm_rev (src/ipf.h) — the
 * same bitcell container the IPF decoder's side-0 mirror and hfe_to_scp feed
 * to scp_from_mfm_tracks. So a synthesized track drops straight into the SCP
 * flux-container assembler with no second flux-emission path.
 *
 * MFM cell contract (must match flux.cpp read_mfm_byte, which samples data at
 * the ODD half-cell of each pair): every data bit becomes a (clock, data)
 * cell pair; the clock cell is 1 only when BOTH the previous and current data
 * bits are 0 (the standard MFM minimum-transition rule). The A1 sync mark is
 * the fixed 16-cell 0x4489 pattern — 0xA1 with the clock bit that would fall
 * between data bits d3 and d2 suppressed (the "missing clock") — emitted
 * verbatim, never through the normal cell encoder.
 *
 * Clean-room: written from the IBM System 34 / ECMA-147 MFM track-format
 * description and the observable contract of flux.cpp's decoder only.
 */
#ifndef KONCPC_MFM_ENCODE_H
#define KONCPC_MFM_ENCODE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ipf.h"  // t_mfm_rev, t_mfm_track

// One sector to synthesize. c/h/r/n are the IBM CHRN address-field values
// (cylinder, head, record/sector-id, size-code); the stored payload is
// 128<<size_code bytes and MUST be exactly that long.
struct MfmSector {
  uint8_t cyl = 0;        // C — cylinder in the ID field
  uint8_t head = 0;       // H — head in the ID field
  uint8_t sec_id = 0;     // R — sector id (record) in the ID field
  uint8_t size_code = 2;  // N — 128<<N payload bytes (<= 5, i.e. <= 4096)
  std::vector<uint8_t> payload;
  uint8_t dam = 0xFB;  // data address mark: 0xFB = normal, 0xF8 = deleted
};

// One track's worth of sectors plus the inter-sector GAP3 length. gap3 is a
// count of 0x4E filler bytes written after each sector's data-field CRC
// (0x52 = 82, the customary CPC/IBM 9-sector value).
struct MfmTrackDesc {
  std::vector<MfmSector> sectors;
  uint8_t gap3 = 0x52;
};

// CRC-CCITT (poly 0x1021, init 0xFFFF, no bit reflection) — the IBM /
// System-34 disk CRC covering address and data fields (including the three
// leading 0xA1 sync bytes). Exposed for unit tests; matches flux.cpp's
// crc_byte chain. Known vector: "123456789" -> 0x29B1.
uint16_t mfm_crc_ccitt(const uint8_t* data, std::size_t len);

// The fixed 16-cell 0x4489 A1 missing-clock sync mark (MSb-first). Exposed so
// tests can assert the exact pattern the encoder lays down.
constexpr uint16_t kMfmA1SyncCells = 0x4489;

// Expand one data byte to its 16 MFM cells (MSb-first: cell 0 in bit 15 of
// the result). prev_data_bit carries the previous data bit in (the clock
// rule needs it) and is updated to this byte's last data bit on return.
// Exposed for unit tests.
uint16_t mfm_expand_byte(uint8_t value, int& prev_data_bit);

// Synthesize ONE revolution of one track from its sector list. The layout:
// GAP4a, then per sector { 12x00 sync, 3x A1, 0xFE IDAM, C H R N, ID CRC,
// GAP2, 12x00 sync, 3x A1, data mark, payload, data CRC, GAP3 }, then GAP4b
// 0x4E filler out to the 6250-byte revolution length. Sectors whose payload
// length disagrees with 128<<size_code, or size_code > 5, are skipped.
t_mfm_rev mfm_encode_track(const MfmTrackDesc& desc);

// Build per-cylinder single-revolution MFM captures from a whole standard or
// EXTENDED DSK image (side 0 only — matching the flux path's side-0 geometry).
// Absent tracks become empty t_mfm_track slots. Empty on a non-DSK buffer.
std::vector<t_mfm_track> mfm_tracks_from_dsk(const uint8_t* dsk,
                                             std::size_t len);

#endif  // KONCPC_MFM_ENCODE_H
