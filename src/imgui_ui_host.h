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
};

// Install the ImGuiUiHost as the process-wide IUiHost.  Call once, early
// in koncpc_main(), before any ui_host() use.  Idempotent.
//
// This MUST be an explicit call rather than a file-scope static
// constructor.  koncepcja_lib is a STATIC library (CMakeLists.txt), and a
// linker only pulls a member object out of a static library when it
// resolves a referenced symbol.  A TU whose sole entry point is a
// self-registering static ctor references nothing, so MSVC discards
// imgui_ui_host.obj entirely: the ctor never runs, ui_host() keeps
// returning NullUiHost, and the whole ImGui UI silently stops receiving
// input while still rendering.  The GNU/macOS makefile build links every
// object directly and so never exhibited this — it was Windows-only.
// Having koncpc_main() call this by name is what forces the object in.
void install_imgui_ui_host();
