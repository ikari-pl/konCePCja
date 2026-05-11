// IUiHost — abstract contract for the emulator's UI module.
//
// Background: kon_cpc_ja.cpp (the main loop) and the IPC servers should not
// reference ImGui or SDL_GPU directly.  Today they do — they call
// ImGui_ImplSDL3_ProcessEvent on every SDL event, read ImGui::GetIO() to
// check wants-capture-keyboard, and write telemetry into a global
// imgui_state struct.
//
// This header introduces a virtual interface that captures the *minimal*
// surface those callers need.  Two concrete implementations will follow:
//
//   - ImGuiUiHost  — the existing modern UI, ImGui + SDL_GPU.
//   - NullUiHost   — headless: events ignored, no capture, no rendering.
//                    Used by the koncpc-core build (P1.5.2) which has
//                    no ImGui linked in at all.
//
// IMPORTANT non-goals for this header:
//
//   * It does NOT cover GPU rendering (PrepareDrawData/RenderDrawData).
//     Those are only called from inside the SDL_GPU video plugins
//     (src/video.cpp), which themselves are MODERN_UI-only code.  Headless
//     builds never include video.cpp at all, so there's no need for the
//     interface to abstract over GPU rendering.
//
//   * It does NOT replace the global imgui_state struct.  That struct is a
//     publish/subscribe data bus: the main loop writes telemetry samples
//     (frame_time_avg_us, audio_queue_min_ms, drive_a_led, …) and reads
//     UI-set flags (show_devtools, request_cpc_screen_focus, …).  Both
//     sides can use it whether or not an actual UI is present, so it stays
//     as a free-standing struct.  Future work can move imgui_state into
//     the host if there's a reason; there isn't one yet.
//
// Phase: P1.5.1 (beads-1az).  First sub-PR is interface-only — no callers
// rewired yet, no headless build target wired up.  Subsequent sub-PRs in
// this phase move callsites onto IUiHost incrementally, smallest first.

#pragma once

#include <string>

union SDL_Event;

// Severity for `toast()`.  Matches ImGuiUIState::ToastLevel value-for-value
// so wrapping the existing toast helpers is a zero-cost cast — but we don't
// pull in imgui_ui.h here, so headless TUs don't grow an ImGui dependency.
enum class UiToastLevel {
    Info    = 0,
    Success = 1,
    Warning = 2,
    Error   = 3,
};

class IUiHost {
  public:
    virtual ~IUiHost() = default;

    // -- Event input ----------------------------------------------------
    // Called once per SDL_PollEvent.  The host decides whether the event
    // is consumed by UI widgets (modal dialogs, text input, etc.).
    virtual void process_event(const SDL_Event& ev) = 0;

    // -- Query (input-capture state) ------------------------------------
    // True when the UI wants to swallow keyboard / mouse input — main
    // loop must NOT forward those events to the CPC.  Mirrors
    // ImGui::GetIO().WantCaptureKeyboard / WantCaptureMouse.
    virtual bool wants_capture_keyboard() const = 0;
    virtual bool wants_capture_mouse() const = 0;

    // True when any UI element with a focused text/input field is active
    // (modal text input, command palette, address bar in devtools, etc.).
    // Today this is the existing free function `imgui_any_keyboard_ui_active()`.
    virtual bool any_keyboard_ui_active() const = 0;

    // -- Feedback (best-effort) -----------------------------------------
    // Show a transient toast message to the user.  On NullUiHost this
    // logs to stderr so headless runs still surface diagnostics.
    virtual void toast(UiToastLevel level, const std::string& message) = 0;
};

// Returns a process-wide singleton chosen at build time:
//   - MODERN_UI build → returns the ImGui-backed host.
//   - Headless build (P1.5.2)   → returns the null host.
// The pointer is non-owning; the host lives for the duration of the
// process.  Safe to call before any UI init — null host responds with
// safe defaults until the modern host is initialised.
IUiHost& ui_host();

// Test-only: install a custom host for unit tests.  Restores the
// previous host when the returned scope-guard is destroyed.  No-op
// outside tests (the prod factory is statically chosen).
class UiHostOverride {
  public:
    explicit UiHostOverride(IUiHost* test_host);
    ~UiHostOverride();
    UiHostOverride(const UiHostOverride&) = delete;
    UiHostOverride& operator=(const UiHostOverride&) = delete;
  private:
    IUiHost* previous_;
};
