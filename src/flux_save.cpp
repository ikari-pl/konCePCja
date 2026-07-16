/* flux_save.cpp — Stage 4 Save-As core (see flux_save.h). Wires the live FDC
 * medium to the Stage-1 encoders and writes the chosen container. */
#include "flux_save.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "hfe.h"          // HFE_E_* codes for the error string
#include "hfe_write.h"    // hfe_from_disk
#include "hw/device.h"    // Device
#include "hw/fdc.h"       // fdc_media_* accessors
#include "scp_write.h"  // scp_from_disk
#include "subcycle_bridge.h"  // subcycle_bridge_fdc

namespace {

// A flux-backed medium carries a pristine SCP; a sector-backed one does not.
bool medium_is_flux(const std::uint8_t* scp, std::size_t scp_len) {
  return scp != nullptr && scp_len > 0;
}

// A read-only flux dump (no overlay) has no per-track dirty map, so treat every
// track as clean → the encoders splice the original flux verbatim.
int effective_ntracks(const bool* track_dirty, int ntracks) {
  return track_dirty != nullptr ? ntracks : 0;
}

std::vector<std::uint8_t> save_as_dsk(const std::uint8_t* image,
                                      std::size_t image_len, std::string& err) {
  if (image == nullptr || image_len == 0) {
    err = "no writable disk image to save (read-only flux dump)";
    return {};
  }
  std::vector<std::uint8_t> out(image, image + image_len);
  return out;
}

std::vector<std::uint8_t> save_as_scp(const std::uint8_t* scp,
                                      std::size_t scp_len,
                                      const std::uint8_t* image,
                                      std::size_t image_len,
                                      const bool* track_dirty, int ntracks,
                                      std::string& err) {
  if (!medium_is_flux(scp, scp_len)) {
    err = "SCP export needs a flux-backed disc (this one is sector-backed)";
    return {};
  }
  std::vector<std::uint8_t> out = scp_from_disk(
      scp, scp_len, image, image_len, track_dirty,
      effective_ntracks(track_dirty, ntracks));
  if (out.empty()) err = "SCP export failed (unusable flux/disk pairing)";
  return out;
}

const char* hfe_error_text(int code) {
  switch (code) {
    case HFE_E_NOT_HFE:
      return "not an HFE image";
    case HFE_E_UNSUPPORTED:
      return "unsupported HFE variant";
    case HFE_E_TRUNCATED:
      return "truncated HFE data";
    case HFE_E_GEOMETRY:
      return "bad track geometry for HFE";
    default:
      return "HFE encode error";
  }
}

std::vector<std::uint8_t> save_as_hfe(const std::uint8_t* scp,
                                      std::size_t scp_len,
                                      const std::uint8_t* image,
                                      std::size_t image_len,
                                      const bool* track_dirty, int ntracks,
                                      std::string& err) {
  if (!medium_is_flux(scp, scp_len)) {
    err = "HFE export needs a flux-backed disc (this one is sector-backed)";
    return {};
  }
  std::vector<std::uint8_t> out;
  const int code = hfe_from_disk(scp, scp_len, image, image_len, track_dirty,
                                 effective_ntracks(track_dirty, ntracks), out);
  if (code != 0) {
    err = std::string("HFE export failed: ") + hfe_error_text(code);
    out.clear();
  }
  return out;
}

}  // namespace

std::vector<std::uint8_t> flux_save_bytes_from_medium(
    const std::uint8_t* scp, std::size_t scp_len, const std::uint8_t* image,
    std::size_t image_len, const bool* track_dirty, int ntracks,
    SaveFormat fmt, std::string& err) {
  err.clear();
  switch (fmt) {
    case SaveFormat::Dsk:
      return save_as_dsk(image, image_len, err);
    case SaveFormat::Scp:
      return save_as_scp(scp, scp_len, image, image_len, track_dirty, ntracks,
                         err);
    case SaveFormat::Hfe:
      return save_as_hfe(scp, scp_len, image, image_len, track_dirty, ntracks,
                         err);
  }
  err = "unknown save format";
  return {};
}

FluxSaveCaps flux_save_caps_dev(const Device* fdc, std::uint8_t unit) {
  FluxSaveCaps caps;
  if (fdc == nullptr) return caps;
  std::size_t image_len = 0;
  const std::uint8_t* image = fdc_media_image_unit(fdc, unit, image_len);
  const std::uint8_t* scp = nullptr;
  std::size_t scp_len = 0;
  if ((unit & 1u) == 0) scp = fdc_media_flux_scp(fdc, scp_len);  // flux = drive A
  const bool flux = medium_is_flux(scp, scp_len);
  caps.can_dsk = image != nullptr && image_len > 0;
  caps.can_scp = flux;
  caps.can_hfe = flux;
  caps.present = caps.can_dsk || flux;
  return caps;
}

std::vector<std::uint8_t> flux_save_bytes_dev(const Device* fdc,
                                              std::uint8_t unit, SaveFormat fmt,
                                              std::string& err) {
  if (fdc == nullptr) {
    err = "no drive medium (engine inactive)";
    return {};
  }
  std::size_t image_len = 0;
  const std::uint8_t* image = fdc_media_image_unit(fdc, unit, image_len);
  const std::uint8_t* scp = nullptr;
  std::size_t scp_len = 0;
  const bool* track_dirty = nullptr;
  int ntracks = 0;
  if ((unit & 1u) == 0) {  // flux is drive-A-only; drive B is always sector
    scp = fdc_media_flux_scp(fdc, scp_len);
    track_dirty = fdc_media_track_dirty(fdc, ntracks);
  }
  return flux_save_bytes_from_medium(scp, scp_len, image, image_len, track_dirty,
                                     ntracks, fmt, err);
}

bool flux_write_file(const std::vector<std::uint8_t>& bytes,
                     const std::string& path, std::string& err) {
  if (bytes.empty()) {
    err = "nothing to write (empty image)";
    return false;
  }
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    err = "cannot open '" + path + "' for writing";
    return false;
  }
  const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), file) ==
                     bytes.size();
  const bool flushed = std::fflush(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!wrote || !flushed || !closed) {
    err = "write error for '" + path + "' (disk full or I/O error)";
    return false;
  }
  return true;
}

std::vector<std::uint8_t> flux_save_bytes(int unit, SaveFormat fmt,
                                          std::string& err) {
  return flux_save_bytes_dev(subcycle_bridge_fdc(),
                             static_cast<std::uint8_t>(unit & 1), fmt, err);
}

FluxSaveCaps flux_save_caps(int unit) {
  return flux_save_caps_dev(subcycle_bridge_fdc(),
                            static_cast<std::uint8_t>(unit & 1));
}

bool flux_save_to_file(int unit, SaveFormat fmt, const std::string& path,
                       std::string& err) {
  const std::vector<std::uint8_t> bytes = flux_save_bytes(unit, fmt, err);
  if (bytes.empty()) {
    if (err.empty()) err = "no bytes to save";
    return false;
  }
  return flux_write_file(bytes, path, err);
}
