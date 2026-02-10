#include "drive_status.h"
#include "koncepcja.h"
#include "disk.h"

#include <filesystem>
#include <sstream>

extern t_CPC CPC;
extern t_FDC FDC;
extern t_drive driveA;
extern t_drive driveB;

// Extract just the filename from the slot file path
static std::string image_basename(const std::string& path) {
  if (path.empty()) return "";
  return std::filesystem::path(path).filename().string();
}

std::string emulator_status_summary() {
  std::ostringstream oss;
  oss << "paused=" << (CPC.paused ? 1 : 0)
      << " model=" << CPC.model
      << " speed=" << CPC.speed;
  return oss.str();
}

std::string drive_status_summary() {
  std::ostringstream oss;
  std::string imgA = image_basename(CPC.driveA.file);
  std::string imgB = image_basename(CPC.driveB.file);

  oss << "driveA:"
      << " motor=" << FDC.motor
      << " track=" << driveA.current_track
      << " side=" << driveA.current_side
      << " image=" << imgA
      << " wp=" << driveA.write_protected
      << "\n";
  oss << "driveB:"
      << " motor=" << FDC.motor
      << " track=" << driveB.current_track
      << " side=" << driveB.current_side
      << " image=" << imgB
      << " wp=" << driveB.write_protected;
  return oss.str();
}

std::string drive_status_detailed() {
  std::ostringstream oss;
  std::string imgA = image_basename(CPC.driveA.file);
  std::string imgB = image_basename(CPC.driveB.file);

  oss << "drive=A"
      << " motor=" << FDC.motor
      << " track=" << driveA.current_track
      << " side=" << driveA.current_side
      << " tracks=" << driveA.tracks
      << " sides=" << driveA.sides
      << " image=" << imgA
      << " write_protected=" << driveA.write_protected
      << " altered=" << (driveA.altered ? 1 : 0)
      << "\n";
  oss << "drive=B"
      << " motor=" << FDC.motor
      << " track=" << driveB.current_track
      << " side=" << driveB.current_side
      << " tracks=" << driveB.tracks
      << " sides=" << driveB.sides
      << " image=" << imgB
      << " write_protected=" << driveB.write_protected
      << " altered=" << (driveB.altered ? 1 : 0);
  return oss.str();
}
