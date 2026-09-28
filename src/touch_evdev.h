#pragma once
// Touch input for panels where SDL has no input backend.
//
// Under SDL_VIDEODRIVER=offscreen SDL neither opens input devices nor
// generates events, so the UI would be unreachable. This reads the kernel's
// evdev touchscreen directly and feeds ImGui's IO, which is all the handheld
// UI needs (ImGui treats a single touch as the left mouse button).
//
// Enabled with KONCPC_TOUCH=<device>, e.g. /dev/input/event1. A no-op when
// unset or on platforms without evdev.
void touch_evdev_poll(int surface_w, int surface_h);
void touch_evdev_shutdown();
