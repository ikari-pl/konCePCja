#include "test_window.h"

#include <cstdlib>

#include "macos_menu.h"
#include "quit_policy.h"

namespace {
bool g_test_window = false;
}  // namespace

void koncpc_test_window_init_from_env() {
  g_test_window = koncpc_env_flag_on(std::getenv("KONCPC_TEST_WINDOW"));
  if (!g_test_window) return;
  // macOS: run as a background app, so launching never activates it and
  // pulls keyboard focus away from whatever the person is typing in.
  SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
}

bool koncpc_test_window_active() { return g_test_window; }

SDL_WindowFlags koncpc_test_window_flags() {
  return g_test_window ? SDL_WINDOW_NOT_FOCUSABLE : 0;
}

void koncpc_test_window_apply(SDL_Window* window) {
  if (!g_test_window || window == nullptr) return;
  SDL_SetWindowOpacity(window, 0.6f);
  koncpc_set_window_click_through(window);  // SDL3 has no flag for it
}

bool koncpc_is_host_input_event(std::uint32_t type) {
  // Keyboard (0x300), mouse (0x400), joystick and gamepad (0x600), touch
  // (0x700); clipboard starts the next block at 0x900.
  if (type >= SDL_EVENT_KEY_DOWN && type < SDL_EVENT_CLIPBOARD_UPDATE)
    return true;
  // Drag-and-drop: a dropped file loads media.
  if (type >= SDL_EVENT_DROP_FILE && type < SDL_EVENT_AUDIO_DEVICE_ADDED)
    return true;
  // Pen.
  return type >= SDL_EVENT_PEN_PROXIMITY_IN &&
         type < SDL_EVENT_CAMERA_DEVICE_ADDED;
}

bool koncpc_test_window_drops_event(std::uint32_t type) {
  return g_test_window && koncpc_is_host_input_event(type);
}
