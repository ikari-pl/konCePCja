#include "joy_key_capture.h"

#include <gtest/gtest.h>

// The Settings capture for the stick-to-keys bindings (beads-avox): fed from
// KEY_DOWN/KEY_UP events, it binds the key that went down with the modifiers
// of that press, ignores auto-repeat, and never outlives its slot buttons.

namespace {
JoyKeyBinding take(JoyKeyCapture& c) {
  std::optional<JoyKeyBinding> b = c.take_binding();
  EXPECT_TRUE(b.has_value());
  return b.value_or(JoyKeyBinding{-1, SDL_SCANCODE_UNKNOWN, SDL_KMOD_NONE});
}
}  // namespace

TEST(JoyKeyCapture, IdleCaptureLetsEveryEventThrough) {
  JoyKeyCapture c;
  EXPECT_FALSE(c.on_key_down(SDL_SCANCODE_A, SDL_KMOD_NONE, false));
  EXPECT_FALSE(c.on_key_up(SDL_SCANCODE_A));
  EXPECT_FALSE(c.take_binding().has_value());
}

TEST(JoyKeyCapture, KeyBindsOnPressWithItsOwnModifiers) {
  JoyKeyCapture c;
  c.arm(3);
  EXPECT_TRUE(c.on_key_down(SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT, false));
  EXPECT_TRUE(c.armed());  // a modifier alone waits for its release
  EXPECT_TRUE(
      c.on_key_down(SDL_SCANCODE_SEMICOLON, SDL_KMOD_LSHIFT, /*repeat=*/false));
  EXPECT_FALSE(c.armed());
  JoyKeyBinding const b = take(c);
  EXPECT_EQ(3, b.slot);
  EXPECT_EQ(SDL_SCANCODE_SEMICOLON, b.scancode);
  EXPECT_EQ(SDL_KMOD_LSHIFT, b.mods);
  EXPECT_FALSE(c.take_binding().has_value());  // handed out once
  // The releases after the binding are not the capture's any more.
  EXPECT_FALSE(c.on_key_up(SDL_SCANCODE_LSHIFT));
}

// The review's repro: a modifier held long enough to auto-repeat, then a key.
// The repeats must neither bind the modifier nor change the chosen key.
TEST(JoyKeyCapture, AutoRepeatIsIgnored) {
  JoyKeyCapture c;
  c.arm(0);
  c.on_key_down(SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT, false);
  EXPECT_TRUE(c.on_key_down(SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT, true));
  EXPECT_TRUE(c.on_key_down(SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT, true));
  EXPECT_TRUE(c.armed());
  EXPECT_FALSE(c.take_binding().has_value());
  c.on_key_down(SDL_SCANCODE_SEMICOLON, SDL_KMOD_LSHIFT, false);
  JoyKeyBinding const b = take(c);
  EXPECT_EQ(SDL_SCANCODE_SEMICOLON, b.scancode);
  EXPECT_EQ(SDL_KMOD_LSHIFT, b.mods);
}

TEST(JoyKeyCapture, RepeatOfAKeyHeldBeforeArmingIsIgnored) {
  JoyKeyCapture c;
  c.arm(1);
  EXPECT_TRUE(c.on_key_down(SDL_SCANCODE_X, SDL_KMOD_NONE, /*repeat=*/true));
  EXPECT_TRUE(c.on_key_up(SDL_SCANCODE_X));  // its release binds nothing
  EXPECT_TRUE(c.armed());
  EXPECT_FALSE(c.take_binding().has_value());
}

TEST(JoyKeyCapture, ModifierAloneBindsOnRelease) {
  JoyKeyCapture c;
  c.arm(4);
  c.on_key_down(SDL_SCANCODE_RCTRL, SDL_KMOD_RCTRL, false);
  c.on_key_up(SDL_SCANCODE_RCTRL);
  JoyKeyBinding const b = take(c);
  EXPECT_EQ(4, b.slot);
  EXPECT_EQ(SDL_SCANCODE_RCTRL, b.scancode);
  EXPECT_EQ(SDL_KMOD_NONE, b.mods);
}

TEST(JoyKeyCapture, ModifierHeldBeforeArmingDoesNotBindOnRelease) {
  JoyKeyCapture c;
  c.arm(2);
  c.on_key_up(SDL_SCANCODE_LSHIFT);
  EXPECT_TRUE(c.armed());
  EXPECT_FALSE(c.take_binding().has_value());
}

TEST(JoyKeyCapture, EscapeCancels) {
  JoyKeyCapture c;
  c.arm(5);
  EXPECT_TRUE(c.on_key_down(SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, false));
  EXPECT_FALSE(c.armed());
  EXPECT_FALSE(c.take_binding().has_value());
}

// Finding #3: collapse, mode change, tab change, Settings closed or reverted
// all mean a frame without the slot buttons, and that frame disarms.
TEST(JoyKeyCapture, FrameWithoutTheButtonsDisarms) {
  JoyKeyCapture c;
  c.arm(0);
  c.mark_shown();
  c.end_frame();
  EXPECT_TRUE(c.armed());
  c.end_frame();  // the buttons were not drawn this frame
  EXPECT_FALSE(c.armed());
  EXPECT_FALSE(c.on_key_down(SDL_SCANCODE_A, SDL_KMOD_NONE, false));
}

TEST(JoyKeyCapture, UntakenBindingDiesWithTheButtons) {
  JoyKeyCapture c;
  c.arm(0);
  c.on_key_down(SDL_SCANCODE_A, SDL_KMOD_NONE, false);
  c.end_frame();
  EXPECT_FALSE(c.take_binding().has_value());
}

TEST(JoyKeyCapture, ArmingAgainDropsTheOldCapture) {
  JoyKeyCapture c;
  c.arm(0);
  c.on_key_down(SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT, false);
  c.arm(1);
  c.on_key_up(SDL_SCANCODE_LSHIFT);  // pending modifier belonged to slot 0
  EXPECT_TRUE(c.armed());
  EXPECT_EQ(1, c.slot());
  EXPECT_FALSE(c.take_binding().has_value());
}
