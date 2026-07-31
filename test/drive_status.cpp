#include "drive_status.h"

#include <gtest/gtest.h>

#include "flux_save.h"
#include "hw_views.h"
#include "koncepcja.h"

extern t_CPC CPC;
extern t_FDC FDC;
extern t_drive driveA;
extern t_drive driveB;

class DriveStatusTest : public testing::Test {
 protected:
  void SetUp() override {
    // Reset emulator state
    CPC.paused = false;
    CPC.model = 2;
    CPC.speed = 4;
    CPC.driveA.file = "";
    CPC.driveB.file = "";

    // Reset FDC
    memset(&FDC, 0, sizeof(FDC));

    // Reset drives
    memset(&driveA, 0, sizeof(driveA));
    memset(&driveB, 0, sizeof(driveB));
  }
};

TEST_F(DriveStatusTest, EmulatorStatusFormat) {
  CPC.paused = false;
  CPC.model = 2;
  CPC.speed = 4;
  auto s = emulator_status_summary();
  EXPECT_EQ(s, "paused=0 model=2 speed=4");
}

TEST_F(DriveStatusTest, EmulatorStatusPaused) {
  CPC.paused = true;
  CPC.model = 0;
  CPC.speed = 8;
  auto s = emulator_status_summary();
  EXPECT_EQ(s, "paused=1 model=0 speed=8");
}

TEST_F(DriveStatusTest, DriveStatusNoDisc) {
  auto s = drive_status_summary();
  EXPECT_EQ(s,
            "driveA: motor=0 track=0 side=0 image= wp=0\n"
            "driveB: motor=0 track=0 side=0 image= wp=0");
}

TEST_F(DriveStatusTest, DriveStatusWithDisc) {
  CPC.driveA.file = "/path/to/game.dsk";
  driveA.tracks = 42;
  driveA.sides = 1;
  driveA.current_track = 12;
  driveA.current_side = 0;
  driveA.write_protected = 0;
  FDC.motor = 1;

  auto s = drive_status_summary();
  EXPECT_EQ(s,
            "driveA: motor=1 track=12 side=0 image=game.dsk wp=0\n"
            "driveB: motor=1 track=0 side=0 image= wp=0");
}

TEST_F(DriveStatusTest, MotorStateReporting) {
  FDC.motor = 0;
  auto s = drive_status_summary();
  EXPECT_EQ(s,
            "driveA: motor=0 track=0 side=0 image= wp=0\n"
            "driveB: motor=0 track=0 side=0 image= wp=0");

  FDC.motor = 1;
  s = drive_status_summary();
  EXPECT_EQ(s,
            "driveA: motor=1 track=0 side=0 image= wp=0\n"
            "driveB: motor=1 track=0 side=0 image= wp=0");
}

TEST_F(DriveStatusTest, WriteProtectedFlag) {
  driveA.write_protected = 1;
  auto s = drive_status_summary();
  EXPECT_EQ(s,
            "driveA: motor=0 track=0 side=0 image= wp=1\n"
            "driveB: motor=0 track=0 side=0 image= wp=0");
}

TEST_F(DriveStatusTest, DetailedDriveStatusNoDisc) {
  auto s = drive_status_detailed();
  EXPECT_EQ(s,
            "drive=A motor=0 track=0 side=0 present=0 flux=0 tracks=0 sides=0 "
            "image= write_protected=0 altered=0\n"
            "drive=B motor=0 track=0 side=0 present=0 flux=0 tracks=0 sides=0 "
            "image= write_protected=0 altered=0");
}

// present/flux/tracks/sides come from the FDC medium, which these tests have
// no way to mount — writing driveA.tracks does NOT insert a disc, and reading
// it back as if it did is the mistake that made flux discs show "(no disk)".
// The decision itself is covered by the drive_medium_from cases below.
TEST_F(DriveStatusTest, DetailedDriveStatusReportsTheHostFieldsItOwns) {
  CPC.driveA.file = "/games/roland.dsk";
  driveA.tracks = 40;  // a sector view with no medium behind it
  driveA.sides = 2;
  driveA.current_track = 5;
  driveA.current_side = 1;
  driveA.write_protected = 1;
  driveA.altered = true;
  FDC.motor = 1;

  auto s = drive_status_detailed();
  EXPECT_EQ(s,
            "drive=A motor=1 track=5 side=1 present=0 flux=0 tracks=0 sides=0 "
            "image=roland.dsk write_protected=1 altered=1\n"
            "drive=B motor=1 track=0 side=0 present=0 flux=0 tracks=0 sides=0 "
            "image= write_protected=0 altered=0");
}

TEST_F(DriveStatusTest, DetailedBothDrives) {
  CPC.driveA.file = "/path/disc1.dsk";
  CPC.driveB.file = "/path/disc2.dsk";
  driveA.tracks = 42;
  driveA.sides = 1;
  driveB.tracks = 80;
  driveB.sides = 2;
  driveB.current_track = 7;
  driveB.write_protected = 1;
  driveB.altered = false;

  auto s = drive_status_detailed();
  EXPECT_NE(s.find("drive=A"), std::string::npos);
  EXPECT_NE(s.find("image=disc1.dsk"), std::string::npos);
  EXPECT_NE(s.find("drive=B"), std::string::npos);
  EXPECT_NE(s.find("image=disc2.dsk"), std::string::npos);
  EXPECT_NE(s.find("write_protected=1"), std::string::npos);
}

// ─────────────────────────────────────────────────
// drive_medium_from — what is in the drive
// ─────────────────────────────────────────────────

TEST(DriveMediumTest, SectorDiscKeepsItsOwnGeometry) {
  FluxSaveCaps caps;
  caps.present = true;
  caps.can_dsk = true;

  const DriveMedium m = drive_medium_from(caps, 40, 2, 0);
  EXPECT_TRUE(m.present);
  EXPECT_FALSE(m.flux);
  EXPECT_EQ(m.tracks, 40u);
  EXPECT_EQ(m.sides, 2u);
}

// The bug: a mounted .hfe/.scp/.a2r has no sector view at all, so every gate
// that read t_drive::tracks called it an empty drive — "(no disk)" in the
// status bar, no track readout, and a click offering Load instead of Eject.
TEST(DriveMediumTest, FluxDiscIsPresentDespiteAnEmptySectorView) {
  FluxSaveCaps caps;
  caps.present = true;
  caps.can_scp = true;
  caps.can_hfe = true;

  const DriveMedium m = drive_medium_from(caps, 0, 0, 80);
  EXPECT_TRUE(m.present) << "a flux disc read as an empty drive";
  EXPECT_TRUE(m.flux);
  EXPECT_EQ(m.tracks, 80u) << "cylinder count comes from the flux medium";
  EXPECT_EQ(m.sides, 1u) << "the FDC captures side 0 only";
}

// A written flux disc grows a DSK overlay, so it reports both capabilities.
// The overlay's geometry is real and wins over the flux cylinder count.
TEST(DriveMediumTest, WrittenFluxDiscPrefersTheOverlayGeometry) {
  FluxSaveCaps caps;
  caps.present = true;
  caps.can_dsk = true;
  caps.can_scp = true;

  const DriveMedium m = drive_medium_from(caps, 42, 1, 80);
  EXPECT_TRUE(m.present);
  EXPECT_TRUE(m.flux);
  EXPECT_EQ(m.tracks, 42u);
  EXPECT_EQ(m.sides, 1u);
}

// t_drive is not cleared the instant a disc leaves, so trusting its geometry
// would keep reporting the disc that is no longer there.
TEST(DriveMediumTest, EmptyDriveIgnoresStaleSectorGeometry) {
  const DriveMedium m = drive_medium_from(FluxSaveCaps{}, 40, 2, 80);
  EXPECT_FALSE(m.present);
  EXPECT_FALSE(m.flux);
  EXPECT_EQ(m.tracks, 0u);
  EXPECT_EQ(m.sides, 0u);
}

// A flux medium whose cylinder count is unavailable is still a disc.
TEST(DriveMediumTest, FluxDiscWithUnknownCylinderCountIsStillPresent) {
  FluxSaveCaps caps;
  caps.present = true;
  caps.can_scp = true;

  const DriveMedium m = drive_medium_from(caps, 0, 0, 0);
  EXPECT_TRUE(m.present);
  EXPECT_EQ(m.tracks, 0u) << "unknown, not fabricated";
}
