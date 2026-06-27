#include "amx_mouse.h"

#include <gtest/gtest.h>

// The AMX Mouse appears on the joystick-port keyboard-matrix row 9 (active-low:
// a cleared bit means "active"). These tests exercise the pure device logic
// that the IPC `input mouse` commands feed via amx_mouse_update() — motion
// accumulation, the row-9 direction/button bit encoding, and the
// deselect/reselect mickey-consumption handshake.
class AmxMouseTest : public ::testing::Test {
 protected:
  void SetUp() override { amx_mouse_reset(); }
};

TEST_F(AmxMouseTest, ResetClearsState) {
  amx_mouse_update(5.0f, -3.0f, 0x07);
  amx_mouse_reset();
  EXPECT_EQ(0, g_amx_mouse.mickey_x);
  EXPECT_EQ(0, g_amx_mouse.mickey_y);
  EXPECT_EQ(0, g_amx_mouse.buttons);
  EXPECT_NEAR(0.0f, g_amx_mouse.accum_x, 1e-6f);
  EXPECT_NEAR(0.0f, g_amx_mouse.accum_y, 1e-6f);
  EXPECT_EQ(0xFF, amx_mouse_get_row9());  // nothing active
}

TEST_F(AmxMouseTest, WholePixelMotionAccumulatesMickeys) {
  amx_mouse_update(5.0f, 3.0f, 0);
  EXPECT_EQ(5, g_amx_mouse.mickey_x);
  EXPECT_EQ(3, g_amx_mouse.mickey_y);
  // Subsequent motion adds to the pending mickey counters.
  amx_mouse_update(-2.0f, -1.0f, 0);
  EXPECT_EQ(3, g_amx_mouse.mickey_x);
  EXPECT_EQ(2, g_amx_mouse.mickey_y);
}

TEST_F(AmxMouseTest, SubPixelMotionAccumulatesUntilWhole) {
  amx_mouse_update(0.4f, 0.0f, 0);
  EXPECT_EQ(0, g_amx_mouse.mickey_x);  // 0.4 — no whole mickey yet
  amx_mouse_update(0.4f, 0.0f, 0);
  EXPECT_EQ(0, g_amx_mouse.mickey_x);  // 0.8 — still none
  amx_mouse_update(0.4f, 0.0f, 0);
  EXPECT_EQ(1, g_amx_mouse.mickey_x);  // 1.2 — one mickey, 0.2 carried over
  EXPECT_NEAR(0.2f, g_amx_mouse.accum_x, 1e-3f);
}

// ── Row-9 direction encoding (active-low) ──────────────────────────────
TEST_F(AmxMouseTest, PositiveXIsRight) {
  amx_mouse_update(1.0f, 0.0f, 0);
  EXPECT_EQ(0xFF & ~0x08, amx_mouse_get_row9());
}
TEST_F(AmxMouseTest, NegativeXIsLeft) {
  amx_mouse_update(-1.0f, 0.0f, 0);
  EXPECT_EQ(0xFF & ~0x04, amx_mouse_get_row9());
}
TEST_F(AmxMouseTest, NegativeYIsUp) {
  amx_mouse_update(0.0f, -1.0f, 0);
  EXPECT_EQ(0xFF & ~0x01, amx_mouse_get_row9());
}
TEST_F(AmxMouseTest, PositiveYIsDown) {
  amx_mouse_update(0.0f, 1.0f, 0);
  EXPECT_EQ(0xFF & ~0x02, amx_mouse_get_row9());
}

// ── Button encoding (SDL mask -> row-9 fire bits) ──────────────────────
TEST_F(AmxMouseTest, LeftButtonMapsToFire2) {
  amx_mouse_update(0.0f, 0.0f, 1);  // SDL_BUTTON_LMASK
  EXPECT_EQ(0xFF & ~0x10, amx_mouse_get_row9());
}
TEST_F(AmxMouseTest, RightButtonMapsToFire1) {
  amx_mouse_update(0.0f, 0.0f, 4);  // SDL_BUTTON_RMASK
  EXPECT_EQ(0xFF & ~0x20, amx_mouse_get_row9());
}
TEST_F(AmxMouseTest, MiddleButtonMapsToFire3) {
  amx_mouse_update(0.0f, 0.0f, 2);  // SDL_BUTTON_MMASK
  EXPECT_EQ(0xFF & ~0x40, amx_mouse_get_row9());
}
TEST_F(AmxMouseTest, MotionAndButtonsCombine) {
  amx_mouse_update(1.0f, 0.0f, 1);  // right + left button
  EXPECT_EQ(0xFF & ~0x08 & ~0x10, amx_mouse_get_row9());
}

// ── Deselect/reselect handshake consumes one mickey per axis ───────────
TEST_F(AmxMouseTest, RowSelectCycleConsumesOneMickeyPerAxis) {
  amx_mouse_update(3.0f, -2.0f, 0);
  amx_mouse_row_select(9);  // select row 9
  amx_mouse_row_select(5);  // deselect (software reads another row)
  amx_mouse_row_select(9);  // reselect -> consume one mickey toward zero
  EXPECT_EQ(2, g_amx_mouse.mickey_x);
  EXPECT_EQ(-1, g_amx_mouse.mickey_y);
}

TEST_F(AmxMouseTest, ReselectWithoutDeselectDoesNotConsume) {
  amx_mouse_update(3.0f, 0.0f, 0);
  amx_mouse_row_select(9);
  amx_mouse_row_select(9);  // no intervening deselect -> no consumption
  EXPECT_EQ(3, g_amx_mouse.mickey_x);
}
