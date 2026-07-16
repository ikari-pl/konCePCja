/* scp_write.h — SuperCard Pro flux WRITE-back (the inverse of hw/flux.cpp's
 * read path). See docs/hardware/flux-media.md and beads-l5q5.
 *
 * Building a written-to flux image from a (possibly modified) DSK plus the
 * original flux capture: CLEAN tracks are spliced verbatim out of the
 * original SCP so weak/fuzzy bits and real gap geometry survive untouched;
 * DIRTY tracks are re-synthesized as standard IBM System 34 MFM from the DSK
 * (mfm_encode_track -> the same flux-word emission scp_from_mfm_tracks uses).
 * A New flux disk (no original capture) synthesizes every present DSK track.
 *
 * Side-0 single-sided geometry throughout, matching the sub-cycle FDC's flux
 * cache (docs/hardware/flux-ingestion-contract.md §1). Pure function,
 * std::vector-owned output. */
#ifndef KONCPC_SCP_WRITE_H
#define KONCPC_SCP_WRITE_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Assemble an SCP flux image from `dsk` (the current sector image) and, when
// present, `orig_scp` (the flux the disc was loaded from). `track_dirty` has
// `ntracks` entries — cylinder N is re-synthesized from the DSK when
// track_dirty[N] is true, otherwise its original flux is copied verbatim.
//
// - orig_scp == nullptr / orig_len == 0 (a New flux disc): every present DSK
//   track is synthesized (one revolution).
// - No track dirty and orig_scp present: a byte-for-byte copy of orig_scp.
// - Mixed: the output carries the original capture's revolution count; a
//   synthesized track repeats its single deterministic revolution to match.
//
// Returns an empty vector on unusable input (no DSK tracks and no original).
std::vector<uint8_t> scp_from_disk(const uint8_t* orig_scp, std::size_t orig_len,
                                   const uint8_t* dsk, std::size_t dsk_len,
                                   const bool* track_dirty, int ntracks);

#endif  // KONCPC_SCP_WRITE_H
