#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <iterator>

#include "keyboard.h"
#include "koncepcja.h"
#include "vjoystick_map.h"

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

// ── The live controller path (SDL gamepad/joystick events) ─────────────
// Every SDL controller case in the event pump (gamepad button, gamepad
// axis, raw joystick button and axis) ends in controller_apply_bit(). In
// JoystickEmulation::JoystickToKeys that one function must press the bound
// CPC keys and leave the joystick rows alone (beads-avox).
class ControllerApplyBitTest : public JoystickInputTest {
 protected:
  void SetUp() override {
    JoystickInputTest::SetUp();
    saved_mode_ = CPC.joystick_emulation;
    saved_paused_ = CPC.paused;
    saved_support_ = CPC.keyboard_support_mode;
    std::copy(std::begin(CPC.joy_to_keys), std::end(CPC.joy_to_keys),
              saved_keys_);
    CPC.paused = false;  // applyKeypress() drops keys while paused
    // Releases land at once only in Direct mode; another test may have left
    // a buffered mode behind.
    CPC.keyboard_support_mode = KeyboardSupportMode::Direct;
    for (auto& cell : keyboard_matrix)
      cell.store(0xFF, std::memory_order_relaxed);
  }
  void TearDown() override {
    controller_release_all();  // forget what the test left held
    for (auto& cell : keyboard_matrix)
      cell.store(0xFF, std::memory_order_relaxed);
    CPC.joystick_emulation = saved_mode_;
    CPC.paused = saved_paused_;
    CPC.keyboard_support_mode = saved_support_;
    std::copy(std::begin(saved_keys_), std::end(saved_keys_), CPC.joy_to_keys);
  }

  bool live_pressed(CPC_KEYS key) {
    CPCScancode const sc = scancode_of(key);
    int const line = static_cast<byte>(sc) >> 4;
    int const bit = static_cast<byte>(sc) & 7;
    return (keyboard_matrix[line].load(std::memory_order_relaxed) &
            bit_values[bit]) == 0;
  }

  JoystickEmulation saved_mode_ = JoystickEmulation::None;
  bool saved_paused_ = false;
  KeyboardSupportMode saved_support_ = KeyboardSupportMode::Direct;
  int saved_keys_[6] = {};
};

TEST_F(ControllerApplyBitTest, JoystickToKeysPressesTheBoundKeys) {
  CPC.joystick_emulation = JoystickEmulation::JoystickToKeys;
  int const bound[6] = {CPC_Q, CPC_A, CPC_O, CPC_P, CPC_SPACE, CPC_M};
  std::copy(std::begin(bound), std::end(bound), CPC.joy_to_keys);
  unsigned const bits[6] = {vjoy::VJOY_UP,    vjoy::VJOY_DOWN,
                            vjoy::VJOY_LEFT,  vjoy::VJOY_RIGHT,
                            vjoy::VJOY_FIRE1, vjoy::VJOY_FIRE2};
  CPC_KEYS const joy0[6] = {CPC_J0_UP,    CPC_J0_DOWN,  CPC_J0_LEFT,
                            CPC_J0_RIGHT, CPC_J0_FIRE1, CPC_J0_FIRE2};
  for (int slot = 0; slot < 2; ++slot) {
    for (int i = 0; i < 6; ++i) {
      SCOPED_TRACE(testing::Message() << "slot " << slot << " control " << i);
      controller_apply_bit(slot, bits[i], true);
      EXPECT_TRUE(live_pressed(static_cast<CPC_KEYS>(bound[i])));
      EXPECT_FALSE(live_pressed(joy0[i]));
      EXPECT_FALSE(live_pressed(vjoy::vjoy_active_keys(1, bits[i]).keys[0]));
      controller_apply_bit(slot, bits[i], false);
      EXPECT_FALSE(live_pressed(static_cast<CPC_KEYS>(bound[i])));
    }
  }
}

TEST_F(ControllerApplyBitTest, KeyboardModeStillDrivesTheJoystickRows) {
  CPC.joystick_emulation = JoystickEmulation::Keyboard;
  CPC.joy_to_keys[0] = CPC_Q;
  controller_apply_bit(0, vjoy::VJOY_UP, true);
  EXPECT_TRUE(live_pressed(CPC_J0_UP));
  EXPECT_FALSE(live_pressed(CPC_Q));
  controller_apply_bit(1, vjoy::VJOY_FIRE2, true);
  EXPECT_TRUE(live_pressed(CPC_J1_FIRE2));
}

// An emulator command (KONCPC_EXIT, ~0x1000) in the binding table must press
// nothing: it is not a CPC key, and as an index it is far past the
// 209-entry CPC key table (beads-z0zb).
TEST_F(ControllerApplyBitTest, CommandKeyBindingPressesNothing) {
  CPC.joystick_emulation = JoystickEmulation::JoystickToKeys;
  CPC.joy_to_keys[0] = static_cast<int>(KONCPC_EXIT);
  CPC.joy_to_keys[1] = CPC_KEY_NUM;
  controller_apply_bit(0, vjoy::VJOY_UP, true);
  controller_apply_bit(0, vjoy::VJOY_DOWN, true);
  for (int row = 0; row < 16; ++row) {
    EXPECT_EQ(0xFF, keyboard_matrix[row].load(std::memory_order_relaxed))
        << "row " << row;
  }
}

// Changing the mode while a control is held releases what the OLD mode
// pressed; the release used to look the key up in the new mode and leave
// the old key down for good (beads-z0zb).
TEST_F(ControllerApplyBitTest, ModeChangeReleasesTheOldModesKeys) {
  CPC.joystick_emulation = JoystickEmulation::JoystickToKeys;
  CPC.joy_to_keys[0] = CPC_Q;
  controller_apply_bit(0, vjoy::VJOY_UP, true);
  ASSERT_TRUE(live_pressed(CPC_Q));
  set_joystick_emulation_mode(JoystickEmulation::Keyboard);
  EXPECT_FALSE(live_pressed(CPC_Q));
  // The physical release that follows, in the new mode, presses nothing
  // and releases nothing it did not press.
  controller_apply_bit(0, vjoy::VJOY_UP, false);
  EXPECT_FALSE(live_pressed(CPC_J0_UP));

  controller_apply_bit(1, vjoy::VJOY_FIRE1, true);
  ASSERT_TRUE(live_pressed(CPC_J1_FIRE1));
  set_joystick_emulation_mode(JoystickEmulation::JoystickToKeys);
  EXPECT_FALSE(live_pressed(CPC_J1_FIRE1));
  EXPECT_EQ(JoystickEmulation::JoystickToKeys, CPC.joystick_emulation);
}

// In JoystickToKeys both pads press the same keys. A key goes up only when
// neither pad still holds it: pad 2 resting in its dead zone (a stream of
// releases) must not lift the key pad 1 holds (beads-z0zb).
TEST_F(ControllerApplyBitTest, SharedKeyStaysDownWhileTheOtherSlotHoldsIt) {
  CPC.joystick_emulation = JoystickEmulation::JoystickToKeys;
  int const bound[6] = {CPC_Q, CPC_A, CPC_O, CPC_P, CPC_SPACE, CPC_SPACE};
  std::copy(std::begin(bound), std::end(bound), CPC.joy_to_keys);
  controller_apply_bit(0, vjoy::VJOY_LEFT, true);
  controller_apply_bit(1, vjoy::VJOY_LEFT, false);  // pad 2 in its dead zone
  EXPECT_TRUE(live_pressed(CPC_O));
  controller_apply_bit(1, vjoy::VJOY_LEFT, true);
  controller_apply_bit(0, vjoy::VJOY_LEFT, false);
  EXPECT_TRUE(live_pressed(CPC_O));  // pad 2 still holds it
  controller_apply_bit(1, vjoy::VJOY_LEFT, false);
  EXPECT_FALSE(live_pressed(CPC_O));
  // Two controls of one pad bound to the same key: the same rule.
  controller_apply_bit(0, vjoy::VJOY_FIRE1, true);
  controller_apply_bit(0, vjoy::VJOY_FIRE2, true);
  controller_apply_bit(0, vjoy::VJOY_FIRE1, false);
  EXPECT_TRUE(live_pressed(CPC_SPACE));
  controller_apply_bit(0, vjoy::VJOY_FIRE2, false);
  EXPECT_FALSE(live_pressed(CPC_SPACE));
}
