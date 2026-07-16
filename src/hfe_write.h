/* hfe_write.h — HxC "HFE" v1 WRITE-back: the byte-exact inverse of
 * src/hfe.cpp's hfe_to_scp decoder. See beads-ly1p and hfe.h for the format
 * provenance.
 *
 * Per-cylinder side-0 MFM bitcells (t_mfm_rev, MSb-first) are packed back into
 * HFE v1's on-disk form: a 512-byte header, a 512-byte track lookup table,
 * then repeating 512-byte blocks of 256 bytes side 0 followed by 256 bytes of
 * side-1 filler, bytes transmitted LSb-first, at the CPC/IBM double-density
 * bitRate of 250 kbit/s. hfe_to_scp(hfe_from_mfm_tracks(cyls)) reproduces
 * scp_from_mfm_tracks(cyls) exactly.
 *
 * HFE v1 stores ONE fixed bitstream per track with no weak-bit / multi-
 * revolution variation (that is HFE v3, out of scope). So on the disk-level
 * path a clean track's weak bits necessarily collapse to its revolution 0 —
 * an inherent limitation of the container, not of this writer.
 *
 * Error codes are shared with the decoder (HFE_E_* in hfe.h). Clean-room:
 * written from the public HFE spec and hfe.cpp's observable contract only. */
#ifndef KONCPC_HFE_WRITE_H
#define KONCPC_HFE_WRITE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hfe.h"  // HFE_E_* error enum
#include "ipf.h"  // t_mfm_track

// Encode per-cylinder side-0 bitcells (revolution 0 of each) as an HFE v1
// image into `out`. Every present cylinder's bitcell count must be a whole
// number of bytes (a multiple of 8) — mfm_encode_track output always is.
// Returns 0 or a negative HFE_E_* code; `out` is left empty on failure.
int hfe_from_mfm_tracks(const std::vector<t_mfm_track>& cyls,
                        std::vector<uint8_t>& out);

// Disk-level WRITE-back: clean cylinders are reconstructed from the original
// flux capture, dirty cylinders are synthesized as standard MFM from the DSK,
// then encoded as one HFE v1 image. `track_dirty` has `ntracks` entries. With
// no original capture every present DSK track is synthesized. Returns 0 or a
// negative HFE_E_* code; `out` is left empty on failure.
int hfe_from_disk(const uint8_t* orig_scp, std::size_t orig_len,
                  const uint8_t* dsk, std::size_t dsk_len,
                  const bool* track_dirty, int ntracks, std::vector<uint8_t>& out);

#endif  // KONCPC_HFE_WRITE_H
