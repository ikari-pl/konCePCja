// beads-p5t: Disc Tools kept listing the previous disc after a swap, because
// nothing told it the drive's medium had changed. dsk_media_generation() is
// that signal: it must move on every load and eject of drive A/B, and must NOT
// move on the pull path, which re-reads the same medium every time Disc Tools
// refreshes (moving there would make the listing rebuild every frame).

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "disk_file_editor.h"
#include "disk_format.h"
#include "koncepcja.h"
#include "slotshandler.h"

extern t_drive driveA;
extern t_drive driveB;

namespace {

class DiscMediaGenerationTest : public testing::Test {
 protected:
  void SetUp() override {
    dsk_eject_host(&driveA);
    dsk_eject_host(&driveB);
    dir_ = std::filesystem::temp_directory_path() / "koncepcja-media-gen-test";
    std::filesystem::create_directories(dir_);
    // A formatted data disc on disk, to load.
    ASSERT_EQ("", disk_format_drive('A', "data"));
    dsk_path_ = (dir_ / "blank.dsk").string();
    ASSERT_EQ(0, dsk_save(dsk_path_, &driveA));
    dsk_eject_host(&driveA);
  }

  void TearDown() override {
    dsk_eject_host(&driveA);
    dsk_eject_host(&driveB);
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::string dsk_path_;
};

}  // namespace

TEST_F(DiscMediaGenerationTest, LoadingADiscMovesThatDrivesGeneration) {
  uint64_t const a0 = dsk_media_generation(0);
  uint64_t const b0 = dsk_media_generation(1);
  ASSERT_EQ(0, dsk_load(dsk_path_, &driveA));
  EXPECT_NE(a0, dsk_media_generation(0));
  EXPECT_EQ(b0, dsk_media_generation(1)) << "drive B's listing is still good";
}

TEST_F(DiscMediaGenerationTest, EjectMovesTheGeneration) {
  ASSERT_EQ(0, dsk_load(dsk_path_, &driveB));
  uint64_t const b0 = dsk_media_generation(1);
  dsk_eject(&driveB);
  EXPECT_NE(b0, dsk_media_generation(1));
}

TEST_F(DiscMediaGenerationTest, FileLoadMovesTheGeneration) {
  // The GUI, IPC 'load', drag-and-drop and the command line all go through
  // file_load. It must signal even for loads that never pass dsk_eject (a
  // .dsk inside a .zip is parsed from a FILE*).
  t_slot slot{};
  slot.drive = DRIVE::DSK_B;
  slot.file = dsk_path_;
  uint64_t const b0 = dsk_media_generation(1);
  ASSERT_EQ(0, file_load(slot));
  EXPECT_NE(b0, dsk_media_generation(1));
}

TEST_F(DiscMediaGenerationTest, PullingTheSameMediumDoesNotMoveIt) {
  ASSERT_EQ(0, dsk_load(dsk_path_, &driveA));
  std::vector<uint8_t> bytes;
  ASSERT_EQ(0, dsk_to_bytes(&driveA, bytes));
  uint64_t const a0 = dsk_media_generation(0);
  // What subcycle_bridge_pull_drive_view does on every Disc Tools refresh.
  ASSERT_EQ(0, dsk_load_bytes(bytes.data(), bytes.size(), &driveA));
  dsk_eject_host(&driveA);
  EXPECT_EQ(a0, dsk_media_generation(0))
      << "the pull path re-reads the same disc; moving the generation here "
         "would rebuild the Disc Tools listing every frame";
}
