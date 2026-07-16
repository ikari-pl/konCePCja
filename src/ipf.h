#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hw_views.h"

int ipf_load(FILE* /*pfileIn*/, t_drive* /*drive*/);
int ipf_load(const std::string& /*filename*/, t_drive* /*drive*/);

// One captured revolution of one cylinder: MFM bitcells, packed MSB-first.
struct t_mfm_rev {
  std::vector<uint8_t> bits;
  uint32_t nbits = 0;
};
using t_mfm_track = std::vector<t_mfm_rev>;  // one entry per revolution

// Assemble an in-memory SuperCard Pro container (side 0, 25 ns ticks, 2 µs
// DD bitcells) from per-cylinder MFM captures: a transition on every '1'
// bitcell IS the flux timeline. Cylinders with no revolutions become absent
// (unformatted) SCP slots; every present cylinder must carry the same
// revolution count. Empty on bad geometry. Fed by the clean IPF decoder's
// side-0 flux mirror (ipf::Image::mirror_side0), hfe_to_scp, and mfm_encode.
std::vector<uint8_t> scp_from_mfm_tracks(const std::vector<t_mfm_track>& cyls);
