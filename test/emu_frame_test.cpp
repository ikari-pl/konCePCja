// beads-cv2.2: the GUI's Z80 thread and the -H main loop used to carry two
// copies of the per-frame duties, and the -H copy had drifted: it never
// relayed the scanned keyboard rows to the KeyboardManager, and a finished
// step paused by writing CPC.paused instead of calling cpc_pause(). Both loops
// now call emu_run_frame(); these tests pin the GUI behaviour it carries.

#include "emu_frame.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "autotype.h"
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

// Programs the CRTC with the firmware's 50 Hz screen (R0-R9), then DI : JR $.
// The pacing tests need frames far cheaper than the 20 ms period: with the
// CRTC left at zero (as the scan loop above does) a frame costs ~10-12 ms;
// this one runs in well under a millisecond.
const unsigned char kIdle[] = {
    0xF3, 0x21, 0x18, 0x00, 0x16, 0x00, 0x06, 0xBC, 0xED, 0x51, 0x7E, 0x23,
    0x06, 0xBD, 0xED, 0x79, 0x14, 0x7A, 0xFE, 0x0A, 0x20, 0xF0, 0x18, 0xFE,
    63,   40,   46,   0x8E, 38,   0,    25,   30,   0,    7};

// Brings the sub-cycle board up on a synthetic 32K system ROM and tears it
// down again so no other test sees an active bridge.
class EmuFrameTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / "koncepcja-emu-frame";
    std::filesystem::create_directories(dir_);
    std::vector<char> rom(0x8000, 0);
    std::vector<unsigned char> const code = rom_code();
    for (size_t i = 0; i < code.size(); ++i)
      rom[i] = static_cast<char>(code[i]);
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
    saved_frameskip_ = CPC.frameskip;
    CPC.rom_path = dir_.string();
    CPC.model = 2;  // chROMFile[2] == "cpc6128.rom"
    CPC.ram_size = 128;
    CPC.limit_speed = 0;
    // A queue another (shuffled) test left behind would type through
    // CPC.InputMapper, which the test binary never builds, and would force
    // real-time pacing on.
    g_autotype_queue.clear();
    ASSERT_TRUE(subcycle_bridge_start());
    started_ = true;
    ASSERT_NE(nullptr, subcycle_bridge_machine());
    subcycle_bridge_machine()->reset();
    for (auto& row : keyboard_matrix) row.store(0xFF);
  }

  void TearDown() override {
    if (started_) subcycle_bridge_stop();
    g_autotype_queue.clear();
    for (auto& row : keyboard_matrix) row.store(0xFF);
    CPC.rom_path = saved_rom_path_;
    CPC.model = saved_model_;
    CPC.ram_size = saved_ram_;
    CPC.keyboard_support_mode = saved_mode_;
    CPC.limit_speed = saved_limit_;
    CPC.frameskip = saved_frameskip_;
    CPC.skip_rendering = false;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  virtual std::vector<unsigned char> rom_code() const {
    return {std::begin(kScanRow5), std::end(kScanRow5)};
  }

  std::filesystem::path dir_;
  std::string saved_rom_path_;
  unsigned int saved_model_ = 0;
  unsigned int saved_ram_ = 0;
  KeyboardSupportMode saved_mode_ = KeyboardSupportMode::Direct;
  unsigned int saved_limit_ = 0;
  unsigned int saved_frameskip_ = 0;
  bool started_ = false;
};

class EmuFramePacingTest : public EmuFrameTest {
 protected:
  std::vector<unsigned char> rom_code() const override {
    return {std::begin(kIdle), std::end(kIdle)};
  }
};

}  // namespace

// BufferedUntilRead holds a released key down until the firmware has read its
// row. The frame must relay the scanned rows to the KeyboardManager, or the
// key never comes up (the -H loop's drift).
TEST_F(EmuFrameTest, FrameReleasesABufferedKeyOnceItsRowIsScanned) {
  // The first frame after reset is a short warm-up that ends before the
  // scan loop reads the keyboard.
  ASSERT_EQ(EmuFrameResult::kFrameComplete, emu_run_frame());
  CPC.keyboard_support_mode = KeyboardSupportMode::BufferedUntilRead;
  g_keyboard_manager.handle_keydown(kSpace, keyboard_matrix);
  g_keyboard_manager.handle_keyup(kSpace, keyboard_matrix, false, 0);
  ASSERT_EQ(0, keyboard_matrix[5].load() & 0x80)
      << "the key must stay held until its row is scanned";

  ASSERT_EQ(EmuFrameResult::kFrameComplete, emu_run_frame());

  EXPECT_EQ(0x80, keyboard_matrix[5].load() & 0x80)
      << "the frame scanned row 5 with SPACE down; the held key must be "
         "released";
}

// A finished step pauses through cpc_pause(), so g_emu_paused (what the GUI
// Z80 thread, the render thread and the pause lease read) follows CPC.paused.
TEST_F(EmuFrameTest, FinishedStepPausesThroughCpcPause) {
  // Set the running state directly: cpc_resume() is a no-op when an earlier
  // (shuffled) test left CPC.paused clear or a pause lease held.
  bool const saved_paused = CPC.paused;
  bool const saved_emu_paused = g_emu_paused.load();
  CPC.paused = false;
  g_emu_paused.store(false);
  z80.breakpoint_reached = 0;
  z80.watchpoint_reached = 0;
  z80.step_in = 2;

  EXPECT_EQ(EmuFrameResult::kStopped, emu_handle_stop());

  EXPECT_TRUE(CPC.paused);
  EXPECT_TRUE(g_emu_paused.load());
  EXPECT_EQ(0, z80.step_in);
  CPC.paused = saved_paused;
  g_emu_paused.store(saved_emu_paused);
}

// beads-af0k / beads-o478: the frameskip deadline (perfTicksTarget) was only
// ever advanced by a limiter that no longer runs, so with frameskip on every
// frame ~20 ms after start counted as "late" and none reached the video ring.
// Skipping now keys on the bridge pacer's verdict (FramePacer, unit-tested
// with a synthetic clock), capped so the display never starves. These tests
// run on the real clock, so they only assert what host load cannot change:
// an oversleep makes frames later, never earlier.
TEST_F(EmuFramePacingTest, FrameskipNeverStarvesTheDisplay) {
  CPC.limit_speed = 1;
  CPC.frameskip = 1;
  ASSERT_EQ(EmuFrameResult::kFrameComplete, emu_run_frame());  // warm-up
  int rendered = 0;
  for (int i = 0; i < 12; ++i) {
    ASSERT_EQ(EmuFrameResult::kFrameComplete, emu_run_frame());
    if (!CPC.skip_rendering) ++rendered;
  }
  // However late every frame runs, one in kMaxConsecutiveSkips + 1 renders.
  EXPECT_GE(rendered, 12 / static_cast<int>(kMaxConsecutiveSkips + 1));
}

// The wiring, with an exact oracle: every frame's skip decision is the
// pacer's verdict on the frame before it, fed through frameskip_should_skip()
// with the running skip count. Whatever the host's timing makes of the stall
// (late frames, or a >250 ms resync on a crawling CI Mac), the decision must
// match it.
TEST_F(EmuFramePacingTest, SkipFollowsThePacersVerdictOnThePreviousFrame) {
  CPC.limit_speed = 1;
  CPC.frameskip = 1;
  ASSERT_EQ(EmuFrameResult::kFrameComplete, emu_run_frame());
  unsigned consecutive = 0;
  for (int i = 0; i < 12; ++i) {
    // 100 ms behind (under the pacer's 250 ms resync) leaves the next
    // frames more than a period late until they catch up.
    if (i == 4) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool const prev_late = subcycle_bridge_frame_was_late();
    bool const expected =
        frameskip_should_skip(true, true, 1, prev_late, consecutive);
    ASSERT_EQ(EmuFrameResult::kFrameComplete, emu_run_frame());
    EXPECT_EQ(expected, CPC.skip_rendering)
        << "frame " << i << " (previous frame late: " << prev_late << ")";
    consecutive = CPC.skip_rendering ? consecutive + 1 : 0;
  }
}

TEST_F(EmuFramePacingTest, FrameskipOffNeverSkips) {
  CPC.limit_speed = 1;
  CPC.frameskip = 0;
  ASSERT_EQ(EmuFrameResult::kFrameComplete, emu_run_frame());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(EmuFrameResult::kFrameComplete, emu_run_frame());
    EXPECT_FALSE(CPC.skip_rendering);
  }
}

// The pacer's sleep is what the DevTools "sleep" stat reports; it was never
// accumulated once the bridge took over pacing (beads-af0k).
TEST_F(EmuFramePacingTest, PacedFramesReportTheirSleep) {
  uint8_t rows[16];
  for (auto& r : rows) r = 0xFF;
  (void)subcycle_bridge_take_sleep_ticks();
  // The first frame sets the deadline; the next three wait for theirs.
  for (int i = 0; i < 4; ++i)
    subcycle_bridge_frame(rows, nullptr, /*limit=*/true);
  double const slept_ms =
      static_cast<double>(subcycle_bridge_take_sleep_ticks()) * 1000.0 /
      static_cast<double>(SDL_GetPerformanceFrequency());
  // Three 20 ms periods less three sub-millisecond frames; a descheduled
  // frame can eat some of that, so assert well under the ideal ~59 ms.
  EXPECT_GT(slept_ms, 20.0);
  EXPECT_EQ(0u, subcycle_bridge_take_sleep_ticks()) << "take must reset";

  subcycle_bridge_frame(rows, nullptr, /*limit=*/false);
  EXPECT_EQ(0u, subcycle_bridge_take_sleep_ticks())
      << "an unpaced frame never sleeps";
}

TEST(FrameskipDecision, SkipsOnlyALateCompletedPacedFrameUpToTheCap) {
  // prev_complete, limit, frameskip, prev_late, consecutive
  EXPECT_TRUE(frameskip_should_skip(true, true, 1, true, 0));
  EXPECT_FALSE(frameskip_should_skip(true, true, 1, false, 0));  // on time
  EXPECT_FALSE(frameskip_should_skip(true, true, 0, true, 0));   // option off
  EXPECT_FALSE(frameskip_should_skip(true, false, 1, true, 0));  // unpaced
  EXPECT_FALSE(frameskip_should_skip(false, true, 1, true, 0));  // mid-frame
  EXPECT_TRUE(
      frameskip_should_skip(true, true, 1, true, kMaxConsecutiveSkips - 1));
  EXPECT_FALSE(frameskip_should_skip(true, true, 1, true, kMaxConsecutiveSkips))
      << "a machine that can never keep up must still show a frame";
}
