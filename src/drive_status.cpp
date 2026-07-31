#include "drive_status.h"

#include <filesystem>
#include <sstream>

#include "flux_save.h"
#include "hw/fdc.h"
#include "hw_views.h"
#include "koncepcja.h"
#include "subcycle_bridge.h"

extern t_CPC CPC;
extern t_FDC FDC;
extern t_drive driveA;
extern t_drive driveB;

// Extract just the filename from the slot file path
namespace {
std::string image_basename(const std::string& path) {
  if (path.empty()) return "";
  return std::filesystem::path(path).filename().string();
}
}  // namespace

DriveMedium drive_medium_from(const FluxSaveCaps& caps, unsigned sector_tracks,
                              unsigned sector_sides, int flux_ntracks) {
  DriveMedium m;
  if (!caps.present) return m;  // empty drive: t_drive may hold stale geometry

  m.present = true;
  m.flux = caps.can_scp;  // .scp/.hfe export ⇔ the medium is flux-backed
  if (m.flux && sector_tracks == 0) {
    // Flux with no sector view: the cylinder count comes from the flux medium,
    // and the FDC captures side 0 only, so the disc is single-sided here.
    m.tracks = flux_ntracks > 0 ? static_cast<unsigned>(flux_ntracks) : 0;
    m.sides = 1;
  } else {
    m.tracks = sector_tracks;
    m.sides = sector_sides;
  }
  return m;
}

DriveMedium drive_medium(int unit) {
  const t_drive& d = (unit & 1) != 0 ? driveB : driveA;
  int ntracks = 0;
  if ((unit & 1) == 0) {  // flux is drive-A-only
    if (const Device* fdc = subcycle_bridge_fdc()) {
      fdc_media_track_dirty(fdc, ntracks);
    }
  }
  return drive_medium_from(flux_save_caps(unit), d.tracks, d.sides, ntracks);
}

std::string emulator_status_summary() {
  std::ostringstream oss;
  oss << "paused=" << (CPC.paused ? 1 : 0) << " model=" << CPC.model
      << " speed=" << CPC.speed;
  return oss.str();
}

std::string drive_status_summary() {
  std::ostringstream oss;
  std::string const imgA = image_basename(CPC.driveA.file);
  std::string const imgB = image_basename(CPC.driveB.file);

  oss << "driveA:"
      << " motor=" << FDC.motor << " track=" << driveA.current_track
      << " side=" << driveA.current_side << " image=" << imgA
      << " wp=" << driveA.write_protected << "\n";
  oss << "driveB:"
      << " motor=" << FDC.motor << " track=" << driveB.current_track
      << " side=" << driveB.current_side << " image=" << imgB
      << " wp=" << driveB.write_protected;
  return oss.str();
}

std::string drive_status_detailed() {
  std::ostringstream oss;
  std::string const imgA = image_basename(CPC.driveA.file);
  std::string const imgB = image_basename(CPC.driveB.file);

  // Geometry comes from the medium: a flux-backed disc has no sector view,
  // so reading driveX.tracks here reports an empty drive for a mounted disc.
  const DriveMedium medA = drive_medium(0);
  const DriveMedium medB = drive_medium(1);

  oss << "drive=A"
      << " motor=" << FDC.motor << " track=" << driveA.current_track
      << " side=" << driveA.current_side
      << " present=" << (medA.present ? 1 : 0)
      << " flux=" << (medA.flux ? 1 : 0) << " tracks=" << medA.tracks
      << " sides=" << medA.sides << " image=" << imgA
      << " write_protected=" << driveA.write_protected
      << " altered=" << (driveA.altered ? 1 : 0) << "\n";
  oss << "drive=B"
      << " motor=" << FDC.motor << " track=" << driveB.current_track
      << " side=" << driveB.current_side
      << " present=" << (medB.present ? 1 : 0)
      << " flux=" << (medB.flux ? 1 : 0) << " tracks=" << medB.tracks
      << " sides=" << medB.sides << " image=" << imgB
      << " write_protected=" << driveB.write_protected
      << " altered=" << (driveB.altered ? 1 : 0);
  return oss.str();
}
