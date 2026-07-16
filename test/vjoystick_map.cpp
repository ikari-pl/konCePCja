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
