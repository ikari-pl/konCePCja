#include "vjoystick_map.h"

#include <gtest/gtest.h>

// The virtual joystick (on-screen pad) and the gamepad/raw-joystick event path
// both funnel through vjoy::vjoy_active_keys(): given the target CPC joystick
// and a bitmask of held controls, it returns the CPC_J{n}_* keys to assert into
// keyboard-matrix row 9 (joy 0) / row 6 (joy 1).  These tests pin that pure
// mapping — no ImGui, no SDL, no live emulator.

using namespace vjoy;

namespace {
bool has_key(const VJoyKeys& k, CPC_KEYS want) {
  for (int i = 0; i < k.count; i++) {
    if (k.keys[i] == want) return true;
  }
  return false;
}
}  // namespace

TEST(VJoyMap, Joy0SingleDirections) {
  EXPECT_TRUE(has_key(vjoy_active_keys(0, VJOY_UP), CPC_J0_UP));
  EXPECT_TRUE(has_key(vjoy_active_keys(0, VJOY_DOWN), CPC_J0_DOWN));
  EXPECT_TRUE(has_key(vjoy_active_keys(0, VJOY_LEFT), CPC_J0_LEFT));
  EXPECT_TRUE(has_key(vjoy_active_keys(0, VJOY_RIGHT), CPC_J0_RIGHT));
  EXPECT_EQ(1, vjoy_active_keys(0, VJOY_UP).count);
}

TEST(VJoyMap, Joy0Fires) {
  EXPECT_TRUE(has_key(vjoy_active_keys(0, VJOY_FIRE1), CPC_J0_FIRE1));
  EXPECT_TRUE(has_key(vjoy_active_keys(0, VJOY_FIRE2), CPC_J0_FIRE2));
}

// Target 1 must select the CPC_J1_* variants (a different matrix row).
TEST(VJoyMap, Joy1SelectsJ1Variants) {
  EXPECT_TRUE(has_key(vjoy_active_keys(1, VJOY_UP), CPC_J1_UP));
  EXPECT_TRUE(has_key(vjoy_active_keys(1, VJOY_FIRE1), CPC_J1_FIRE1));
  EXPECT_FALSE(has_key(vjoy_active_keys(1, VJOY_UP), CPC_J0_UP));
}

// A diagonal is two direction bits at once -> exactly two keys.
TEST(VJoyMap, DiagonalYieldsTwoDirectionKeys) {
  VJoyKeys k = vjoy_active_keys(0, VJOY_UP | VJOY_RIGHT);
  EXPECT_EQ(2, k.count);
  EXPECT_TRUE(has_key(k, CPC_J0_UP));
  EXPECT_TRUE(has_key(k, CPC_J0_RIGHT));
}

// A direction plus fire held together (e.g. run-and-shoot).
TEST(VJoyMap, DirectionPlusFire) {
  VJoyKeys k = vjoy_active_keys(0, VJOY_LEFT | VJOY_FIRE1);
  EXPECT_EQ(2, k.count);
  EXPECT_TRUE(has_key(k, CPC_J0_LEFT));
  EXPECT_TRUE(has_key(k, CPC_J0_FIRE1));
}

// Releasing everything (mask 0) holds nothing — the clean-release case.
TEST(VJoyMap, EmptyMaskHoldsNothing) {
  EXPECT_EQ(0, vjoy_active_keys(0, 0u).count);
  EXPECT_EQ(0, vjoy_active_keys(1, 0u).count);
}

// vjoy_all_keys() (used to force-release a slot on unplug / window close) must
// cover all six controls for the requested target.
TEST(VJoyMap, AllKeysCoversSixControls) {
  VJoyKeys k0 = vjoy_all_keys(0);
  EXPECT_EQ(6, k0.count);
  EXPECT_TRUE(has_key(k0, CPC_J0_UP));
  EXPECT_TRUE(has_key(k0, CPC_J0_DOWN));
  EXPECT_TRUE(has_key(k0, CPC_J0_LEFT));
  EXPECT_TRUE(has_key(k0, CPC_J0_RIGHT));
  EXPECT_TRUE(has_key(k0, CPC_J0_FIRE1));
  EXPECT_TRUE(has_key(k0, CPC_J0_FIRE2));

  VJoyKeys k1 = vjoy_all_keys(1);
  EXPECT_EQ(6, k1.count);
  EXPECT_TRUE(has_key(k1, CPC_J1_FIRE2));
}

// Out-of-range / negative target clamps to joystick 0 (only 0 == joy1 selects
// the J1 bank), so anything that isn't 1 is treated as joy 0.
TEST(VJoyMap, NonOneTargetIsJoy0) {
  EXPECT_TRUE(has_key(vjoy_active_keys(2, VJOY_UP), CPC_J0_UP));
  EXPECT_TRUE(has_key(vjoy_active_keys(-1, VJOY_UP), CPC_J0_UP));
}

// JoystickToKeys: a controller presses the bound CPC keys, in VJOY_* bit
// order, for either slot; a null binding table keeps the joystick bits.
TEST(VJoystickMap, ControllerKeysUseTheBindingTableWhenGiven) {
  int const bound[6] = {CPC_Q, CPC_A, CPC_O, CPC_P, CPC_SPACE, CPC_RETURN};
  VJoyKeys const one = vjoy_controller_keys(1, VJOY_LEFT, bound);
  ASSERT_EQ(1, one.count);
  EXPECT_EQ(CPC_O, one.keys[0]);
  VJoyKeys const all = vjoy_controller_keys(0, VJOY_ALL, bound);
  ASSERT_EQ(6, all.count);
  for (int i = 0; i < 6; ++i) EXPECT_EQ(bound[i], all.keys[i]);
  VJoyKeys const joy = vjoy_controller_keys(1, VJOY_FIRE1, nullptr);
  ASSERT_EQ(1, joy.count);
  EXPECT_EQ(CPC_J1_FIRE1, joy.keys[0]);
}

// A binding that is not a CPC key (an emulator command such as KONCPC_EXIT,
// ~0x1000, or anything past the CPC key table) presses nothing: cast to
// CPC_KEYS it would wrap to some unrelated key (beads-z0zb).
TEST(VJoystickMap, ControllerKeysSkipBindingsThatAreNotCpcKeys) {
  int const bound[6] = {
      static_cast<int>(KONCPC_EXIT), -1, CPC_KEY_NUM, CPC_P, 0xff, CPC_RETURN};
  EXPECT_EQ(0, vjoy_controller_keys(0, VJOY_UP, bound).count);
  EXPECT_EQ(0, vjoy_controller_keys(0, VJOY_DOWN, bound).count);
  EXPECT_EQ(0, vjoy_controller_keys(0, VJOY_LEFT, bound).count);
  EXPECT_EQ(0, vjoy_controller_keys(1, VJOY_FIRE1, bound).count);
  VJoyKeys const all = vjoy_controller_keys(0, VJOY_ALL, bound);
  ASSERT_EQ(2, all.count);
  EXPECT_EQ(CPC_P, all.keys[0]);
  EXPECT_EQ(CPC_RETURN, all.keys[1]);
}
