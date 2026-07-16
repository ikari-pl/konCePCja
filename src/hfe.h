/* hfe.h — HxC Floppy Emulator "HFE" v1 -> in-memory SCP transcoder.
 *
 * HFE (Rev 3.1, HxC2001 — hxc2001.com/download/floppy_drive_emulator/
 * HxC_Floppy_Emulator_HFE_file_format.pdf, public vendor spec, DOC-safe;
 * see docs/hardware/flux-formats-feasibility.md §2.3) is NOT flux-native:
 * it stores fixed-rate MFM/FM *bitcells*, one abstraction level above flux.
 * Unlike a2r.cpp (a container transcode of already-timed flux), HFE needs a
 * bitcell -> flux expansion step first — but that expansion is deterministic
 * and PLL-free: walking a side's bitstream, a `1` bit IS a flux reversal at
 * that cell boundary, a `0` is not; the interval since the previous `1` is
 * (zeros_between + 1) * cell_time. No clock recovery is needed on the way
 * in (docs/hardware/flux-formats-feasibility.md §2.3, "the load-bearing
 * section").
 *
 * That bitcell-to-flux contract is *exactly* the second reuse rung the
 * flux-ingestion contract describes (docs/hardware/flux-ingestion-contract.md
 * §1.1): "already have decoded MFM bitcells per revolution -> build
 * t_mfm_track entries, call scp_from_mfm_tracks()". This module does the
 * former (HFE header/LUT parse + side de-interleave + LSb-first bit-order
 * fixup) and hands off to the latter (src/ipf.h), reusing 100% of the
 * SCP-container-assembly logic the IPF/CAPS mirror already has tested,
 * rather than re-deriving flux-word emission a second time.
 *
 * Decision — reuse, not direct emission: scp_from_mfm_tracks hard-codes
 * kTicksPerCell = 80 (a 2 us bitcell / 25 ns SCP tick — the CPC/IBM
 * double-density MFM standard). hfe_ticks_per_cell() below computes the
 * *general* HFE bitrate -> SCP-tick ratio so the arithmetic is checkable
 * on its own; hfe_to_scp() only proceeds down the reuse path when that
 * ratio comes out to exactly 80 (bitRate == 250 kbit/s), which is what
 * every CPC HFE dump uses (floppyinterfacemode CPC_DD_FLOPPYMODE = 0x06 is
 * DD-only). A non-CPC-standard bitrate is rejected (HFE_E_UNSUPPORTED)
 * rather than silently mis-timed through a mismatched constant — v1 scope
 * only needs CPC media, so no second flux-emission path is written (and
 * therefore no second implementation of that arithmetic exists to drift
 * out of sync with the tested one in ipf.cpp).
 *
 * Only side 0 is emitted: the sub-cycle FDC's flux cache is side-0-only by
 * construction end to end (docs/hardware/flux-ingestion-contract.md §1),
 * matching the IPF decoder's own side-0-only flux mirror. A single revolution
 * per cylinder is emitted — HFE v1 stores one fixed bitcell stream per
 * track with no weak-bit/opcode variation (that's HFE v3, out of scope),
 * so a second captured revolution would just repeat the first.
 *
 * Clean-room note: implemented from the public HFE spec text above (and the
 * hxc2001.com wiki mirror of it) only. libhxcfe (the reference decoder) is
 * GPL and was never opened for this module.
 */
#ifndef KONCPC_HFE_H
#define KONCPC_HFE_H

#include <cstddef>
#include <cstdint>
#include <vector>

enum : std::int8_t {
  HFE_E_NOT_HFE = -1,     /* missing "HXCPICFE" (v1/v2) signature; note the
                           * distinct "HXCHFEV3" magic also maps here      */
  HFE_E_UNSUPPORTED = -2, /* HFE v3, a formatrevision other than 0 (v1),
                           * a non-CPC-standard bitrate (only 250 kbit/s -
                           * 2 us cells - is accepted), or more tracks than
                           * the SCP side-0 slot space (84) can address     */
  HFE_E_TRUNCATED = -3,   /* header / track LUT / a track's data block runs
                           * past the end of the supplied buffer            */
  HFE_E_GEOMETRY = -4,    /* zero tracks, or scp_from_mfm_tracks rejected
                           * the assembled bitcell set (e.g. every track
                           * came out unformatted, or too many cylinders)   */
};

/* SCP ticks (25 ns each) per HFE bitcell for a header bitRate (kbit/s).
 * Pure helper, exposed for unit tests:
 *
 *   cell_time (s)  = 1 / (bitRate * 1000 * 2)          [HFE spec formula]
 *   ticks (25 ns)  = cell_time / 25e-9
 *                  = 1e9 / (bitRate * 2000 * 25)
 *                  = 20000 / bitRate                    [integer form]
 *
 * At the CPC/IBM DD-MFM standard bitRate = 250: 20000/250 = 80 ticks/cell
 * = 2 us, exactly the kTicksPerCell constant scp_from_mfm_tracks (src/
 * ipf.cpp) is built around. Returns 0 for bitRate == 0 (division-by-zero
 * guard — always treated as unsupported by the caller). Truncates for
 * bitRates that don't divide 20000 exactly; hfe_to_scp() only accepts the
 * one exact value, so a truncated (inexact) result never reaches the SCP
 * output silently. */
uint32_t hfe_ticks_per_cell(uint16_t bit_rate_kbit_s);

/* Transcode an in-memory HFE v1 image (`hfe`/`len`) into an in-memory SCP
 * flux buffer (`out`) matching Machine::insert_flux's contract (see
 * docs/hardware/flux-ingestion-contract.md §1). Side 0 only, one
 * revolution per cylinder. Returns 0 on success or a negative HFE_E_* code;
 * `out` is left empty on failure. */
int hfe_to_scp(const uint8_t* hfe, size_t len, std::vector<uint8_t>& out);

#endif  // KONCPC_HFE_H
