#pragma once

#include <string>

struct FluxSaveCaps;

// What is actually in a drive.
//
// The FDC medium is the disc. t_drive is the host's *sector* view of it, and a
// flux-backed medium (.hfe/.scp/.a2r) has no sector view at all — driveA.tracks
// reads 0 for a perfectly good disc. Every gate that asks "is a disc in there?"
// must come through here rather than reading t_drive directly.
struct DriveMedium {
  bool present = false;  // a disc is in the drive
  bool flux = false;     // flux-backed (no sector view; .scp/.hfe export)
  unsigned tracks = 0;   // cylinder count, 0 when unknown
  unsigned sides = 0;    // 1 for flux — the FDC captures side 0 only
};

// Pure decision: describe the medium `caps` reports, filling in the geometry
// from whichever view holds it. A sector-backed disc knows its own tracks and
// sides; a flux-backed one has neither, so its cylinder count comes from
// `flux_ntracks` (fdc_media_track_dirty) and it is single-sided by
// construction. No disc in the drive means no geometry, whatever t_drive
// happens to still hold.
DriveMedium drive_medium_from(const FluxSaveCaps& caps, unsigned sector_tracks,
                              unsigned sector_sides, int flux_ntracks);

// The live medium of drive `unit` (0=A, 1=B).
DriveMedium drive_medium(int unit);

// Brief one-line-per-drive summary (for `status` command)
std::string drive_status_summary();

// Detailed multi-line per-drive output (for `status drives` command)
std::string drive_status_detailed();

// Overall emulator state line (paused, model, speed)
std::string emulator_status_summary();
