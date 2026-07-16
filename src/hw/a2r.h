/* a2r.h — Applesauce A2R (v3 RWCP flux) → in-memory SCP transcoder.
 *
 * The flux decoder (src/hw/flux) speaks SCP only. A2R is the same *kind* of
 * artifact — raw flux transition timings — just a different container, so we
 * transcode rather than write a second decoder: parse the A2R3 RWCP capture and
 * emit an SCP byte image that flux_scp_* consumes unchanged.
 *
 * Only side-0 timing captures are emitted (the decoder is side-0 only). A2R's
 * tick is 125 ns; we set the SCP resolution byte to 4 (25 ns * (4+1) = 125 ns)
 * so the flux values copy across without rescaling and the decoder's nominal
 * half-cell (80/(res+1) = 16 ticks = 2 us) is physically correct for DD.
 *
 * This is a container transcode, NOT an encoding conversion: an Apple-GCR A2R
 * transcodes fine but still won't decode (the MFM decoder finds no sectors).
 */
#ifndef HW_A2R_H
#define HW_A2R_H

#include <cstddef>
#include <cstdint>
#include <vector>

enum : std::int8_t {
  A2R_E_NOT_A2R = -1,     /* missing "A2R3" signature                        */
  A2R_E_UNSUPPORTED = -2, /* A2R2/STRM or a non-timing (bit) capture only    */
  A2R_E_TRUNCATED = -3,   /* a chunk / capture entry runs past the buffer    */
  A2R_E_NO_FLUX = -4,     /* no usable side-0 timing capture found           */
};

/* Transcode `a2r`/`len` into `out` (an SCP image). Returns 0 on success or a
 * negative A2R_E_* code; on error `out` is left empty. */
int a2r_to_scp(const uint8_t* a2r, size_t len, std::vector<uint8_t>& out);

#endif  // HW_A2R_H
