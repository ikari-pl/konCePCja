// beads-p5t: Disc Tools kept listing the previous disc after a swap, because
// nothing told it the drive's medium had changed. dsk_media_generation() is
// that signal: it must move on every load and eject of drive A/B, and must NOT
// move on the pull path, which re-reads the same medium every time Disc Tools
// refreshes (moving there would make the listing rebuild every frame).

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "disk_file_editor.h"
#include "disk_format.h"
#include "koncepcja.h"
#include "slotshandler.h"
#include "subcycle_bridge.h"

extern t_CPC CPC;
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

// -----------------------------------------------
// With the board running: a load made while paused is only queued
// -----------------------------------------------

namespace {

// Brings the sub-cycle board up on a synthetic 32K system ROM (no firmware
// needed: nothing executes, the test only swaps media) and tears it down again,
// so no other test sees an active bridge.
class LiveBoardMediaTest : public DiscMediaGenerationTest {
 protected:
  void SetUp() override {
    DiscMediaGenerationTest::SetUp();
    saved_cpc_rom_path_ = CPC.rom_path;
    saved_model_ = CPC.model;
    saved_ram_ = CPC.ram_size;
    std::vector<char> rom(0x8000, 0);
    std::filesystem::path const rom_file = dir_ / "cpc6128.rom";
    FILE* f = fopen(rom_file.string().c_str(), "wb");
    ASSERT_NE(nullptr, f);
    ASSERT_EQ(rom.size(), fwrite(rom.data(), 1, rom.size(), f));
    ASSERT_EQ(0, fclose(f));
    CPC.rom_path = dir_.string();
    CPC.model = 2;  // chROMFile[2] == "cpc6128.rom"
    CPC.ram_size = 128;
    ASSERT_TRUE(subcycle_bridge_start());

    // Two discs to swap between: one file, then two.
    one_ = write_disc("one.dsk", {"ONE.BIN"});
    two_ = write_disc("two.dsk", {"TWO.BIN", "THREE.BIN"});
  }

  void TearDown() override {
    subcycle_bridge_stop();
    CPC.rom_path = saved_cpc_rom_path_;
    CPC.model = saved_model_;
    CPC.ram_size = saved_ram_;
    DiscMediaGenerationTest::TearDown();
  }

  std::string write_disc(const char* name,
                         const std::vector<std::string>& files) {
    dsk_eject_host(&driveA);
    EXPECT_EQ("", disk_format_drive('A', "data"));
    for (const std::string& file : files) {
      EXPECT_EQ("", disk_write_file(&driveA, file, {'x'}, true));
    }
    std::string const path = (dir_ / name).string();
    EXPECT_EQ(0, dsk_save(path, &driveA));
    dsk_eject_host(&driveA);
    return path;
  }

  static size_t listed_files() {
    std::string err;
    return disk_list_files(&driveA, err).size();
  }

  static void load_a(const std::string& path) {
    t_slot slot{};
    slot.drive = DRIVE::DSK_A;
    slot.file = path;
    ASSERT_EQ(0, file_load(slot));
  }

  std::string one_;
  std::string two_;

 private:
  std::string saved_cpc_rom_path_;
  unsigned int saved_model_ = 0;
  unsigned int saved_ram_ = 0;
};

}  // namespace

TEST_F(LiveBoardMediaTest, PullWhilePausedSeesTheDiscJustLoaded) {
  // No frame runs in this test -- exactly the paused machine: every insert
  // stays queued until something applies it.
  load_a(one_);
  ASSERT_TRUE(subcycle_bridge_pull_drive_view(0));
  EXPECT_EQ(1u, listed_files());

  load_a(two_);
  ASSERT_TRUE(subcycle_bridge_pull_drive_view(0));
  EXPECT_EQ(2u, listed_files())
      << "the pull read the outgoing disc over the one just loaded";

  // A Disc Tools edit pushes the view back. It must land on the new disc,
  // not cancel a still-queued swap and leave the old one mounted.
  ASSERT_EQ("", disk_write_file(&driveA, "FOUR.BIN", {'y'}, true));
  ASSERT_TRUE(subcycle_bridge_push_drive_view(0));
  ASSERT_TRUE(subcycle_bridge_pull_drive_view(0));
  EXPECT_EQ(3u, listed_files());
}

TEST_F(LiveBoardMediaTest, ARejectedHotSwapStillMovesTheGeneration) {
  // apply_pending_media() ejects the drive when insert_disk() refuses the
  // image, so Disc Tools must be told to look again on the failure branch too
  // -- the generation bump sits outside the if/else by construction, and a
  // future edit that tucks it into the success arm would leave the listing
  // showing a disc the FDC no longer holds.
  load_a(one_);
  subcycle_bridge_apply_pending_media();
  ASSERT_TRUE(subcycle_bridge_pull_drive_view(0));
  ASSERT_EQ(1u, listed_files());

  // Garbage the FDC cannot parse, queued exactly the way file_load queues a
  // disc image.
  std::vector<uint8_t> const junk(512, 0xA5);
  subcycle_bridge_insert_media(junk, false, 0);
  uint64_t const queued = dsk_media_generation(0);
  subcycle_bridge_apply_pending_media();

  EXPECT_NE(queued, dsk_media_generation(0))
      << "the rejected swap emptied the drive; the listing must be rebuilt";
  EXPECT_FALSE(subcycle_bridge_pull_drive_view(0))
      << "a rejected image must leave the drive empty, not half-mounted";
}

TEST_F(LiveBoardMediaTest, TheBoardApplyingASwapMovesTheGeneration) {
  load_a(one_);
  uint64_t const queued = dsk_media_generation(0);
  subcycle_bridge_apply_pending_media();
  EXPECT_NE(queued, dsk_media_generation(0))
      << "a listing pulled between queue and apply saw the outgoing disc; "
         "readers must be told to look again when the FDC takes the new one";
}
