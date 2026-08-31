// ImGuiUiHost — concrete IUiHost implementation backed by Dear ImGui
// and the SDL3 platform backend.  Compiled only in MODERN_UI builds.
//
// The host is a thin wrapper around existing free functions in
// imgui_ui.cpp + the imgui_impl_sdl3 backend.  It exists to:
//   1. Give kon_cpc_ja.cpp's event pump an interface to call so that
//      the main loop stops referencing ImGui directly.
//   2. Provide a single installation point (see imgui_ui_host.cpp)
//      that replaces NullUiHost at startup.
//
// Phase: P1.5.1 sub-PR 2 (beads-1az).

#pragma once

#include "iui_host.h"

class ImGuiUiHost final : public IUiHost {
 public:
  void process_event(const SDL_Event& ev) override;
  bool wants_capture_keyboard() const override;
  bool wants_capture_mouse() const override;
  bool any_keyboard_ui_active() const override;
  void toast(UiToastLevel level, const std::string& message) override;
  int topbar_height() const override;
  void set_display_scale(float scale) override;
};

// Install the ImGuiUiHost as the process-wide IUiHost.  Call once, early in
// koncpc_main(), before any ui_host() use.  Idempotent.
//
// koncepcja_lib is a STATIC library, and a linker pulls a member object out
// of one only to resolve a referenced symbol.  koncpc_main() calling this by
// name is what keeps imgui_ui_host.obj in the link, and therefore what makes
// ui_host() return this host on MSVC.
void install_imgui_ui_host();
