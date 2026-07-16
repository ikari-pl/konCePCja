#pragma once

#include <cstdint>
#include <string>
#include <vector>

// How a newly created disc is backed. Sector = a plain blank .dsk (small,
// universal, no weak-bit capability). Flux = a blank standard DSK made
// flux-backed via a synthesized .scp, so FDC writes are preserved and the disc
// can export to real hardware (.hfe) and .scp — the backing is chosen at
// creation because flux you don't keep can't be reconstructed later.
enum class DiskBacking : std::uint8_t { Sector, Flux };

// Look up a disk format index by short name (e.g. "data", "vendor").
// Returns -1 if the name is not recognized.
int disk_format_index_by_name(const std::string& name);

// Return a list of recognized short format names.
std::vector<std::string> disk_format_names();

// Create a new blank formatted disk at the given path.
//   format_name: short name like "data" or "vendor".
//   backing:     Sector writes a blank .dsk; Flux synthesizes a blank .scp
//                (flux-backed, writable) from the blank DSK and writes THAT.
//   out_bytes:   when non-null, receives the bytes written (the DSK for Sector,
//                the SCP for Flux) so the caller can insert them into a live
//                drive without re-reading the file.
// Returns empty string on success, error message on failure.
std::string disk_create_new(const std::string& path,
                            const std::string& format_name = "data",
                            DiskBacking backing = DiskBacking::Sector,
                            std::vector<uint8_t>* out_bytes = nullptr);

// Format (or re-format) the disc currently in the given drive.
// drive_letter: 'A' or 'B'.
// format_name: short name like "data" or "vendor".
// Returns empty string on success, error message on failure.
std::string disk_format_drive(char drive_letter,
                              const std::string& format_name);
