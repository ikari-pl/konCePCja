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
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_render.h>

#include <cstdio>
#include <string>

#include "command_palette.h"
#include "devtools_ui.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_ui.h"
#include "koncepcja.h"  // getConfigurationFilename
#include "menu_bridge.h"
#include "video_gpu.h"

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

// Path for imgui.ini in the same directory as koncepcja.cfg.  A static
// string, so the c_str() pointer stays valid for io.IniFilename.
const char* imgui_ini_path() {
  static std::string path;
  if (path.empty()) {
    std::string const cfg = getConfigurationFilename();
    if (!cfg.empty()) {
      auto slash = cfg.find_last_of('/');
      path = (slash != std::string::npos ? cfg.substr(0, slash + 1) : "") +
             "imgui.ini";
    }
  }
  return path.empty() ? nullptr : path.c_str();
}

// The context every backend starts from.  The SDL_Renderer backend does not
// support multi-viewport, so only the SDL_GPU attach may ask for it.
void create_imgui_context(bool viewports) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = imgui_ini_path();
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
  if (viewports) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  ImGui::StyleColorsDark();
  imgui_init_ui();
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
  // Error toasts ALWAYS mirror to stderr — a GUI-only error is invisible to
  // logs, headless runs, and CI, and can't be diagnosed after the fact.
  // Without an ImGui context (cold startup) every level mirrors so early
  // diagnostics aren't lost either.
  if (level == UiToastLevel::Error || !ImGui::GetCurrentContext()) {
    std::fprintf(stderr, "[toast/%s] %s\n", tag, message.c_str());
  }
  if (ImGui::GetCurrentContext()) {
    imgui_toast(message, to_imgui_toast_level(level));
  }
}

void ImGuiUiHost::set_display_scale(float scale) {
  if (!ImGui::GetCurrentContext()) return;
  if (!(scale > 0.0F)) return;  // also rejects NaN

  // Fonts: ImGui 1.92 rasterises on demand at the size actually needed, so
  // FontScaleDpi alone resizes the text.
  //
  // Sizes: padding and spacing scale alongside the text, so the chrome keeps
  // its proportions.  ScaleAllSizes() multiplies in place, so the unscaled
  // style is captured once and every call re-derives from that copy; this
  // stays idempotent and follows the scale back down again.  Colours come
  // from the live style, preserving a theme change made after the capture.
  static ImGuiStyle s_base_style;
  static bool s_base_captured = false;
  ImGuiStyle& style = ImGui::GetStyle();
  if (!s_base_captured) {
    s_base_style = style;
    s_base_captured = true;
  }

  ImVec4 live_colors[ImGuiCol_COUNT];
  for (int i = 0; i < ImGuiCol_COUNT; ++i) live_colors[i] = style.Colors[i];
  style = s_base_style;
  for (int i = 0; i < ImGuiCol_COUNT; ++i) style.Colors[i] = live_colors[i];

  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;
}

int ImGuiUiHost::topbar_height() const {
  // imgui_topbar_height() lives in imgui_ui.cpp and caches the live
  // menubar+statusbar measurements.  Safe pre-init: returns 0 before
  // the first frame.
  return imgui_topbar_height();
}

// -- Debugger UI (DevTools) ------------------------------------------
//
// None of these touch the ImGui context: they set flags the next render()
// acts on, so they are safe before the first frame and from the IPC thread
// exactly as the direct g_devtools_ui calls they replace were.

void ImGuiUiHost::set_debugger_visible(bool visible) {
  imgui_state.show_devtools = visible;
  if (!visible) g_devtools_ui.close_all_windows();
}

bool ImGuiUiHost::set_debugger_window_open(const std::string& name, bool open) {
  bool* const flag = g_devtools_ui.window_ptr(name);
  if (flag == nullptr) return false;
  *flag = open;
  return true;
}

void ImGuiUiHost::debugger_memory_changed() {
  g_devtools_ui.disasm_cache_invalidate();
}

void ImGuiUiHost::debugger_symbols_changed() {
  g_devtools_ui.symtable_mark_dirty();
}

// -- Settings dialog & window requests -------------------------------

bool ImGuiUiHost::fullscreen_request_pending() const {
  return imgui_state.fullscreen_request != -1;
}

void ImGuiUiHost::request_fullscreen(unsigned scr_window) {
  imgui_state.fullscreen_request = static_cast<int>(scr_window);
}

void ImGuiUiHost::settings_baseline_set_kbd_layout(const std::string& name) {
  if (imgui_state.show_options) imgui_state.old_cpc_settings.kbd_layout = name;
}

void ImGuiUiHost::settings_baseline_set_scr_window(unsigned scr_window) {
  if (imgui_state.show_options) {
    imgui_state.old_cpc_settings.scr_window = scr_window;
  }
}

// -- Main-loop requests ----------------------------------------------

void ImGuiUiHost::toggle_command_palette() { g_command_palette.toggle(); }

void ImGuiUiHost::request_file_dialog(FileDialogAction action) {
  koncpc_request_file_dialog(static_cast<int>(action));
}

bool ImGuiUiHost::request_reset_confirmation() {
  imgui_request_reset_confirmation();
  return true;
}

void ImGuiUiHost::release_video_textures() { imgui_invalidate_slot_thumbs(); }

void ImGuiUiHost::await_background_work() { dbg_step_walk_await_shutdown(); }

// -- Render layer ----------------------------------------------------
//
// Moved verbatim from the video plugins in video_host.cpp, which used to
// carry one copy of each sequence per plugin.

bool ImGuiUiHost::gpu_attach(SDL_Window* window, bool viewports,
                             float display_scale) {
  // SDLGPU3 backend.  With viewports, the renderer hooks in
  // vendor/imgui/backends/imgui_impl_sdlgpu3.cpp claim each secondary window
  // for g_gpu.device on creation and submit a per-viewport command buffer on
  // render.  ImGui_ImplSDLGPU3_Init checks io.ConfigFlags and registers the
  // hooks itself, so order matters: set the flags BEFORE Init.
  create_imgui_context(viewports);
  // Scale the chrome to the desktop scale.  Must follow CreateContext(); the
  // host no-ops without a context.  Only the chrome: the CPC image keeps the
  // user's chosen integer scr_scale so it stays pixel-exact.
  set_display_scale(display_scale);
  ImGui_ImplSDL3_InitForSDLGPU(window);
  ImGui_ImplSDLGPU3_InitInfo init_info{};
  init_info.Device = g_gpu.device;
  init_info.ColorTargetFormat = g_gpu.swapchain_fmt;
  init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
  // VSYNC — do NOT switch this to IMMEDIATE: ImGui viewport windows inherit
  // this present mode, and IMMEDIATE breaks their swapchain creation, so
  // detached DevTools windows fail to become separate OS windows (they get
  // clipped inside the main window). The multi-second present stall over remote
  // desktop is fixed properly by decoupling emulation from render (so emulation
  // never waits on present), NOT by the present mode. Any configurable
  // video.vsync must apply only to the MAIN window, with a per-window
  // SDL_WindowSupportsGPUPresentMode check before touching viewport swapchains.
  init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
  if (!ImGui_ImplSDLGPU3_Init(&init_info)) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    return false;
  }
  return true;
}

void ImGuiUiHost::gpu_prepare_frame(SDL_GPUCommandBuffer* cmd) {
  // The CPC image is NOT pushed into the background draw list here, unlike
  // the SDL_Renderer path: the plugin's own blit is authoritative because it
  // picks the sampler (linear vs nearest) per scr_crt_aspect.  Docked mode's
  // CPC Screen window pulls the texture via video_get_cpc_texture().
  ImGui_ImplSDLGPU3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
  imgui_render_ui();
  ImGui::Render();
  ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);
}

void ImGuiUiHost::gpu_draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
  ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);
}

void ImGuiUiHost::render_detached_windows() {
  // The renderer hooks in imgui_impl_sdlgpu3.cpp acquire + submit one
  // command buffer per viewport.  RenderPlatformWindowsDefault skips the
  // main viewport (already rendered) so there's no double-render race.
  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
  }
}

void ImGuiUiHost::gpu_detach() {
  if (ImGui::GetCurrentContext()) {
    ImGui_ImplSDLGPU3_Shutdown();  // releases bd state, still needs device
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
}

bool ImGuiUiHost::renderer_attach(SDL_Window* window, SDL_Renderer* renderer,
                                  float display_scale) {
  create_imgui_context(/*viewports=*/false);
  set_display_scale(display_scale);
  if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
    ImGui::DestroyContext();
    return false;
  }
  if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    return false;
  }
  return true;
}

bool ImGuiUiHost::renderer_prepare_frame(SDL_Texture* background,
                                         const SDL_FRect& dst) {
  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
  if (background != nullptr) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList(vp)->AddImage(
        reinterpret_cast<ImTextureID>(background),
        ImVec2(vp->Pos.x + dst.x, vp->Pos.y + dst.y),
        ImVec2(vp->Pos.x + dst.x + dst.w, vp->Pos.y + dst.y + dst.h));
  }
  imgui_render_ui();
  ImGui::Render();
  return background != nullptr;
}

void ImGuiUiHost::renderer_draw(SDL_Renderer* renderer) {
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

void ImGuiUiHost::renderer_detach() {
  if (ImGui::GetCurrentContext()) {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
}

// -- Install at startup ----------------------------------------------
//
// Replaces the NullUiHost default with our ImGuiUiHost.  koncpc_main()
// calls this explicitly; see the comment on the declaration in
// imgui_ui_host.h for why this cannot be a file-scope static ctor.
//
// The headless build (P1.5.2) doesn't compile this TU, so the null host
// stays installed there — the call site is guarded by KONCPC_MODERN_UI.
namespace {
ImGuiUiHost g_imgui_host_instance;
}  // namespace

void install_imgui_ui_host() { install_ui_host(&g_imgui_host_instance); }
