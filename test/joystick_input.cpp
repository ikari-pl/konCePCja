#include <gtest/gtest.h>

#include <atomic>

#include "keyboard.h"
#include "koncepcja.h"

extern t_CPC CPC;
extern byte bit_values[8];

// Joysticks are read through the keyboard matrix: joystick 0 occupies row 9 and
// joystick 1 occupies row 6 (active-low — a pressed direction/fire CLEARS its
// bit). The IPC `input joy <n> <dir>` command resolves CPC_J0_*/CPC_J1_* via
// the InputMapper to these scancodes and pokes the matrix exactly like a
// keypress. These tests pin both halves of that path: the CPC_KEYS -> scancode
// mapping, and that applying a scancode toggles the expected matrix bit.
class JoystickInputTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CPC.keyboard = 0;  // English layout (cpc_kbd[0])
    if (CPC.InputMapper == nullptr) {
      CPC.InputMapper = new InputMapper(&CPC);
      CPC.InputMapper->init();
    }
    for (auto& cell : matrix) cell.store(0xFF, std::memory_order_relaxed);
  }

  CPCScancode scancode_of(CPC_KEYS key) {
    return CPC.InputMapper->CPCscancodeFromCPCkey(key);
  }

  bool is_pressed(CPCScancode sc) {
    int line = static_cast<byte>(sc) >> 4;
    int bit = static_cast<byte>(sc) & 7;
    return (matrix[line].load(std::memory_order_relaxed) & bit_values[bit]) ==
           0;
  }

  std::atomic<byte> matrix[16];
};

// ── Scancode mapping (catches enum/table reordering regressions) ───────
TEST_F(JoystickInputTest, Joystick0KeysMapToRow9) {
  EXPECT_EQ(0x90u, scancode_of(CPC_J0_UP));
  EXPECT_EQ(0x91u, scancode_of(CPC_J0_DOWN));
  EXPECT_EQ(0x92u, scancode_of(CPC_J0_LEFT));
  EXPECT_EQ(0x93u, scancode_of(CPC_J0_RIGHT));
  EXPECT_EQ(0x94u, scancode_of(CPC_J0_FIRE1));
  EXPECT_EQ(0x95u, scancode_of(CPC_J0_FIRE2));
}

TEST_F(JoystickInputTest, Joystick1KeysMapToRow6) {
  EXPECT_EQ(0x60u, scancode_of(CPC_J1_UP));
  EXPECT_EQ(0x61u, scancode_of(CPC_J1_DOWN));
  EXPECT_EQ(0x62u, scancode_of(CPC_J1_LEFT));
  EXPECT_EQ(0x63u, scancode_of(CPC_J1_RIGHT));
  EXPECT_EQ(0x64u, scancode_of(CPC_J1_FIRE1));
  EXPECT_EQ(0x65u, scancode_of(CPC_J1_FIRE2));
}

// ── Matrix mechanics (the part `input joy` actually drives) ────────────
TEST_F(JoystickInputTest, PressClearsBitReleaseRestoresIt) {
  CPCScancode up = scancode_of(CPC_J0_UP);  // row 9, bit 0
  applyKeypressDirect(up, matrix, true);
  EXPECT_TRUE(is_pressed(up));
  applyKeypressDirect(up, matrix, false);
  EXPECT_FALSE(is_pressed(up));
}

TEST_F(JoystickInputTest, FireIsIndependentOfDirections) {
  CPCScancode fire = scancode_of(CPC_J0_FIRE1);
  applyKeypressDirect(fire, matrix, true);
  EXPECT_TRUE(is_pressed(fire));
  EXPECT_FALSE(is_pressed(scancode_of(CPC_J0_UP)));
  EXPECT_FALSE(is_pressed(scancode_of(CPC_J0_DOWN)));
}

TEST_F(JoystickInputTest, Joystick0AndJoystick1AreOnDifferentRows) {
  CPCScancode j0 = scancode_of(CPC_J0_UP);  // row 9
  CPCScancode j1 = scancode_of(CPC_J1_UP);  // row 6
  EXPECT_NE(static_cast<byte>(j0) >> 4, static_cast<byte>(j1) >> 4);
  // Pressing joystick 0 up must not disturb joystick 1.
  applyKeypressDirect(j0, matrix, true);
  EXPECT_TRUE(is_pressed(j0));
  EXPECT_FALSE(is_pressed(j1));
}
