/* kryoflux_stream.h — KryoFlux STREAM (.raw) → in-memory SCP transcoder.
 *
 * Clean-room, implemented ONLY from public documentation:
 *   "KryoFlux Stream File Documentation" Rev 1.1 (Jean Louis-Guerin /
 *   DrCoolZic, 2013-12-01, marked "Copyleft") plus this project's own specs
 *   docs/hardware/flux-formats-feasibility.md and flux-ingestion-contract.md.
 * No GPL reader and no SPS Decoder Library code was consulted. fluxfox (MIT) was
 * used only to cross-check numeric constants, never for code.
 *
 * WHY A TRANSCODER, NOT A DECODER
 * -------------------------------
 * The engine already has a complete flux pipeline (src/hw/flux.*): a software
 * PLL + IBM System-34 MFM decoder that consumes an in-memory SuperCard Pro
 * (SCP) byte image. KryoFlux STREAM is the same *kind* of artifact — raw flux
 * transition timings — in a different container. So, exactly like src/hw/a2r
 * does for Applesauce A2R, we transcode STREAM into SCP bytes and reuse the
 * existing decoder unchanged. This file emits nothing but a container; it runs
 * no PLL and recovers no bits.
 *
 * ONE FILE PER TRACK (the KryoFlux reality)
 * -----------------------------------------
 * A real KryoFlux dump is normally ONE FILE PER TRACK AND SIDE, named
 * <base>TT.S.raw (track00.0.raw, track00.1.raw, ...). Each such file is a
 * single stream carrying several revolutions of ONE physical track.
 * Accordingly the primary entry point decodes a SINGLE stream (one track):
 *   - kryoflux_decode_stream()  — stream bytes -> per-revolution flux.
 *   - kryoflux_stream_to_scp()  — stream bytes -> a one-track SCP image.
 * A multi-track SCP is then assembled from a set of per-track streams supplied
 * by the (future) dispatch layer:
 *   - kryoflux_streams_to_scp() — {stream, cyl, side}* -> a multi-track SCP.
 *
 * CLOCKS AND THE sck -> 25 ns CONVERSION
 * --------------------------------------
 * KryoFlux measures flux intervals in "sample clock" (sck) ticks. The master
 * clock is mck = ((18432000 * 73) / 14) / 2 = 48054857.142857 Hz; the sample
 * clock is sck = mck / 2 = 24027428.571428 Hz and the index clock is
 * ick = mck / 16 = 3003428.571428 Hz. A KFInfo OOB block may carry explicit
 * "sck="/"ick=" values, which override the defaults.
 *
 * SCP flux words are in 25 ns ticks (we set the SCP resolution byte to 0, so
 * one tick = 25 ns * (0 + 1) = 25 ns, matching the IPF mirror). One sck tick
 * lasts 1e9 / sck ns, so:
 *
 *     ticks_25ns = sck_ticks * (1e9 / 25) / sck_hz
 *                = sck_ticks * 40000000 / sck_hz
 *
 * For the DEFAULT sck this is the EXACT rational 4375 / 2628, because
 *     40000000 / 24027428.571428  =  40000000 * 56 / 1345536000  =  4375 / 2628.
 * Worked example: a nominal 2 us DD flux interval is 2000 ns / (1e9/sck) =
 * ~48.05 sck ticks; 48 * 4375/2628 = 79.909 -> rounds to 80, and 80 * 25 ns =
 * 2000 ns = 2 us, exactly the decoder's nominal 80-tick half-cell. We round to
 * nearest and difference cumulative times (see kryoflux_stream.cpp) so the
 * per-revolution total stays drift-free.
 */
#ifndef KONCPC_KRYOFLUX_STREAM_H
#define KONCPC_KRYOFLUX_STREAM_H

#include <cstddef>
#include <cstdint>
#include <vector>

/* Error codes (negative). On any error the output is left empty. */
enum : std::int8_t {
  KFSTREAM_E_TRUNCATED = -1, /* a flux/OOB block runs past the buffer end     */
  KFSTREAM_E_NO_INDEX = -2,  /* < 2 index pulses -> no complete revolution     */
  KFSTREAM_E_NO_FLUX = -3,   /* no flux transitions / no members               */
  KFSTREAM_E_BAD_OOB = -4,   /* malformed OOB header (e.g. short Index block)   */
  KFSTREAM_E_GEOMETRY = -5,  /* cyl/side maps outside the 168-slot SCP table    */
};

/* Default KryoFlux clocks, in Hz (used unless a KFInfo block overrides sck). */
constexpr double KFSTREAM_DEFAULT_SCK_HZ = 24027428.571428571;
constexpr double KFSTREAM_DEFAULT_ICK_HZ = 3003428.5714285714;

/* Convert an sck-tick count to SCP 25 ns ticks, rounded to nearest. Pure and
 * side-effect free so tests can pin the arithmetic with concrete numbers. */
uint32_t kryoflux_sck_to_25ns(uint64_t sck_ticks, double sck_hz);

/* One decoded revolution, expressed in SCP 25 ns ticks. `flux_25ns[k]` is the
 * interval from the previous transition (from the index pulse for k == 0) to
 * transition k; a value may exceed 0xFFFF (the SCP writer splits it into
 * 0x0000 carry words). `duration_25ns` is the index-to-index revolution time. */
struct KryoFluxRev {
  std::vector<uint32_t> flux_25ns;
  uint32_t duration_25ns = 0;
};

/* One decoded track (one stream / one .raw file). */
struct KryoFluxTrack {
  std::vector<KryoFluxRev> revs;             /* one entry per complete rev     */
  double sck_hz = KFSTREAM_DEFAULT_SCK_HZ;   /* effective sck (KFInfo or def.)  */
};

/* Decode a SINGLE KryoFlux stream (one track) into per-revolution flux.
 * Returns 0 on success or a negative KFSTREAM_E_* code. */
int kryoflux_decode_stream(const uint8_t* data, size_t len, KryoFluxTrack& out);

/* Transcode a SINGLE stream (one track) into an SCP image, placing the track at
 * SCP slot = cyl*2 + side. Note: the flux decoder is side-0 only, so side-1
 * (odd) slots are stored but not decoded by src/hw/flux — matching the
 * documented single-sided-consumption limit. Returns 0 or a negative code. */
int kryoflux_stream_to_scp(const uint8_t* data, size_t len, uint8_t cyl,
                           uint8_t side, std::vector<uint8_t>& out);

/* One member of a multi-track dump: the raw bytes of a single per-track stream
 * plus its physical location. */
struct KryoFluxMember {
  const uint8_t* data = nullptr;
  size_t len = 0;
  uint8_t cyl = 0;
  uint8_t side = 0;
};

/* Assemble a multi-track SCP from a set of per-track streams. Every track is
 * normalized to the same revolution count (the minimum across all members, as
 * SCP carries one global revolution count). Returns 0 or a negative code. */
int kryoflux_streams_to_scp(const std::vector<KryoFluxMember>& members,
                            std::vector<uint8_t>& out);

#endif  // KONCPC_KRYOFLUX_STREAM_H
