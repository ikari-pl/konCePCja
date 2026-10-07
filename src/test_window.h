#pragma once

#include <cstdint>

#include "SDL3/SDL.h"

// KONCPC_TEST_WINDOW=1: the GUI build runs for a test script, next to a person
// using the same desktop. The e2e suites that need a real video driver
// (scr_style, dsk) type into the CPC through autotype; a window that takes
// focus also takes that person's keystrokes, and they land in the CPC's
// keyboard matrix mid-test. In this mode every window stays out of the way:
// the app never activates (macOS), windows can't take focus or clicks and are
// 40% transparent, and host input events never reach the emulator. Autotype,
// IPC and telnet input don't travel as SDL events, so scripts are unaffected.

// Read KONCPC_TEST_WINDOW once, before SDL_Init (main()).
void koncpc_test_window_init_from_env();
bool koncpc_test_window_active();

// Extra SDL_CreateWindow flags for the main window (0 when inactive).
SDL_WindowFlags koncpc_test_window_flags();
// After the main window exists: opacity and, on macOS, click-through.
void koncpc_test_window_apply(SDL_Window* window);

// Keyboard, text, mouse, joystick, gamepad, touch, pen and drag-and-drop
// events: input that comes from whoever sits at the host.
bool koncpc_is_host_input_event(std::uint32_t type);
// True when the event loop must drop this event (mode on and host input).
bool koncpc_test_window_drops_event(std::uint32_t type);
