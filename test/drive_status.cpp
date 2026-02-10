#include <gtest/gtest.h>

#include "koncepcja.h"
#include "disk.h"
#include "drive_status.h"

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
  EXPECT_NE(s.find("driveA: motor=0 track=0 side=0 image= wp=0"), std::string::npos);
  EXPECT_NE(s.find("driveB: motor=0 track=0 side=0 image= wp=0"), std::string::npos);
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
  EXPECT_NE(s.find("driveA: motor=1 track=12 side=0 image=game.dsk wp=0"), std::string::npos);
}

TEST_F(DriveStatusTest, MotorStateReporting) {
  FDC.motor = 0;
  auto s = drive_status_summary();
  EXPECT_NE(s.find("motor=0"), std::string::npos);

  FDC.motor = 1;
  s = drive_status_summary();
  EXPECT_NE(s.find("motor=1"), std::string::npos);
}

TEST_F(DriveStatusTest, WriteProtectedFlag) {
  driveA.write_protected = 1;
  auto s = drive_status_summary();
  EXPECT_NE(s.find("driveA: motor=0 track=0 side=0 image= wp=1"), std::string::npos);
}

TEST_F(DriveStatusTest, DetailedDriveStatusNoDisc) {
  auto s = drive_status_detailed();
  EXPECT_NE(s.find("drive=A motor=0 track=0 side=0 tracks=0 sides=0 image= write_protected=0 altered=0"), std::string::npos);
  EXPECT_NE(s.find("drive=B motor=0 track=0 side=0 tracks=0 sides=0 image= write_protected=0 altered=0"), std::string::npos);
}

TEST_F(DriveStatusTest, DetailedDriveStatusWithDisc) {
  CPC.driveA.file = "/games/roland.dsk";
  driveA.tracks = 40;
  driveA.sides = 2;
  driveA.current_track = 5;
  driveA.current_side = 1;
  driveA.write_protected = 1;
  driveA.altered = true;
  FDC.motor = 1;

  auto s = drive_status_detailed();
  EXPECT_NE(s.find("drive=A motor=1 track=5 side=1 tracks=40 sides=2 image=roland.dsk write_protected=1 altered=1"), std::string::npos);
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
  EXPECT_NE(s.find("tracks=80 sides=2"), std::string::npos);
  EXPECT_NE(s.find("write_protected=1"), std::string::npos);
}
