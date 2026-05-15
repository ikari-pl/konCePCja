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
    bool wants_capture_mouse()    const override;
    bool any_keyboard_ui_active() const override;
    void toast(UiToastLevel level, const std::string& message) override;
    int topbar_height() const override;
};
