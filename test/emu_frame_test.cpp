// beads-cv2.2: the GUI's Z80 thread and the -H main loop used to carry two
// copies of the per-frame duties, and the -H copy had drifted: it never
// relayed the scanned keyboard rows to the KeyboardManager, and a finished
// step paused by writing CPC.paused instead of calling cpc_pause(). Both loops
// now call emu_run_frame(); these tests pin the GUI behaviour it carries.

#include "emu_frame.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "keyboard_manager.h"
#include "koncepcja.h"
#include "subcycle/machine.h"
#include "subcycle_bridge.h"
#include "z80_view.h"

extern t_CPC CPC;
extern t_z80regs z80;

namespace {

// Scans keyboard row 5 forever, the way the firmware does: PSG register 14
// selected, PPI port A switched to input, row 5 put on port C with the PSG in
// read mode, then IN from port A.
const unsigned char kScanRow5[] = {
    0x01, 0x82, 0xF7, 0xED, 0x49,  // LD BC,&F782 : OUT (C),C  PPI A=out
    0x01, 0x0E, 0xF4, 0xED, 0x49,  // LD BC,&F40E : OUT (C),C  port A = 14
    0x01, 0xC0, 0xF6, 0xED, 0x49,  // LD BC,&F6C0 : OUT (C),C  PSG select
    0x01, 0x00, 0xF6, 0xED, 0x49,  // LD BC,&F600 : OUT (C),C  PSG inactive
    0x01, 0x92, 0xF7, 0xED, 0x49,  // LD BC,&F792 : OUT (C),C  PPI A=in
    0x01, 0x45, 0xF6, 0xED, 0x49,  // LD BC,&F645 : OUT (C),C  read row 5
    0x06, 0xF4, 0xED, 0x78,        // LD B,&F4 : IN A,(C)
    0x01, 0x82, 0xF7, 0xED, 0x49,  // LD BC,&F782 : OUT (C),C  PPI A=out
    0x01, 0x00, 0xF6, 0xED, 0x49,  // LD BC,&F600 : OUT (C),C  PSG inactive
    0x18, 0xD2,                    // JR $0000
};
static_assert(sizeof(kScanRow5) == 0x2E, "JR offset assumes 46 bytes");

constexpr CPCScancode kSpace = 0x57;  // row 5, bit 7

// Brings the sub-cycle board up on a synthetic 32K system ROM and tears it
// down again so no other test sees an active bridge.
class EmuFrameTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / "koncepcja-emu-frame";
    std::filesystem::create_directories(dir_);
    std::vector<char> rom(0x8000, 0);
    for (size_t i = 0; i < sizeof(kScanRow5); ++i)
      rom[i] = static_cast<char>(kScanRow5[i]);
    std::filesystem::path const rom_file = dir_ / "cpc6128.rom";
    FILE* f = fopen(rom_file.string().c_str(), "wb");
    ASSERT_NE(nullptr, f);
    ASSERT_EQ(rom.size(), fwrite(rom.data(), 1, rom.size(), f));
    ASSERT_EQ(0, fclose(f));
    saved_rom_path_ = CPC.rom_path;
    saved_model_ = CPC.model;
    saved_ram_ = CPC.ram_size;
    saved_mode_ = CPC.keyboard_support_mode;
    saved_limit_ = CPC.limit_speed;
    CPC.rom_path = dir_.string();
    CPC.model = 2;  // chROMFile[2] == "cpc6128.rom"
    CPC.ram_size = 128;
    CPC.limit_speed = 0;
    ASSERT_TRUE(subcycle_bridge_start());
    started_ = true;
    ASSERT_NE(nullptr, subcycle_bridge_machine());
    subcycle_bridge_machine()->reset();
    for (auto& row : keyboard_matrix) row.store(0xFF);
  }

  void TearDown() override {
    if (started_) subcycle_bridge_stop();
    for (auto& row : keyboard_matrix) row.store(0xFF);
    CPC.rom_path = saved_rom_path_;
    CPC.model = saved_model_;
    CPC.ram_size = saved_ram_;
    CPC.keyboard_support_mode = saved_mode_;
    CPC.limit_speed = saved_limit_;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::string saved_rom_path_;
  unsigned int saved_model_ = 0;
  unsigned int saved_ram_ = 0;
  KeyboardSupportMode saved_mode_ = KeyboardSupportMode::Direct;
  unsigned int saved_limit_ = 0;
  bool started_ = false;
};

}  // namespace

// BufferedUntilRead holds a released key down until the firmware has read its
// row. The frame must relay the scanned rows to the KeyboardManager, or the
// key never comes up (the -H loop's drift).
TEST_F(EmuFrameTest, FrameReleasesABufferedKeyOnceItsRowIsScanned) {
  // The first frame after reset is a short warm-up that ends before the
  // scan loop reads the keyboard.
  ASSERT_EQ(EmuFrameResult::kFrameComplete,
            emu_run_frame(/*bridge_paces=*/false));
  CPC.keyboard_support_mode = KeyboardSupportMode::BufferedUntilRead;
  g_keyboard_manager.handle_keydown(kSpace, keyboard_matrix);
  g_keyboard_manager.handle_keyup(kSpace, keyboard_matrix, false, 0);
  ASSERT_EQ(0, keyboard_matrix[5].load() & 0x80)
      << "the key must stay held until its row is scanned";

  ASSERT_EQ(EmuFrameResult::kFrameComplete,
            emu_run_frame(/*bridge_paces=*/false));

  EXPECT_EQ(0x80, keyboard_matrix[5].load() & 0x80)
      << "the frame scanned row 5 with SPACE down; the held key must be "
         "released";
}

// A finished step pauses through cpc_pause(), so g_emu_paused (what the GUI
// Z80 thread, the render thread and the pause lease read) follows CPC.paused.
TEST_F(EmuFrameTest, FinishedStepPausesThroughCpcPause) {
  cpc_resume();
  ASSERT_FALSE(g_emu_paused.load());
  z80.breakpoint_reached = 0;
  z80.watchpoint_reached = 0;
  z80.step_in = 2;

  EXPECT_EQ(EmuFrameResult::kStopped, emu_handle_stop());

  EXPECT_TRUE(CPC.paused);
  EXPECT_TRUE(g_emu_paused.load());
  EXPECT_EQ(0, z80.step_in);
  cpc_resume();
}
