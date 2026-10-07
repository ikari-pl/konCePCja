// beads-1tsu: KONCPC_TEST_WINDOW drops every event that comes from the person
// at the host, and nothing else (quit, window and device events still flow).

#include "test_window.h"

#include <gtest/gtest.h>

#include "quit_policy.h"

TEST(TestWindow, HostInputEventsAreClassifiedAsHostInput) {
  for (std::uint32_t type :
       {SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP, SDL_EVENT_TEXT_INPUT,
        SDL_EVENT_TEXT_EDITING, SDL_EVENT_MOUSE_MOTION,
        SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP,
        SDL_EVENT_MOUSE_WHEEL, SDL_EVENT_JOYSTICK_AXIS_MOTION,
        SDL_EVENT_JOYSTICK_BUTTON_DOWN, SDL_EVENT_GAMEPAD_BUTTON_DOWN,
        SDL_EVENT_GAMEPAD_AXIS_MOTION, SDL_EVENT_FINGER_DOWN,
        SDL_EVENT_DROP_FILE, SDL_EVENT_DROP_TEXT, SDL_EVENT_PEN_DOWN}) {
    EXPECT_TRUE(koncpc_is_host_input_event(type)) << std::hex << type;
  }
}

TEST(TestWindow, NonInputEventsAreNotHostInput) {
  for (std::uint32_t type :
       {SDL_EVENT_QUIT, SDL_EVENT_WINDOW_CLOSE_REQUESTED,
        SDL_EVENT_WINDOW_RESIZED, SDL_EVENT_WINDOW_FOCUS_LOST,
        SDL_EVENT_CLIPBOARD_UPDATE, SDL_EVENT_AUDIO_DEVICE_ADDED,
        SDL_EVENT_RENDER_TARGETS_RESET, SDL_EVENT_USER}) {
    EXPECT_FALSE(koncpc_is_host_input_event(type)) << std::hex << type;
  }
}

TEST(TestWindow, InactiveModeDropsNothing) {
  // test_runner never sets KONCPC_TEST_WINDOW, and main() is the only
  // caller of koncpc_test_window_init_from_env().
  ASSERT_FALSE(koncpc_test_window_active());
  EXPECT_FALSE(koncpc_test_window_drops_event(SDL_EVENT_KEY_DOWN));
  EXPECT_EQ(SDL_WindowFlags{0}, koncpc_test_window_flags());
}

TEST(TestWindow, EnvFlagSpellings) {
  EXPECT_FALSE(koncpc_env_flag_on(nullptr));
  EXPECT_FALSE(koncpc_env_flag_on(""));
  EXPECT_FALSE(koncpc_env_flag_on("0"));
  EXPECT_FALSE(koncpc_env_flag_on("Off"));
  EXPECT_TRUE(koncpc_env_flag_on("1"));
  EXPECT_TRUE(koncpc_env_flag_on("yes"));
}
