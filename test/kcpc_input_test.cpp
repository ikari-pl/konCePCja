// konCePCja — tests for what KCPC's input opcodes reach in the emulator
// (beads-3o1.6): the keyboard matrix by position, the AutoTypeQueue and the
// staged mouse. No server and no machine: the opcodes themselves are tested
// with a fake machine in kcpc_server_test.cpp and end to end against a core
// in test/integrated/kcpc/test_kcpc.py.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include "autotype.h"
#include "kcpc/kcpc_host.h"
#include "kcpc/kcpc_proto.h"
#include "keyboard.h"
#include "koncepcja.h"
#include "koncepcja_ipc_server.h"
#include "video_plugin.h"

extern t_CPC CPC;
extern video_plugin* vid_plugin;

namespace {

using kcpc::Status;

constexpr byte kShiftRow = 2;
constexpr byte kShiftBit = 5;

struct Matrix {
  Matrix() {
    for (auto& row : rows) row.store(0xFF);
  }
  std::atomic<byte> rows[16];
};

// Restores what a test changed in the globals the host functions reach.
class KcpcHostInputTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mouse_was_fitted_ = ipc_mouse_gate_open();
    gun_was_fitted_ = ipc_gun_gate_open();
    kbd_matrix_release_all(keyboard_matrix);
    g_autotype_queue.clear();
  }
  void TearDown() override {
    kbd_matrix_release_all(keyboard_matrix);
    g_autotype_queue.clear();
    ipc_publish_device_gates(mouse_was_fitted_, gun_was_fitted_);
  }
  bool mouse_was_fitted_ = false;
  bool gun_was_fitted_ = false;
};

}  // namespace

TEST(KbdMatrixByPosition, ALineIsPressedAndReleasedAlone) {
  Matrix m;
  kbd_matrix_set_line(m.rows, 8, 5, true);
  EXPECT_EQ(m.rows[8].load(), 0xDF);
  kbd_matrix_set_line(m.rows, 8, 5, false);
  EXPECT_EQ(m.rows[8].load(), 0xFF);
}

// The difference from applyKeypressDirect() and the text IPC's key commands:
// those release SHIFT and CONTROL around a plain key. A frontend that sends
// matrix positions holds its own modifiers.
TEST(KbdMatrixByPosition, AKeyLeavesTheModifiersAlone) {
  Matrix m;
  kbd_matrix_set_line(m.rows, kShiftRow, kShiftBit, true);
  kbd_matrix_set_line(m.rows, 8, 5, true);
  EXPECT_EQ(m.rows[kShiftRow].load(), 0xDF) << "pressing A released SHIFT";
  kbd_matrix_set_line(m.rows, 8, 5, false);
  EXPECT_EQ(m.rows[kShiftRow].load(), 0xDF) << "releasing A released SHIFT";

  Matrix direct;
  kbd_matrix_set_line(direct.rows, kShiftRow, kShiftBit, true);
  applyKeypressDirect(0x85, direct.rows, true);
  EXPECT_EQ(direct.rows[kShiftRow].load(), 0xFF)
      << "the by-name path is expected to drive the modifiers itself";
}

TEST(KbdMatrixByPosition, OutOfRangeTouchesNothing) {
  Matrix m;
  kbd_matrix_set_line(m.rows, 10, 0, true);
  kbd_matrix_set_line(m.rows, -1, 0, true);
  kbd_matrix_set_line(m.rows, 0, 8, true);
  kbd_matrix_set_line(m.rows, 0, -1, true);
  for (const auto& row : m.rows) EXPECT_EQ(row.load(), 0xFF);
}

TEST(KbdMatrixByPosition, ReleaseAllAndReadCoverTheTenRows) {
  Matrix m;
  kbd_matrix_set_line(m.rows, 0, 0, true);
  kbd_matrix_set_line(m.rows, 9, 7, true);
  byte rows[kKbdMatrixRows];
  kbd_matrix_read(m.rows, rows);
  EXPECT_EQ(rows[0], 0xFE);
  EXPECT_EQ(rows[9], 0x7F);
  for (int row = 1; row < 9; ++row) EXPECT_EQ(rows[row], 0xFF) << row;

  kbd_matrix_release_all(m.rows);
  kbd_matrix_read(m.rows, rows);
  for (int row = 0; row < kKbdMatrixRows; ++row) {
    EXPECT_EQ(rows[row], 0xFF) << row;
  }
}

TEST_F(KcpcHostInputTest, KeyAndInputStateAreThePendingMatrix) {
  ASSERT_EQ(kcpc_host_key(8, 5, true), Status::kOk);
  ASSERT_EQ(kcpc_host_key(9, 0, true), Status::kOk);
  EXPECT_EQ(keyboard_matrix[8].load(), 0xDF);

  kcpc::KeyMatrix rows{};
  ASSERT_EQ(kcpc_host_input_state(rows), Status::kOk);
  EXPECT_EQ(rows[8], 0xDF);
  EXPECT_EQ(rows[9], 0xFE);
  EXPECT_EQ(rows[0], 0xFF);

  ASSERT_EQ(kcpc_host_keys_release(), Status::kOk);
  ASSERT_EQ(kcpc_host_input_state(rows), Status::kOk);
  for (std::uint8_t const row : rows) EXPECT_EQ(row, 0xFF);
}

TEST_F(KcpcHostInputTest, TextGoesOntoTheAutoTypeQueue) {
  std::uint32_t pending = 99;
  std::string message;
  ASSERT_EQ(kcpc_host_text("print 1+1~RETURN~", pending, message), Status::kOk);
  // Nine characters and one token: ten key actions.
  EXPECT_EQ(pending, 10U);
  EXPECT_EQ(g_autotype_queue.remaining(), 10U);
  EXPECT_TRUE(message.empty());

  // More text queues behind what is there.
  ASSERT_EQ(kcpc_host_text("a", pending, message), Status::kOk);
  EXPECT_EQ(pending, 11U);
}

TEST_F(KcpcHostInputTest, TextTheQueueCannotParseQueuesNothing) {
  std::uint32_t pending = 99;
  std::string message;
  EXPECT_EQ(kcpc_host_text("ab~NOSUCHKEY~", pending, message),
            Status::kBadPayload);
  EXPECT_NE(message.find("NOSUCHKEY"), std::string::npos) << message;
  EXPECT_EQ(g_autotype_queue.remaining(), 0U);

  EXPECT_EQ(kcpc_host_text("ab~RETURN", pending, message), Status::kBadPayload);
  EXPECT_NE(message.find("unclosed"), std::string::npos) << message;
  EXPECT_EQ(g_autotype_queue.remaining(), 0U);
}

TEST_F(KcpcHostInputTest, MouseNeedsAMouse) {
  ipc_publish_device_gates(false, false);
  EXPECT_EQ(kcpc_host_mouse(5, 5, false, 0), Status::kNotPresent);
  EXPECT_FALSE(ipc_stage_mouse(1, 1, std::nullopt));

  ipc_publish_device_gates(true, false);
  EXPECT_EQ(kcpc_host_mouse(5, -5, true, kcpc::kMouseButtonLeft), Status::kOk);
  EXPECT_TRUE(ipc_stage_mouse(1, 1, std::nullopt));
}

TEST_F(KcpcHostInputTest, GunNeedsAGun) {
  ipc_publish_device_gates(false, false);
  EXPECT_EQ(kcpc_host_gun(true, 10, 10, true, true), Status::kNotPresent);
  EXPECT_FALSE(ipc_stage_gun(IpcGunAim{1, 1}, std::nullopt));
}

// GUN aims in the frame's pixels, which are CPC.phazer_x/y's own: nothing
// maps them through the window. The headless plugin's scale is 0, so a
// window mapping would put every aim at the top left corner.
TEST_F(KcpcHostInputTest, GunAimsInFramePixelsWhateverTheWindow) {
  static video_plugin headless{};
  headless.x_scale = 0.0f;
  headless.y_scale = 0.0f;
  video_plugin* const saved_plugin = vid_plugin;
  vid_plugin = &headless;
  auto const saved_type = CPC.phazer_emulation;
  CPC.phazer_emulation = PhazerType::AmstradMagnumPhaser;
  CPC.phazer_x = 0;
  CPC.phazer_y = 0;
  CPC.phazer_pressed = false;
  ipc_publish_device_gates(false, true);

  ASSERT_EQ(kcpc_host_gun(true, 400, 120, true, true), Status::kOk);
  ipc_drain_input();
  EXPECT_EQ(CPC.phazer_x, 400U);
  EXPECT_EQ(CPC.phazer_y, 120U);
  EXPECT_TRUE(CPC.phazer_pressed);

  // The trigger alone leaves the aim where it is, and needs no plugin.
  vid_plugin = nullptr;
  ASSERT_EQ(kcpc_host_gun(false, 0, 0, true, false), Status::kOk);
  ipc_drain_input();
  EXPECT_EQ(CPC.phazer_x, 400U);
  EXPECT_EQ(CPC.phazer_y, 120U);
  EXPECT_FALSE(CPC.phazer_pressed);

  vid_plugin = saved_plugin;
  CPC.phazer_emulation = saved_type;
}
