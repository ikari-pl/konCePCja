#ifndef IMGUI_UI_H
#define IMGUI_UI_H

// Modern-UI public surface — function declarations for the ImGui +
// SDL_GPU build.  When KONCPC_BUILD_MODERN_UI=OFF the implementations
// in imgui_ui.cpp / devtools_ui.cpp / video.cpp are excluded, so this
// header is only included from translation units that themselves are
// part of the modern-UI compile.
//
// The data side of the UI (the `imgui_state` singleton + telemetry
// fields the core writes into) lives in `imgui_state.h`, which is
// headless-safe.  This header re-exports `imgui_state.h` so existing
// modern-UI callers keep compiling unchanged.

#include "imgui_state.h"

void imgui_init_ui();
void imgui_render_ui();
int imgui_topbar_height();
void imgui_close_menu();

// Returns true when any keyboard-consuming UI is active (menus, dialogs,
// text fields, popups, devtools). Single source of truth — used by both
// the keyboard capture policy in imgui_ui.cpp and the event filter in
// kon_cpc_ja.cpp. Does NOT include the virtual keyboard (mouse-only).
bool imgui_any_keyboard_ui_active();

// Toast notifications
void imgui_toast(const std::string& message, ImGuiUIState::ToastLevel level = ImGuiUIState::ToastLevel::Info);

// MRU (recent files) helper — pushes path to front, deduplicates, caps at MRU_MAX
void imgui_mru_push(std::vector<std::string>& list, const std::string& path);

// tape_scan_blocks() moved to tape.h (callable from headless builds).

void imgui_toast_info(const std::string& message);
void imgui_toast_success(const std::string& message);
void imgui_toast_error(const std::string& message);

// Serial Terminal window
void imgui_render_serial_terminal();
void serial_terminal_feed_byte(uint8_t byte);

// Plotter Preview window
void imgui_render_plotter_preview();

#endif // IMGUI_UI_H
