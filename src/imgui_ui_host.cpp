// ImGuiUiHost — concrete IUiHost wired to Dear ImGui and SDL3.
//
// Each method delegates to the existing free function or backend call
// that the rest of the codebase used directly before P1.5.1.  Two
// things to be careful of:
//
//   1. ImGui::GetIO() requires an active ImGui context (created in
//      imgui_init_ui() during video plugin init).  Before that point
//      ImGui::GetCurrentContext() is null and any GetIO call segfaults.
//      Every method here guards with GetCurrentContext() and returns a
//      safe default if the context isn't up yet — matches NullUiHost
//      semantics so pre-init code paths don't crash.
//
//   2. This file is part of the MODERN_UI build only.  In the future
//      headless build (P1.5.2) it won't be compiled, and the NullUiHost
//      stays installed as the default.
//
// Phase: P1.5.1 sub-PR 2 (beads-1az).

#include "imgui_ui_host.h"

#include <SDL3/SDL_events.h>

#include <cstdio>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_ui.h"

namespace {

// Map our backend-agnostic UiToastLevel to the ImGui-side enum so we
// don't leak that enum across the iui_host.h boundary.  Values are 1:1
// (UiToastLevel was deliberately defined to match) — this switch is
// just defensive in case either enum gains members later.
ImGuiUIState::ToastLevel to_imgui_toast_level(UiToastLevel level) {
  switch (level) {
    case UiToastLevel::Info:
      return ImGuiUIState::ToastLevel::Info;
    case UiToastLevel::Success:
      return ImGuiUIState::ToastLevel::Success;
    case UiToastLevel::Error:
      return ImGuiUIState::ToastLevel::Error;
  }
  return ImGuiUIState::ToastLevel::Info;
}

}  // namespace

void ImGuiUiHost::process_event(const SDL_Event& ev) {
  if (!ImGui::GetCurrentContext()) {
    return;  // Pre-init: drop the event.  The main loop's emulator
             // dispatch still runs below this call in kon_cpc_ja.cpp.
  }
  ImGui_ImplSDL3_ProcessEvent(&ev);
}

bool ImGuiUiHost::wants_capture_keyboard() const {
  if (!ImGui::GetCurrentContext()) return false;
  return ImGui::GetIO().WantCaptureKeyboard;
}

bool ImGuiUiHost::wants_capture_mouse() const {
  if (!ImGui::GetCurrentContext()) return false;
  return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiUiHost::any_keyboard_ui_active() const {
  if (!ImGui::GetCurrentContext()) return false;
  return imgui_any_keyboard_ui_active();
}

void ImGuiUiHost::toast(UiToastLevel level, const std::string& message) {
  if (!ImGui::GetCurrentContext()) {
    // No UI to show on — fall through to stderr like NullUiHost.
    // Otherwise diagnostics during cold startup vanish silently.
    const char* tag = "info";
    switch (level) {
      case UiToastLevel::Info:
        tag = "info";
        break;
      case UiToastLevel::Success:
        tag = "success";
        break;
      case UiToastLevel::Error:
        tag = "error";
        break;
    }
    std::fprintf(stderr, "[toast/%s] %s\n", tag, message.c_str());
    return;
  }
  imgui_toast(message, to_imgui_toast_level(level));
}

int ImGuiUiHost::topbar_height() const {
  // imgui_topbar_height() lives in imgui_ui.cpp and caches the live
  // menubar+statusbar measurements.  Safe pre-init: returns 0 before
  // the first frame.
  return imgui_topbar_height();
}

// -- Install at startup ----------------------------------------------
//
// A file-scope global + static-init pair replaces the NullUiHost default
// with our ImGuiUiHost before main() runs.  This means:
//   - kon_cpc_ja.cpp:main can call ui_host() from the very first line
//     and get the right impl.
//   - We don't need to plumb an explicit init point through the existing
//     startup sequence (argparse → emulator_init → video_init → ...).
//   - The headless build (P1.5.2) just doesn't compile this TU, so the
//     null host stays installed.
namespace {
ImGuiUiHost g_imgui_host_instance;
struct AutoInstaller {
  AutoInstaller() { install_ui_host(&g_imgui_host_instance); }
};
[[maybe_unused]] AutoInstaller g_auto_installer;
}  // namespace
