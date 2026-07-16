#include "disk_format.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

#include "flux_save.h"    // flux_write_file
#include "ipf.h"          // scp_from_mfm_tracks, t_mfm_track
#include "koncepcja.h"
#include "log.h"
#include "mfm_encode.h"   // mfm_tracks_from_dsk
#include "slotshandler.h"

extern t_drive driveA;
extern t_drive driveB;
extern t_disk_format disk_format[];

// Short name -> disk_format[] index.
// Built-in formats are at indices 0 and 1; indices 2..MAX_DISK_FORMAT-1 are
// user-customisable and may or may not be populated.
static const std::map<std::string, int> builtin_format_names = {
    {"data", 0},
    {"vendor", 1},
};

namespace {
std::string to_lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}
}  // namespace

int disk_format_index_by_name(const std::string& name) {
  if (name.empty()) return -1;
  auto it = builtin_format_names.find(to_lower(name));
  if (it != builtin_format_names.end()) return it->second;

  // Also allow matching by full label (case-insensitive prefix match).
  std::string const lower_name = to_lower(name);
  for (int i = 0; i < MAX_DISK_FORMAT; i++) {
    if (disk_format[i].label.empty()) continue;
    std::string const lower_label = to_lower(disk_format[i].label);
    if (lower_label.find(lower_name) == 0) return i;
  }
  return -1;
}

std::vector<std::string> disk_format_names() {
  std::vector<std::string> names;
  // Return the short names for built-in formats.
  names.emplace_back("data");
  names.emplace_back("vendor");
  // Also include any populated custom formats by their label.
  for (int i = FIRST_CUSTOM_DISK_FORMAT; i < MAX_DISK_FORMAT; i++) {
    if (!disk_format[i].label.empty()) {
      names.push_back(disk_format[i].label);
    }
  }
  return names;
}

// Write a formatted drive as a plain blank .dsk; optionally hand back the bytes.
namespace {
std::string create_sector_disk(const std::string& path, t_drive* drive,
                                      std::vector<uint8_t>* out_bytes) {
  if (out_bytes != nullptr) dsk_to_bytes(drive, *out_bytes);
  if (dsk_save(path, drive) != 0) return "write error for " + path;
  return "";
}
}  // namespace

// Synthesize a flux-backed .scp from a blank formatted drive: DSK bytes ->
// standard IBM MFM tracks -> an SCP flux container. Writing THAT (not a .dsk)
// gives a disc that preserves writes and can export to .scp/.hfe.
namespace {
std::string create_flux_disk(const std::string& path, t_drive* drive,
                                    std::vector<uint8_t>* out_bytes) {
  std::vector<uint8_t> dsk;
  if (dsk_to_bytes(drive, dsk) != 0) return "format error: no tracks to encode";
  const std::vector<t_mfm_track> tracks =
      mfm_tracks_from_dsk(dsk.data(), dsk.size());
  if (tracks.empty()) return "flux synth error: could not encode blank DSK";
  std::vector<uint8_t> scp = scp_from_mfm_tracks(tracks);
  if (scp.empty()) return "flux synth error: could not build SCP container";
  std::string werr;
  if (!flux_write_file(scp, path, werr)) return werr;
  if (out_bytes != nullptr) *out_bytes = std::move(scp);
  return "";
}
}  // namespace

std::string disk_create_new(const std::string& path,
                            const std::string& format_name, DiskBacking backing,
                            std::vector<uint8_t>* out_bytes) {
  const int idx = disk_format_index_by_name(format_name);
  if (idx < 0) {
    return "unknown format: " + format_name;
  }

  // Create a temporary drive struct, format it, then serialize/save.
  t_drive tmp_drive{};
  const int rc = dsk_format(&tmp_drive, idx);
  if (rc != 0) {
    dsk_eject(&tmp_drive);
    return "format error code " + std::to_string(rc);
  }

  const std::string err = backing == DiskBacking::Flux
                              ? create_flux_disk(path, &tmp_drive, out_bytes)
                              : create_sector_disk(path, &tmp_drive, out_bytes);
  dsk_eject(&tmp_drive);  // free the formatted drive's allocated tracks
  return err;
}

std::string disk_format_drive(char drive_letter,
                              const std::string& format_name) {
  t_drive* drive = nullptr;
  char const upper =
      static_cast<char>(std::toupper(static_cast<unsigned char>(drive_letter)));
  if (upper == 'A') {
    drive = &driveA;
  } else if (upper == 'B') {
    drive = &driveB;
  } else {
    return "invalid drive letter: " + std::string(1, drive_letter);
  }

  int const idx = disk_format_index_by_name(format_name);
  if (idx < 0) {
    return "unknown format: " + format_name;
  }

  // Eject any existing disc content before formatting.
  dsk_eject(drive);

  int const rc = dsk_format(drive, idx);
  if (rc != 0) {
    return "format error code " + std::to_string(rc);
  }
  return "";
}
