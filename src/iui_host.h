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
// What this header does NOT cover:
//
//   * GPU rendering of the CPC image itself.  The video plugins
//     (src/video_host.cpp) own the window, the swapchain and the CPC blit.
//     They hand the UI a command buffer / render pass / renderer to draw
//     its chrome into through the render-layer hooks below (beads-cv2.6);
//     a host without chrome leaves those as no-ops and the plugins present
//     the CPC image alone.
//
//   * The UI request flags in imgui_state (show_devtools, show_menu,
//     fullscreen_request, ...).  Those link in every build; the telemetry
//     the main loop writes (frame timing, audio queue, drive LEDs, tape
//     scopes) lives in host_state.h (beads-cv2.5).
//
// Phase: P1.5.1 (beads-1az).  First sub-PR is interface-only — no callers
// rewired yet, no headless build target wired up.  Subsequent sub-PRs in
// this phase move callsites onto IUiHost incrementally, smallest first.

#pragma once

#include <cstdint>
#include <string>

union SDL_Event;
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_FRect;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
// imgui_state.h owns the enumerators; the opaque declaration keeps that
// header (and the ImGui-side state it carries) out of this one.
enum class FileDialogAction : std::uint8_t;

// Severity for `toast()`.  Values match ImGuiUIState::ToastLevel
// (defined in src/imgui_ui.h:110) one-for-one so wrapping the existing
// toast helpers is a zero-cost cast — but we don't pull in imgui_ui.h
// here, so headless TUs don't grow an ImGui dependency.  Keep this in
// sync if the underlying enum gains a new level.
enum class UiToastLevel : std::uint8_t {
  Info = 0,
  Success = 1,
  Error = 2,
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

  // -- Layout query ---------------------------------------------------
  // Pixel height of the modern UI's top bar (menu + status row).  The
  // CPC framebuffer renders below it, so the main loop subtracts this
  // when computing mouse-over-topbar regions.  Returns 0 on NullUiHost
  // so headless callers see the full window as the CPC viewport.
  virtual int topbar_height() const = 0;

  // -- Display scale --------------------------------------------------
  // Tell the UI the display content scale (1.0 = 100%, 2.25 = 225%), at
  // startup and whenever SDL reports SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED.
  // The ImGui host scales its chrome by this; the CPC image keeps its own
  // scaling in video_host.cpp.  Defaults to a no-op, which hosts without
  // chrome inherit.
  virtual void set_display_scale(float /*scale*/) {}

  // -- Debugger UI (DevTools) — beads-cv2.4 ---------------------------
  // The IPC server reaches DevTools only through these.  A host without a
  // debugger UI keeps the no-op defaults, and IPC 'devtools' answers
  // ERR 503 no-ui there instead of claiming success.
  virtual bool has_debugger_ui() const { return false; }
  // Show or hide the DevTools toolbar; hiding also closes its windows.
  virtual void set_debugger_visible(bool /*visible*/) {}
  // Open or close one DevTools window by key (registers, disasm, …).
  // False when the key names no window.
  virtual bool set_debugger_window_open(const std::string& /*name*/,
                                        bool /*open*/) {
    return false;
  }
  // Guest memory changed behind the debugger's back (IPC mem write/fill):
  // cached disassembly is stale.  Callable from any thread.
  virtual void debugger_memory_changed() {}
  // The symbol table changed (IPC sym load/add/del).  Any thread.
  virtual void debugger_symbols_changed() {}

  // -- Settings dialog & window requests (main thread) ----------------
  // True while a fullscreen toggle the UI posted (menu, F2, Settings) is
  // still waiting for the main loop; IPC's staged request waits behind it.
  virtual bool fullscreen_request_pending() const { return false; }
  // Ask the main loop to switch the window to `scr_window` (1 = windowed,
  // 0 = fullscreen) on its next pass.
  virtual void request_fullscreen(unsigned /*scr_window*/) {}
  // A setting changed outside the Settings dialog.  While the dialog is
  // open its Cancel restores the values it saw on opening; fold the change
  // into that baseline so Cancel does not silently revert it.
  virtual void settings_baseline_set_kbd_layout(const std::string& /*name*/) {}
  virtual void settings_baseline_set_scr_window(unsigned /*scr_window*/) {}

  // -- Main-loop requests (main thread) — beads-cv2.6 -----------------
  // Host chords (Cmd/Ctrl+K, O, S) and menu items the UI owns.
  virtual void toggle_command_palette() {}
  virtual void request_file_dialog(FileDialogAction /*action*/) {}
  // F5 with unsaved disk edits: true when the UI took the request and will
  // ask the user; false means nobody can ask, so the caller resets now.
  virtual bool request_reset_confirmation() { return false; }
  // The video plugin is about to be torn down and rebuilt: drop textures
  // the UI created on the old render device before it disappears.
  virtual void release_video_textures() {}
  // Shutdown: wait for UI-started worker threads that still touch the
  // machine (a DevTools Step Out walk) before the teardown pauses it.
  virtual void await_background_work() {}

  // -- Render layer (render thread) — beads-cv2.6 ---------------------
  // The video plugins drive the UI's draw backend through these, in this
  // order each frame: *_prepare_frame, then *_draw inside the plugin's own
  // pass, then (SDL_GPU only) render_detached_windows after the submit.
  // The defaults are a host without chrome: attaching succeeds and draws
  // nothing.
  //
  // SDL_GPU backend, bound to the g_gpu device (video_gpu.h) the plugin
  // created on `window`.  `viewports` lets UI windows detach into their own
  // OS windows.  `display_scale` is the desktop content scale, as for
  // set_display_scale().  False: the plugin must tear down and fail.
  virtual bool gpu_attach(SDL_Window* /*window*/, bool /*viewports*/,
                          float /*display_scale*/) {
    return true;
  }
  // Build this frame's UI and upload its vertex data on `cmd`.  SDL_GPU
  // forbids copy passes inside a render pass, so this must precede
  // SDL_BeginGPURenderPass.
  virtual void gpu_prepare_frame(SDL_GPUCommandBuffer* /*cmd*/) {}
  // Draw the prepared UI into the main window's render pass.
  virtual void gpu_draw(SDL_GPUCommandBuffer* /*cmd*/,
                        SDL_GPURenderPass* /*pass*/) {}
  // After the main command buffer is submitted: render detached windows.
  virtual void render_detached_windows() {}
  // Release the backend.  Needs the device alive; safe when not attached.
  virtual void gpu_detach() {}

  // SDL_Renderer backend (no detached windows).
  virtual bool renderer_attach(SDL_Window* /*window*/,
                               SDL_Renderer* /*renderer*/,
                               float /*display_scale*/) {
    return true;
  }
  // Build this frame's UI.  `background` (nullable) is the CPC image to sit
  // beneath it at `dst`, in main-window coordinates.  True when the UI
  // queued the background; false leaves drawing it to the caller.
  virtual bool renderer_prepare_frame(SDL_Texture* /*background*/,
                                      const SDL_FRect& /*dst*/) {
    return false;
  }
  virtual void renderer_draw(SDL_Renderer* /*renderer*/) {}
  virtual void renderer_detach() {}
};

// Returns a process-wide singleton chosen at build time:
//   - MODERN_UI build → returns the ImGui-backed host.
//   - Headless build (P1.5.2)   → returns the null host.
// The pointer is non-owning; the host lives for the duration of the
// process.  Safe to call before any UI init — null host responds with
// safe defaults until the modern host is installed.
IUiHost& ui_host();

// Install a concrete IUiHost as the process-wide implementation.  The
// caller retains ownership; the host pointer must outlive any call to
// ui_host() that returns it.  Passing nullptr restores the null host.
// Returns the previously installed host (or null host if first call).
//
// Use from production code — for example, the modern-UI build installs
// an ImGuiUiHost via a static constructor in src/imgui_ui_host.cpp.
// Tests should prefer UiHostOverride (below) for scoped install/restore.
IUiHost* install_ui_host(IUiHost* host);

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
