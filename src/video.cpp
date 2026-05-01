/* konCePCja - Amstrad CPC Emulator
   (c) Copyright 1997-2004 Ulrich Doewich

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
   This file includes video filters from the SMS Plus/SDL 
   sega master system emulator :
   (c) Copyright Gregory Montoir
   http://membres.lycos.fr/cyxdown/smssdl/
*/

/*
   This file includes video filters from MAME
   (Multiple Arcade Machine Emulator) :
   (c) Copyright The MAME Team
   http://www.mame.net/
*/

#include "video.h"
#include "koncepcja.h"
#include "log.h"
#include "glfuncs.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <math.h>
#include <memory>
#include <iostream>
#include <unordered_map>
#include "savepng.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_impl_sdlgpu3.h"
#include "imgui_ui.h"
#include "macos_menu.h"
#include "video_gpu.h"
#include "shaders/blit_shaders.h"
#include <cstring>

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

// GL 2.0+/3.0+ types and constants needed by CRT shaders.
// On macOS, <OpenGL/gl3.h> provides these. On Windows/Linux with GL 1.1
// headers only, we define them here.
#ifndef GL_FRAGMENT_SHADER
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_ARRAY_BUFFER                   0x8892
#define GL_STATIC_DRAW                    0x88E4
#define GL_TEXTURE0                       0x84C0
#define GL_FRAMEBUFFER                    0x8D40
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_FRAMEBUFFER_BINDING            0x8CA6
#endif

SDL_Window* mainSDLWindow = nullptr;
SDL_Renderer* renderer = nullptr;
SDL_Texture* texture = nullptr;
SDL_GLContext glcontext;

// OpenGL texture for CPC framebuffer (used by OpenGL3/ImGui rendering path)
static GLuint cpc_gl_texture = 0;
// SDL texture for CPC framebuffer (used by SDL_Renderer/D3D rendering path)
static SDL_Texture* cpc_sdl_texture = nullptr;
// Which ImGui rendering backend is active
static bool using_sdl_renderer = false;

// Returns path for imgui.ini in the same directory as koncepcja.cfg.
// Uses a static string so the c_str() pointer remains valid for io.IniFilename.
static const char* imgui_ini_path() {
  static std::string path;
  if (path.empty()) {
    std::string cfg = getConfigurationFilename();
    if (!cfg.empty()) {
      auto slash = cfg.find_last_of('/');
      path = (slash != std::string::npos ? cfg.substr(0, slash + 1) : "") + "imgui.ini";
    }
  }
  return path.empty() ? nullptr : path.c_str();
}

// the video surface ready to display
SDL_Surface* vid = nullptr;
// the video surface scaled with same format as pub
SDL_Surface* scaled = nullptr;
// the video surface shown by the plugin to the application
SDL_Surface* pub = nullptr;

SDL_Surface* devtools_panel_surface = nullptr;
int devtools_panel_width = 0;
int devtools_panel_height = 0;
int devtools_panel_surface_width = 0;
int devtools_panel_surface_height = 0;
int devtools_cpc_height = 0;

SDL_Surface* topbar_surface = nullptr;
int topbar_height = 0;
static int bottombar_height = 0;

extern t_CPC CPC;
extern video_plugin* vid_plugin;

// Window screenshot: set a pending path for capture by the main thread.
// The capture happens in direct_flip() after ImGui is rendered.
#include <mutex>
#include <vector>
static std::mutex g_wss_mutex;
static std::string g_wss_pending_path;

std::atomic<bool> g_repaint_pending{false};
std::atomic<bool> g_repaint_done{false};
std::mutex g_repaint_mutex;
std::string g_repaint_screenshot_path;
std::string g_repaint_error;

void video_request_window_screenshot(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_wss_mutex);
  g_wss_pending_path = path;
}

// Returns the CPC screen texture as an opaque ImTextureID-compatible value.
// The actual type is backend-dependent: GL texture ID for OpenGL backends,
// SDL_Texture* cast to uintptr_t for SDL_Renderer backends.
// Callers should only use the returned value as an ImTextureID.
uintptr_t video_get_cpc_texture() {
  if (cpc_sdl_texture)
    return reinterpret_cast<uintptr_t>(cpc_sdl_texture);
  // GPU plugin active — ImGui's SDLGPU3 backend accepts SDL_GPUTexture* as
  // an ImTextureID for Docked-mode ImGui::Image() calls on the CPC Screen.
  if (g_gpu.cpc_texture)
    return reinterpret_cast<uintptr_t>(g_gpu.cpc_texture);
  return static_cast<uintptr_t>(cpc_gl_texture);
}

void video_get_cpc_size(int& w, int& h) {
  if (vid) { w = vid->w; h = vid->h; }
  else     { w = 0; h = 0; }
}

bool video_is_sdl_renderer() { return using_sdl_renderer; }

// ── Offscreen texture cache (implementation follows gl3 struct below) ──────

struct OffscreenEntry {
    GLuint fbo = 0;
    GLuint tex = 0;
    int    w   = 0;
    int    h   = 0;
    size_t dirty = ~size_t(0);
};
static std::unordered_map<std::string, OffscreenEntry> g_offscreen_cache;

// Called from direct_flip_a() and swscale_blit_a() to capture the current frame
static void video_capture_if_pending() {
  std::string wss_path;
  {
    std::lock_guard<std::mutex> lock(g_wss_mutex);
    if (g_wss_pending_path.empty()) return;
    wss_path = g_wss_pending_path;
    g_wss_pending_path.clear();
  }

  if (vid) {
    if (SDL_SavePNG(vid, wss_path)) {
      LOG_ERROR("Screenshot: SDL_SavePNG failed for " + wss_path);
    } else {
      LOG_INFO("Screenshot saved to " + wss_path);
    }
  }
}

void video_take_pending_window_screenshot() {
  // no-op: capture happens inside flip handlers via video_capture_if_pending
}

#ifndef min
#define min(a,b) ((a)<(b) ? (a) : (b))
#endif

#ifndef max
#define max(a,b) ((a)>(b) ? (a) : (b))
#endif

// Returns a bpp compatible with the renderer
int renderer_bpp(SDL_Renderer *sdl_renderer)
{
  (void)sdl_renderer;
  return 32;
}

// TODO: Cleanup sw_scaling if really not needed
void compute_scale(video_plugin* t, int w, int h)
{
  int win_width, win_height;
  SDL_GetWindowSize(mainSDLWindow, &win_width, &win_height);
  if (devtools_panel_width > 0) {
    win_width = max(1, win_width - devtools_panel_width);
  }
  if (topbar_height > 0 || bottombar_height > 0) {
    win_height = max(1, win_height - topbar_height - bottombar_height);
  }
  if (devtools_cpc_height > 0) {
    win_height = devtools_cpc_height;
  }
  if (CPC.scr_preserve_aspect_ratio != 0) {
    // Fixed scale (1x-4x): display at exact pixel multiple, centered.
    // Fit (scr_scale=0): fill available space preserving aspect ratio.
    int disp_w, disp_h;
    // Scale factor table: index 0 = Fit (unused here), 1+ = fixed multipliers
    static const float scale_factors[] = { 0.f, 1.f, 1.5f, 2.f, 3.f };
    // Target aspect ratio: 4:3 for CRT monitors, or raw pixel ratio
    float target_aspect = CPC.scr_crt_aspect
        ? (4.f / 3.f)
        : (static_cast<float>(w) / h);
    if (CPC.scr_scale > 0 && CPC.scr_scale < sizeof(scale_factors)/sizeof(scale_factors[0])) {
      // Fixed scale: exact pixel multiple. If window is smaller, image is
      // cropped (centered) — never scaled down.  Options resizes the window
      // to fit when the user picks a new scale.
      float sf = scale_factors[CPC.scr_scale];
      disp_w = static_cast<int>(CPC_RENDER_WIDTH * sf);
      disp_h = static_cast<int>(disp_w / target_aspect);
    } else {
      // Fit window: fill available space preserving target aspect ratio.
      float win_aspect = static_cast<float>(win_width) / win_height;
      if (win_aspect > target_aspect) {
        // Window wider than target — height-limited
        disp_h = win_height;
        disp_w = static_cast<int>(win_height * target_aspect);
      } else {
        // Window taller than target — width-limited
        disp_w = win_width;
        disp_h = static_cast<int>(win_width / target_aspect);
      }
    }
    t->width = disp_w;
    t->height = disp_h;
    // Center in available area — offset can be negative (cropping)
    float x_offset = 0.5f * (win_width - t->width);
    float y_offset = 0.5f * (win_height - t->height);
    if (devtools_panel_width > 0) x_offset = 0;
    if (topbar_height > 0) y_offset += static_cast<float>(topbar_height);
    t->x_offset = x_offset;
    t->y_offset = y_offset;
    t->x_scale = w / static_cast<float>(disp_w);
    t->y_scale = h / static_cast<float>(disp_h);
  } else {
    t->x_offset=0;
    t->y_offset=0;
    t->x_scale=w/static_cast<float>(win_width);
    t->y_scale=h/static_cast<float>(win_height);
    t->width = win_width;
    t->height = win_height;
  }
}

/* ------------------------------------------------------------------------------------ */
/* Half size video plugin ------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */
SDL_Surface* direct_init(video_plugin* t, int scale, bool fs)
{
  // Create OpenGL window
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

  mainSDLWindow = SDL_CreateWindow("konCePCja " VERSION_STRING,
      CPC_RENDER_WIDTH * scale, CPC_VISIBLE_SCR_HEIGHT * scale,
      (fs ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!mainSDLWindow) return nullptr;

  glcontext = SDL_GL_CreateContext(mainSDLWindow);
  if (!glcontext) return nullptr;
  SDL_GL_MakeCurrent(mainSDLWindow, glcontext);
  // Disable vsync: the emulator's own speed limiter handles frame pacing.
  // Vsync causes SDL_GL_SwapWindow to block when macOS throttles background apps,
  // which prevents IPC screenshot capture from working.
  // macOS forces compositor-level vsync anyway, so no tearing in practice.
  SDL_GL_SetSwapInterval(0);

  // Initialize Dear ImGui
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = imgui_ini_path();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  ImGui::StyleColorsDark();
  imgui_init_ui();
  ImGui_ImplSDL3_InitForOpenGL(mainSDLWindow, glcontext);
  if (!ImGui_ImplOpenGL3_Init("#version 150")) {
    // OpenGL loader failed (e.g. driver only exposes GL 1.1)
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(glcontext);
    glcontext = nullptr;
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
    return nullptr;
  }

  // Always render at native Mode 2 width. Y doubling for scale > 1.
  int surface_width = CPC_RENDER_WIDTH;
  int surface_height = (scale > 1) ? CPC_VISIBLE_SCR_HEIGHT * 2 : CPC_VISIBLE_SCR_HEIGHT;
  t->half_pixels = (scale <= 1) ? 1 : 0;  // controls dwYScale in video_set_style
  vid = SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGBA32);
  if (!vid) return nullptr;

  // Create GL texture for CPC framebuffer
  glGenTextures(1, &cpc_gl_texture);
  glBindTexture(GL_TEXTURE_2D, cpc_gl_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface_width, surface_height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

  {
    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(vid->format);
    SDL_Palette* pal = SDL_GetSurfacePalette(vid);
    SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(fmt, pal, 0, 0, 0));
  }
  compute_scale(t, surface_width, surface_height);
  return vid;
}

void direct_setpal(SDL_Color* c)
{
  if (SDL_Palette* pal = SDL_GetSurfacePalette(vid)) {
    SDL_SetPaletteColors(pal, c, 0, 32);
  }
}

// Phase A: CPC framebuffer upload + ImGui render + main viewport (~3ms).
// Called first so audio can be pushed before the expensive phase B.
void direct_flip_a(video_plugin* t)
{
  // Recompute display area each frame (handles window resize, 4:3 aspect)
  compute_scale(t, vid->w, vid->h);

  // Update texture filtering: LINEAR for 4:3 stretch, NEAREST for square pixels
  GLenum filter = CPC.scr_crt_aspect ? GL_LINEAR : GL_NEAREST;
  glBindTexture(GL_TEXTURE_2D, cpc_gl_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

  // Upload CPC framebuffer to GL texture
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vid->w, vid->h,
                  GL_RGBA, GL_UNSIGNED_BYTE, vid->pixels);

  // Start ImGui frame
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  // Draw CPC framebuffer as background image via ImGui (classic mode only;
  // in docked mode the CPC Screen window handles its own ImGui::Image())
  if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList(vp)->AddImage(
        static_cast<ImTextureID>(cpc_gl_texture),
        ImVec2(vp->Pos.x + t->x_offset, vp->Pos.y + t->y_offset),
        ImVec2(vp->Pos.x + t->x_offset + t->width, vp->Pos.y + t->y_offset + t->height));
  }

  // Render all ImGui windows
  imgui_render_ui();
  ImGui::Render();

  // GL clear and render
  int display_w, display_h;
  SDL_GetWindowSizeInPixels(mainSDLWindow, &display_w, &display_h);
  glViewport(0, 0, display_w, display_h);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  // Capture screenshot (emulator screen only)
  video_capture_if_pending();
}

// Phase B: floating ImGui viewports + window swap (0-60ms depending on viewport count).
// Runs after audio push so GL stalls don't starve the audio queue.
void direct_flip_b([[maybe_unused]] video_plugin* t)
{
  // Multi-viewport: update and render platform windows.
  // Only render when there are actual platform viewports (floating devtools, popups, submenus).
  // When only the main viewport exists (Viewports.Size == 1), skip — saves GL context
  // switches and SDL_GL_SwapWindow calls that block on macOS compositor.
  ImGuiIO& io = ImGui::GetIO();
  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    SDL_Window* backup_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext backup_context = SDL_GL_GetCurrentContext();
    ImGui::UpdatePlatformWindows();
    if (ImGui::GetPlatformIO().Viewports.Size > 1) {
      koncpc_order_viewports_above_main();
      ImGui::RenderPlatformWindowsDefault();
    }
    SDL_GL_MakeCurrent(backup_window, backup_context);
  }

  SDL_GL_SwapWindow(mainSDLWindow);
}

void direct_close()
{
  if (ImGui::GetCurrentContext()) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
  if (cpc_gl_texture) { glDeleteTextures(1, &cpc_gl_texture); cpc_gl_texture = 0; }
  if (vid) { SDL_DestroySurface(vid); vid = nullptr; }
  if (glcontext) { SDL_GL_DestroyContext(glcontext); glcontext = nullptr; }
  if (mainSDLWindow) { SDL_DestroyWindow(mainSDLWindow); mainSDLWindow = nullptr; }
}

/* ------------------------------------------------------------------------------------ */
/* "Direct (GPU)" plugin — SDL3 GPU path, additive (P1.2b Phase 4) --------------------- */
/* ------------------------------------------------------------------------------------ */
// A self-contained GPU variant of the Direct plugin.  Completely isolated
// from direct_init / direct_flip_a / direct_flip_b / direct_close above:
// no shared state, no dispatcher, no renaming.  Activated only when the
// new plugin entry (added at the end of video_plugin_list) is selected
// AND g_gpu.blit_pipeline is available (Metal today; Vulkan/D3D12 once
// Phase 3b ships shader blobs).
//
// On non-GPU-capable backends gpu_direct_init returns nullptr and
// video_init falls through its existing SDL_Renderer / headless chain.
//
// Design notes — these avoid the crash/deadlock failures from the first
// Phase 4 attempt:
//   - Command buffer is submitted in Phase A (gpu_flip_a).  The render
//     loop's "skip video_display_b() on quit" optimisation therefore
//     cannot leak an unsubmitted buffer.
//   - Multi-viewport is DISABLED (ImGuiConfigFlags_ViewportsEnable off).
//     imgui_impl_sdlgpu3 registers no platform handlers, so secondary
//     windows would not render correctly.  Floating devtools stays
//     docked for now; full viewport support is a follow-up.
//   - Non-blocking swapchain acquire — never blocks the render thread.

SDL_Surface* gpu_direct_init(video_plugin* t, int scale, bool fs)
{
    mainSDLWindow = SDL_CreateWindow("konCePCja " VERSION_STRING,
        CPC_RENDER_WIDTH * scale, CPC_VISIBLE_SCR_HEIGHT * scale,
        (fs ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_RESIZABLE);
    if (!mainSDLWindow) return nullptr;

    const int surface_width  = CPC_RENDER_WIDTH;
    const int surface_height = (scale > 1) ? CPC_VISIBLE_SCR_HEIGHT * 2
                                            : CPC_VISIBLE_SCR_HEIGHT;
    t->half_pixels = (scale <= 1) ? 1 : 0;

    if (!video_gpu_init(mainSDLWindow,
                        static_cast<uint32_t>(surface_width),
                        static_cast<uint32_t>(surface_height))
        || g_gpu.blit_pipeline == nullptr) {
        video_gpu_shutdown();
        SDL_DestroyWindow(mainSDLWindow);
        mainSDLWindow = nullptr;
        return nullptr;
    }

    // ImGui — SDLGPU3 backend, viewports DISABLED (see design note above).
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename  = imgui_ini_path();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard
                    | ImGuiConfigFlags_DockingEnable;
    // Intentionally NO ImGuiConfigFlags_ViewportsEnable.
    ImGui::StyleColorsDark();
    imgui_init_ui();
    ImGui_ImplSDL3_InitForSDLGPU(mainSDLWindow);
    ImGui_ImplSDLGPU3_InitInfo init_info{};
    init_info.Device               = g_gpu.device;
    init_info.ColorTargetFormat    = g_gpu.swapchain_fmt;
    init_info.MSAASamples          = SDL_GPU_SAMPLECOUNT_1;
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    init_info.PresentMode          = SDL_GPU_PRESENTMODE_VSYNC;
    if (!ImGui_ImplSDLGPU3_Init(&init_info)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        video_gpu_shutdown();
        SDL_DestroyWindow(mainSDLWindow);
        mainSDLWindow = nullptr;
        return nullptr;
    }

    vid = SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGBA32);
    if (!vid) {
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        video_gpu_shutdown();
        SDL_DestroyWindow(mainSDLWindow);
        mainSDLWindow = nullptr;
        return nullptr;
    }
    {
        const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(vid->format);
        SDL_Palette* pal = SDL_GetSurfacePalette(vid);
        SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(fmt, pal, 0, 0, 0));
    }
    compute_scale(t, surface_width, surface_height);
    LOG_INFO("Direct (GPU) plugin active — device created, blit pipeline ready");
    return vid;
}

void gpu_flip_a(video_plugin* t)
{
    compute_scale(t, vid->w, vid->h);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_gpu.device);
    if (!cmd) return;   // device lost or OOM — drop the frame

    // 1. Upload CPC framebuffer via transfer buffer (cycle=true on both).
    const uint32_t row_bytes = g_gpu.cpc_tex_w * 4;
    void* dst = SDL_MapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload, /*cycle=*/true);
    if (dst) {
        if (static_cast<uint32_t>(vid->pitch) == row_bytes) {
            std::memcpy(dst, vid->pixels, row_bytes * g_gpu.cpc_tex_h);
        } else {
            auto* d = static_cast<uint8_t*>(dst);
            auto* s = static_cast<const uint8_t*>(vid->pixels);
            for (uint32_t y = 0; y < g_gpu.cpc_tex_h; ++y) {
                std::memcpy(d + y * row_bytes, s + y * vid->pitch, row_bytes);
            }
        }
        SDL_UnmapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload);

        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src_info{};
        src_info.transfer_buffer = g_gpu.cpc_upload;
        src_info.offset          = 0;
        src_info.pixels_per_row  = g_gpu.cpc_tex_w;
        src_info.rows_per_layer  = g_gpu.cpc_tex_h;

        SDL_GPUTextureRegion dst_region{};
        dst_region.texture = g_gpu.cpc_texture;
        dst_region.w = g_gpu.cpc_tex_w;
        dst_region.h = g_gpu.cpc_tex_h;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, /*cycle=*/true);
        SDL_EndGPUCopyPass(copy);
    }

    // 2. ImGui frame.  Unlike the GL path, we do NOT push the CPC image
    //    into the ImGui background draw list for Classic mode — the
    //    manual blit below (step 5) is the authoritative path because it
    //    picks the right sampler (linear vs nearest) per scr_crt_aspect.
    //    For Docked mode the CPC Screen ImGui window pulls the texture
    //    via video_get_cpc_texture() and renders it with ImGui::Image().
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    imgui_render_ui();
    ImGui::Render();

    // 3. CRITICAL: PrepareDrawData must precede BeginGPURenderPass.
    ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);

    // 4. NON-BLOCKING swapchain acquire.  Null return = minimised / resizing;
    //    skip the render pass but still submit the copy pass so the GPU
    //    keeps draining.
    SDL_GPUTexture* swap_tex = nullptr;
    Uint32 sw = 0, sh = 0;
    bool have_swap = SDL_AcquireGPUSwapchainTexture(cmd, mainSDLWindow,
                                                    &swap_tex, &sw, &sh)
                     && swap_tex != nullptr;

    if (have_swap) {
        SDL_GPUColorTargetInfo tgt{};
        tgt.texture     = swap_tex;
        tgt.load_op     = SDL_GPU_LOADOP_CLEAR;
        tgt.store_op    = SDL_GPU_STOREOP_STORE;
        tgt.cycle       = false;
        tgt.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);

        if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
            SDL_GPUViewport vp{};
            vp.x = static_cast<float>(t->x_offset);
            vp.y = static_cast<float>(t->y_offset);
            vp.w = static_cast<float>(t->width);
            vp.h = static_cast<float>(t->height);
            vp.max_depth = 1.0f;
            SDL_SetGPUViewport(pass, &vp);

            SDL_BindGPUGraphicsPipeline(pass, g_gpu.blit_pipeline);
            SDL_GPUTextureSamplerBinding binding{};
            binding.texture = g_gpu.cpc_texture;
            binding.sampler = CPC.scr_crt_aspect ? g_gpu.linear_sampler
                                                  : g_gpu.nearest_sampler;
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

            // Reset viewport to full swapchain so ImGui renders everywhere.
            SDL_GPUViewport full{};
            full.w = static_cast<float>(sw);
            full.h = static_cast<float>(sh);
            full.max_depth = 1.0f;
            SDL_SetGPUViewport(pass, &full);
        }

        ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);
        SDL_EndGPURenderPass(pass);
    }

    // 5. CPU-side capture (reads vid->pixels, backend-agnostic).
    video_capture_if_pending();

    // 6. SUBMIT IN PHASE A — avoids the quit-skip leak from the first attempt.
    SDL_SubmitGPUCommandBuffer(cmd);
    g_gpu.pending_cmd = nullptr;
}

void gpu_flip_b([[maybe_unused]] video_plugin* t)
{
    // Intentionally empty for the GPU plugin:
    //   - Command buffer was already submitted in gpu_flip_a.
    //   - ImGui multi-viewport is disabled, so no per-viewport work needed.
    // The render loop's "skip on quit" optimisation is therefore harmless.
}

void gpu_direct_close()
{
    // Teardown order is critical — see the plan's teardown-order table.
    // INVARIANT: no pending_cmd exists here (submitted in gpu_flip_a).

    if (g_gpu.device) SDL_WaitForGPUIdle(g_gpu.device);

    if (ImGui::GetCurrentContext()) {
        ImGui_ImplSDLGPU3_Shutdown();   // releases bd state, still needs device
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (vid) { SDL_DestroySurface(vid); vid = nullptr; }
    video_gpu_shutdown();               // destroys device, samplers, texture…
    if (mainSDLWindow) { SDL_DestroyWindow(mainSDLWindow); mainSDLWindow = nullptr; }
}

/* ------------------------------------------------------------------------------------ */
/* "CRT Basic (GPU)" plugin — SDL3 GPU path (P1.2b Phase 6b) --------------------------- */
/* ------------------------------------------------------------------------------------ */
//
// GPU variant of CRT Basic.  Single-pass: instead of blit_pipeline, the
// main render pass binds a CRT shader pipeline that samples the CPC
// texture through barrel distortion, scanline mixing, and an RGB
// phosphor mask.  Uniforms (input_size, output_size) pushed per frame.
//
// MSL source only; SPIRV/DXBC deferred.  On non-Metal backends the
// init returns nullptr and video_init falls through.

struct CrtBasicUniforms {
    float input_size[2];
    float output_size[2];
};

static SDL_GPUShader*           g_crt_basic_vertex_shader   = nullptr;
static SDL_GPUShader*           g_crt_basic_fragment_shader = nullptr;
static SDL_GPUGraphicsPipeline* g_crt_basic_pipeline        = nullptr;

static bool create_crt_basic_pipeline()
{
    if (!g_gpu.device) return false;
    const char* driver = SDL_GetGPUDeviceDriver(g_gpu.device);
    if (!driver) return false;

    SDL_GPUShaderCreateInfo vsi{};
    vsi.stage               = SDL_GPU_SHADERSTAGE_VERTEX;

    SDL_GPUShaderCreateInfo fsi{};
    fsi.stage               = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsi.num_samplers        = 1;
    fsi.num_uniform_buffers = 1;

    if (std::strcmp(driver, "metal") == 0) {
        vsi.format     = SDL_GPU_SHADERFORMAT_MSL;
        vsi.code       = reinterpret_cast<const Uint8*>(kCrtBasicMSLSource);
        vsi.code_size  = std::strlen(kCrtBasicMSLSource);
        vsi.entrypoint = "vert_main";
        fsi.format     = SDL_GPU_SHADERFORMAT_MSL;
        fsi.code       = reinterpret_cast<const Uint8*>(kCrtBasicMSLSource);
        fsi.code_size  = std::strlen(kCrtBasicMSLSource);
        fsi.entrypoint = "frag_main";
    } else if (std::strcmp(driver, "vulkan") == 0
               && kBlitVertexSPIRVSize > 0 && kCrtBasicFragmentSPIRVSize > 0) {
        // Reuses the blit vertex shader — both emit v_uv at location 0 from
        // a gl_VertexIndex-driven fullscreen triangle.
        vsi.format     = SDL_GPU_SHADERFORMAT_SPIRV;
        vsi.code       = kBlitVertexSPIRV;
        vsi.code_size  = kBlitVertexSPIRVSize;
        vsi.entrypoint = "main";
        fsi.format     = SDL_GPU_SHADERFORMAT_SPIRV;
        fsi.code       = kCrtBasicFragmentSPIRV;
        fsi.code_size  = kCrtBasicFragmentSPIRVSize;
        fsi.entrypoint = "main";
    } else if (std::strcmp(driver, "direct3d12") == 0
               && kBlitVertexDXBCSize > 0 && kCrtBasicFragmentDXBCSize > 0) {
        vsi.format     = SDL_GPU_SHADERFORMAT_DXBC;
        vsi.code       = kBlitVertexDXBC;
        vsi.code_size  = kBlitVertexDXBCSize;
        vsi.entrypoint = "main";
        fsi.format     = SDL_GPU_SHADERFORMAT_DXBC;
        fsi.code       = kCrtBasicFragmentDXBC;
        fsi.code_size  = kCrtBasicFragmentDXBCSize;
        fsi.entrypoint = "main";
    } else {
        return false;  // no shader blob available for this backend
    }

    g_crt_basic_vertex_shader   = SDL_CreateGPUShader(g_gpu.device, &vsi);
    g_crt_basic_fragment_shader = SDL_CreateGPUShader(g_gpu.device, &fsi);
    if (!g_crt_basic_vertex_shader || !g_crt_basic_fragment_shader) {
        LOG_ERROR("CRT Basic (GPU) shader create failed: " << SDL_GetError());
        if (g_crt_basic_vertex_shader)   { SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_vertex_shader);   g_crt_basic_vertex_shader   = nullptr; }
        if (g_crt_basic_fragment_shader) { SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_fragment_shader); g_crt_basic_fragment_shader = nullptr; }
        return false;
    }

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = g_gpu.swapchain_fmt;

    SDL_GPUGraphicsPipelineTargetInfo target_info{};
    target_info.num_color_targets         = 1;
    target_info.color_target_descriptions = &color_target;
    target_info.has_depth_stencil_target  = false;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader   = g_crt_basic_vertex_shader;
    info.fragment_shader = g_crt_basic_fragment_shader;
    info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.target_info     = target_info;

    g_crt_basic_pipeline = SDL_CreateGPUGraphicsPipeline(g_gpu.device, &info);
    if (!g_crt_basic_pipeline) {
        LOG_ERROR("CRT Basic (GPU) pipeline create failed: " << SDL_GetError());
        SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_vertex_shader);   g_crt_basic_vertex_shader   = nullptr;
        SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_fragment_shader); g_crt_basic_fragment_shader = nullptr;
        return false;
    }
    return true;
}

static void destroy_crt_basic_pipeline()
{
    if (!g_gpu.device) return;
    if (g_crt_basic_pipeline)        { SDL_ReleaseGPUGraphicsPipeline(g_gpu.device, g_crt_basic_pipeline); g_crt_basic_pipeline = nullptr; }
    if (g_crt_basic_fragment_shader) { SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_fragment_shader); g_crt_basic_fragment_shader = nullptr; }
    if (g_crt_basic_vertex_shader)   { SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_vertex_shader);   g_crt_basic_vertex_shader   = nullptr; }
}

static SDL_Surface* crt_basic_gpu_init(video_plugin* t, int scale, bool fs)
{
    SDL_Surface* surf = gpu_direct_init(t, scale, fs);  // reuses Phase 4 setup
    if (!surf) return nullptr;
    if (!create_crt_basic_pipeline()) {
        gpu_direct_close();
        return nullptr;
    }
    LOG_INFO("CRT Basic (GPU) plugin active");
    return surf;
}

static void crt_basic_gpu_flip_a(video_plugin* t)
{
    compute_scale(t, vid->w, vid->h);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_gpu.device);
    if (!cmd) return;

    // 1. Upload CPC framebuffer (identical to gpu_flip_a).
    const uint32_t row_bytes = g_gpu.cpc_tex_w * 4;
    void* dst = SDL_MapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload, /*cycle=*/true);
    if (dst) {
        if (static_cast<uint32_t>(vid->pitch) == row_bytes) {
            std::memcpy(dst, vid->pixels, row_bytes * g_gpu.cpc_tex_h);
        } else {
            auto* d = static_cast<uint8_t*>(dst);
            auto* s = static_cast<const uint8_t*>(vid->pixels);
            for (uint32_t y = 0; y < g_gpu.cpc_tex_h; ++y)
                std::memcpy(d + y * row_bytes, s + y * vid->pitch, row_bytes);
        }
        SDL_UnmapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload);

        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src_info{};
        src_info.transfer_buffer = g_gpu.cpc_upload;
        src_info.pixels_per_row  = g_gpu.cpc_tex_w;
        src_info.rows_per_layer  = g_gpu.cpc_tex_h;
        SDL_GPUTextureRegion dst_region{};
        dst_region.texture = g_gpu.cpc_texture;
        dst_region.w = g_gpu.cpc_tex_w;
        dst_region.h = g_gpu.cpc_tex_h;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, /*cycle=*/true);
        SDL_EndGPUCopyPass(copy);
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    imgui_render_ui();
    ImGui::Render();
    ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);

    SDL_GPUTexture* swap_tex = nullptr;
    Uint32 sw = 0, sh = 0;
    bool have_swap = SDL_AcquireGPUSwapchainTexture(cmd, mainSDLWindow, &swap_tex, &sw, &sh)
                     && swap_tex != nullptr;

    if (have_swap) {
        SDL_GPUColorTargetInfo tgt{};
        tgt.texture     = swap_tex;
        tgt.load_op     = SDL_GPU_LOADOP_CLEAR;
        tgt.store_op    = SDL_GPU_STOREOP_STORE;
        tgt.cycle       = false;
        tgt.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);

        if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
            SDL_GPUViewport vp{};
            vp.x = static_cast<float>(t->x_offset);
            vp.y = static_cast<float>(t->y_offset);
            vp.w = static_cast<float>(t->width);
            vp.h = static_cast<float>(t->height);
            vp.max_depth = 1.0f;
            SDL_SetGPUViewport(pass, &vp);

            SDL_BindGPUGraphicsPipeline(pass, g_crt_basic_pipeline);
            SDL_GPUTextureSamplerBinding binding{};
            binding.texture = g_gpu.cpc_texture;
            binding.sampler = g_gpu.linear_sampler;
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

            // Push uniforms.  input_size is CPC resolution (half the
            // surface height because scanlines are doubled); output_size
            // drives the RGB phosphor mask cell period.
            CrtBasicUniforms uni{};
            uni.input_size[0]  = static_cast<float>(vid->w);
            uni.input_size[1]  = static_cast<float>(t->half_pixels ? vid->h : vid->h / 2);
            uni.output_size[0] = static_cast<float>(t->width);
            uni.output_size[1] = static_cast<float>(t->height);
            SDL_PushGPUFragmentUniformData(cmd, 0, &uni, sizeof(uni));

            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

            SDL_GPUViewport full{};
            full.w = static_cast<float>(sw);
            full.h = static_cast<float>(sh);
            full.max_depth = 1.0f;
            SDL_SetGPUViewport(pass, &full);
        }

        ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);
        SDL_EndGPURenderPass(pass);
    }

    video_capture_if_pending();
    SDL_SubmitGPUCommandBuffer(cmd);
    g_gpu.pending_cmd = nullptr;
}

static void crt_basic_gpu_close()
{
    if (g_gpu.device) SDL_WaitForGPUIdle(g_gpu.device);
    destroy_crt_basic_pipeline();
    gpu_direct_close();
}

/* ------------------------------------------------------------------------------------ */
/* "CRT Full (GPU)" plugin — SDL3 GPU port of crt_frag_full (Phase 6c) ----------------- */
/* ------------------------------------------------------------------------------------ */
//
// Same uniform struct as CRT Basic (input_size / output_size); the
// additional curvature / scanline / mask / bloom / vignette knobs
// (which the GL path passed as 5 float uniforms but always with the
// same hard-coded values) are inlined as constants in the shader.

static SDL_GPUShader*           g_crt_full_vertex_shader   = nullptr;
static SDL_GPUShader*           g_crt_full_fragment_shader = nullptr;
static SDL_GPUGraphicsPipeline* g_crt_full_pipeline        = nullptr;

static bool create_crt_full_pipeline()
{
    if (!g_gpu.device) return false;
    const char* driver = SDL_GetGPUDeviceDriver(g_gpu.device);
    if (!driver) return false;

    SDL_GPUShaderCreateInfo vsi{};
    vsi.stage               = SDL_GPU_SHADERSTAGE_VERTEX;

    SDL_GPUShaderCreateInfo fsi{};
    fsi.stage               = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsi.num_samplers        = 1;
    fsi.num_uniform_buffers = 1;

    if (std::strcmp(driver, "metal") == 0) {
        vsi.format     = SDL_GPU_SHADERFORMAT_MSL;
        vsi.code       = reinterpret_cast<const Uint8*>(kCrtFullMSLSource);
        vsi.code_size  = std::strlen(kCrtFullMSLSource);
        vsi.entrypoint = "vert_main";
        fsi.format     = SDL_GPU_SHADERFORMAT_MSL;
        fsi.code       = reinterpret_cast<const Uint8*>(kCrtFullMSLSource);
        fsi.code_size  = std::strlen(kCrtFullMSLSource);
        fsi.entrypoint = "frag_main";
    } else if (std::strcmp(driver, "vulkan") == 0
               && kBlitVertexSPIRVSize > 0 && kCrtFullFragmentSPIRVSize > 0) {
        vsi.format     = SDL_GPU_SHADERFORMAT_SPIRV;
        vsi.code       = kBlitVertexSPIRV;
        vsi.code_size  = kBlitVertexSPIRVSize;
        vsi.entrypoint = "main";
        fsi.format     = SDL_GPU_SHADERFORMAT_SPIRV;
        fsi.code       = kCrtFullFragmentSPIRV;
        fsi.code_size  = kCrtFullFragmentSPIRVSize;
        fsi.entrypoint = "main";
    } else if (std::strcmp(driver, "direct3d12") == 0
               && kBlitVertexDXBCSize > 0 && kCrtFullFragmentDXBCSize > 0) {
        vsi.format     = SDL_GPU_SHADERFORMAT_DXBC;
        vsi.code       = kBlitVertexDXBC;
        vsi.code_size  = kBlitVertexDXBCSize;
        vsi.entrypoint = "main";
        fsi.format     = SDL_GPU_SHADERFORMAT_DXBC;
        fsi.code       = kCrtFullFragmentDXBC;
        fsi.code_size  = kCrtFullFragmentDXBCSize;
        fsi.entrypoint = "main";
    } else {
        return false;
    }

    g_crt_full_vertex_shader   = SDL_CreateGPUShader(g_gpu.device, &vsi);
    g_crt_full_fragment_shader = SDL_CreateGPUShader(g_gpu.device, &fsi);
    if (!g_crt_full_vertex_shader || !g_crt_full_fragment_shader) {
        LOG_ERROR("CRT Full (GPU) shader create failed: " << SDL_GetError());
        if (g_crt_full_vertex_shader)   { SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_vertex_shader);   g_crt_full_vertex_shader   = nullptr; }
        if (g_crt_full_fragment_shader) { SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_fragment_shader); g_crt_full_fragment_shader = nullptr; }
        return false;
    }

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = g_gpu.swapchain_fmt;

    SDL_GPUGraphicsPipelineTargetInfo target_info{};
    target_info.num_color_targets         = 1;
    target_info.color_target_descriptions = &color_target;
    target_info.has_depth_stencil_target  = false;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader   = g_crt_full_vertex_shader;
    info.fragment_shader = g_crt_full_fragment_shader;
    info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.target_info     = target_info;

    g_crt_full_pipeline = SDL_CreateGPUGraphicsPipeline(g_gpu.device, &info);
    if (!g_crt_full_pipeline) {
        LOG_ERROR("CRT Full (GPU) pipeline create failed: " << SDL_GetError());
        SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_vertex_shader);   g_crt_full_vertex_shader   = nullptr;
        SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_fragment_shader); g_crt_full_fragment_shader = nullptr;
        return false;
    }
    return true;
}

static void destroy_crt_full_pipeline()
{
    if (!g_gpu.device) return;
    if (g_crt_full_pipeline)        { SDL_ReleaseGPUGraphicsPipeline(g_gpu.device, g_crt_full_pipeline); g_crt_full_pipeline = nullptr; }
    if (g_crt_full_fragment_shader) { SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_fragment_shader); g_crt_full_fragment_shader = nullptr; }
    if (g_crt_full_vertex_shader)   { SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_vertex_shader);   g_crt_full_vertex_shader   = nullptr; }
}

static SDL_Surface* crt_full_gpu_init(video_plugin* t, int scale, bool fs)
{
    SDL_Surface* surf = gpu_direct_init(t, scale, fs);
    if (!surf) return nullptr;
    if (!create_crt_full_pipeline()) {
        gpu_direct_close();
        return nullptr;
    }
    LOG_INFO("CRT Full (GPU) plugin active");
    return surf;
}

// CRT Full flip_a: same copy-pass + render flow as crt_basic_gpu_flip_a,
// only differs in which pipeline is bound.  Factored inline to keep
// the per-plugin lifecycle explicit.
static void crt_full_gpu_flip_a(video_plugin* t)
{
    compute_scale(t, vid->w, vid->h);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_gpu.device);
    if (!cmd) return;

    const uint32_t row_bytes = g_gpu.cpc_tex_w * 4;
    void* dst = SDL_MapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload, /*cycle=*/true);
    if (dst) {
        if (static_cast<uint32_t>(vid->pitch) == row_bytes) {
            std::memcpy(dst, vid->pixels, row_bytes * g_gpu.cpc_tex_h);
        } else {
            auto* d = static_cast<uint8_t*>(dst);
            auto* s = static_cast<const uint8_t*>(vid->pixels);
            for (uint32_t y = 0; y < g_gpu.cpc_tex_h; ++y)
                std::memcpy(d + y * row_bytes, s + y * vid->pitch, row_bytes);
        }
        SDL_UnmapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload);

        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src_info{};
        src_info.transfer_buffer = g_gpu.cpc_upload;
        src_info.pixels_per_row  = g_gpu.cpc_tex_w;
        src_info.rows_per_layer  = g_gpu.cpc_tex_h;
        SDL_GPUTextureRegion dst_region{};
        dst_region.texture = g_gpu.cpc_texture;
        dst_region.w = g_gpu.cpc_tex_w;
        dst_region.h = g_gpu.cpc_tex_h;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, /*cycle=*/true);
        SDL_EndGPUCopyPass(copy);
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    imgui_render_ui();
    ImGui::Render();
    ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);

    SDL_GPUTexture* swap_tex = nullptr;
    Uint32 sw = 0, sh = 0;
    bool have_swap = SDL_AcquireGPUSwapchainTexture(cmd, mainSDLWindow, &swap_tex, &sw, &sh)
                     && swap_tex != nullptr;

    if (have_swap) {
        SDL_GPUColorTargetInfo tgt{};
        tgt.texture     = swap_tex;
        tgt.load_op     = SDL_GPU_LOADOP_CLEAR;
        tgt.store_op    = SDL_GPU_STOREOP_STORE;
        tgt.cycle       = false;
        tgt.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);

        if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
            SDL_GPUViewport vp{};
            vp.x = static_cast<float>(t->x_offset);
            vp.y = static_cast<float>(t->y_offset);
            vp.w = static_cast<float>(t->width);
            vp.h = static_cast<float>(t->height);
            vp.max_depth = 1.0f;
            SDL_SetGPUViewport(pass, &vp);

            SDL_BindGPUGraphicsPipeline(pass, g_crt_full_pipeline);
            SDL_GPUTextureSamplerBinding binding{};
            binding.texture = g_gpu.cpc_texture;
            binding.sampler = g_gpu.linear_sampler;
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

            CrtBasicUniforms uni{};                 // same layout as CRT Basic
            uni.input_size[0]  = static_cast<float>(vid->w);
            uni.input_size[1]  = static_cast<float>(t->half_pixels ? vid->h : vid->h / 2);
            uni.output_size[0] = static_cast<float>(t->width);
            uni.output_size[1] = static_cast<float>(t->height);
            SDL_PushGPUFragmentUniformData(cmd, 0, &uni, sizeof(uni));

            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

            SDL_GPUViewport full{};
            full.w = static_cast<float>(sw);
            full.h = static_cast<float>(sh);
            full.max_depth = 1.0f;
            SDL_SetGPUViewport(pass, &full);
        }

        ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);
        SDL_EndGPURenderPass(pass);
    }

    video_capture_if_pending();
    SDL_SubmitGPUCommandBuffer(cmd);
    g_gpu.pending_cmd = nullptr;
}

static void crt_full_gpu_close()
{
    if (g_gpu.device) SDL_WaitForGPUIdle(g_gpu.device);
    destroy_crt_full_pipeline();
    gpu_direct_close();
}

/* ------------------------------------------------------------------------------------ */
/* "CRT Lottes (GPU)" plugin — SDL3 GPU port of crt_frag_lottes (Phase 6d) ------------- */
/* ------------------------------------------------------------------------------------ */
//
// Timothy Lottes' CRT shader (public domain) — Gaussian beam profile,
// sRGB-linear blending, curvature warp, slot mask.  Samples 11 input
// pixels per output pixel (3-5-3 horizontal kernel × 3 scanlines), so
// this is the heaviest of the three CRT tiers.  CPC texture is sampled
// with NEAREST filtering (matches the GL path — the Gaussian kernel
// does its own pixel-centre snapping via fetch()).
// Uniforms identical to Basic/Full: { float2 input_size; float2 output_size; }.

static SDL_GPUShader*           g_crt_lottes_vertex_shader   = nullptr;
static SDL_GPUShader*           g_crt_lottes_fragment_shader = nullptr;
static SDL_GPUGraphicsPipeline* g_crt_lottes_pipeline        = nullptr;

static bool create_crt_lottes_pipeline()
{
    if (!g_gpu.device) return false;
    const char* driver = SDL_GetGPUDeviceDriver(g_gpu.device);
    if (!driver) return false;

    SDL_GPUShaderCreateInfo vsi{};
    vsi.stage               = SDL_GPU_SHADERSTAGE_VERTEX;

    SDL_GPUShaderCreateInfo fsi{};
    fsi.stage               = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsi.num_samplers        = 1;
    fsi.num_uniform_buffers = 1;

    if (std::strcmp(driver, "metal") == 0) {
        vsi.format     = SDL_GPU_SHADERFORMAT_MSL;
        vsi.code       = reinterpret_cast<const Uint8*>(kCrtLottesMSLSource);
        vsi.code_size  = std::strlen(kCrtLottesMSLSource);
        vsi.entrypoint = "vert_main";
        fsi.format     = SDL_GPU_SHADERFORMAT_MSL;
        fsi.code       = reinterpret_cast<const Uint8*>(kCrtLottesMSLSource);
        fsi.code_size  = std::strlen(kCrtLottesMSLSource);
        fsi.entrypoint = "frag_main";
    } else if (std::strcmp(driver, "vulkan") == 0
               && kBlitVertexSPIRVSize > 0 && kCrtLottesFragmentSPIRVSize > 0) {
        vsi.format     = SDL_GPU_SHADERFORMAT_SPIRV;
        vsi.code       = kBlitVertexSPIRV;
        vsi.code_size  = kBlitVertexSPIRVSize;
        vsi.entrypoint = "main";
        fsi.format     = SDL_GPU_SHADERFORMAT_SPIRV;
        fsi.code       = kCrtLottesFragmentSPIRV;
        fsi.code_size  = kCrtLottesFragmentSPIRVSize;
        fsi.entrypoint = "main";
    } else if (std::strcmp(driver, "direct3d12") == 0
               && kBlitVertexDXBCSize > 0 && kCrtLottesFragmentDXBCSize > 0) {
        vsi.format     = SDL_GPU_SHADERFORMAT_DXBC;
        vsi.code       = kBlitVertexDXBC;
        vsi.code_size  = kBlitVertexDXBCSize;
        vsi.entrypoint = "main";
        fsi.format     = SDL_GPU_SHADERFORMAT_DXBC;
        fsi.code       = kCrtLottesFragmentDXBC;
        fsi.code_size  = kCrtLottesFragmentDXBCSize;
        fsi.entrypoint = "main";
    } else {
        return false;
    }

    g_crt_lottes_vertex_shader   = SDL_CreateGPUShader(g_gpu.device, &vsi);
    g_crt_lottes_fragment_shader = SDL_CreateGPUShader(g_gpu.device, &fsi);
    if (!g_crt_lottes_vertex_shader || !g_crt_lottes_fragment_shader) {
        LOG_ERROR("CRT Lottes (GPU) shader create failed: " << SDL_GetError());
        if (g_crt_lottes_vertex_shader)   { SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_vertex_shader);   g_crt_lottes_vertex_shader   = nullptr; }
        if (g_crt_lottes_fragment_shader) { SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_fragment_shader); g_crt_lottes_fragment_shader = nullptr; }
        return false;
    }

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = g_gpu.swapchain_fmt;

    SDL_GPUGraphicsPipelineTargetInfo target_info{};
    target_info.num_color_targets         = 1;
    target_info.color_target_descriptions = &color_target;
    target_info.has_depth_stencil_target  = false;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader   = g_crt_lottes_vertex_shader;
    info.fragment_shader = g_crt_lottes_fragment_shader;
    info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.target_info     = target_info;

    g_crt_lottes_pipeline = SDL_CreateGPUGraphicsPipeline(g_gpu.device, &info);
    if (!g_crt_lottes_pipeline) {
        LOG_ERROR("CRT Lottes (GPU) pipeline create failed: " << SDL_GetError());
        SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_vertex_shader);   g_crt_lottes_vertex_shader   = nullptr;
        SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_fragment_shader); g_crt_lottes_fragment_shader = nullptr;
        return false;
    }
    return true;
}

static void destroy_crt_lottes_pipeline()
{
    if (!g_gpu.device) return;
    if (g_crt_lottes_pipeline)        { SDL_ReleaseGPUGraphicsPipeline(g_gpu.device, g_crt_lottes_pipeline); g_crt_lottes_pipeline = nullptr; }
    if (g_crt_lottes_fragment_shader) { SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_fragment_shader); g_crt_lottes_fragment_shader = nullptr; }
    if (g_crt_lottes_vertex_shader)   { SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_vertex_shader);   g_crt_lottes_vertex_shader   = nullptr; }
}

static SDL_Surface* crt_lottes_gpu_init(video_plugin* t, int scale, bool fs)
{
    SDL_Surface* surf = gpu_direct_init(t, scale, fs);
    if (!surf) return nullptr;
    if (!create_crt_lottes_pipeline()) {
        gpu_direct_close();
        return nullptr;
    }
    LOG_INFO("CRT Lottes (GPU) plugin active");
    return surf;
}

// CRT Lottes flip_a: same copy-pass + render flow as Basic/Full, differs
// in the bound pipeline and uses NEAREST sampling on cpc_texture because
// the Gaussian kernel re-quantises via floor()/fract() inside the shader.
static void crt_lottes_gpu_flip_a(video_plugin* t)
{
    compute_scale(t, vid->w, vid->h);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_gpu.device);
    if (!cmd) return;

    const uint32_t row_bytes = g_gpu.cpc_tex_w * 4;
    void* dst = SDL_MapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload, /*cycle=*/true);
    if (dst) {
        if (static_cast<uint32_t>(vid->pitch) == row_bytes) {
            std::memcpy(dst, vid->pixels, row_bytes * g_gpu.cpc_tex_h);
        } else {
            auto* d = static_cast<uint8_t*>(dst);
            auto* s = static_cast<const uint8_t*>(vid->pixels);
            for (uint32_t y = 0; y < g_gpu.cpc_tex_h; ++y)
                std::memcpy(d + y * row_bytes, s + y * vid->pitch, row_bytes);
        }
        SDL_UnmapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload);

        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src_info{};
        src_info.transfer_buffer = g_gpu.cpc_upload;
        src_info.pixels_per_row  = g_gpu.cpc_tex_w;
        src_info.rows_per_layer  = g_gpu.cpc_tex_h;
        SDL_GPUTextureRegion dst_region{};
        dst_region.texture = g_gpu.cpc_texture;
        dst_region.w = g_gpu.cpc_tex_w;
        dst_region.h = g_gpu.cpc_tex_h;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, /*cycle=*/true);
        SDL_EndGPUCopyPass(copy);
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    imgui_render_ui();
    ImGui::Render();
    ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);

    SDL_GPUTexture* swap_tex = nullptr;
    Uint32 sw = 0, sh = 0;
    bool have_swap = SDL_AcquireGPUSwapchainTexture(cmd, mainSDLWindow, &swap_tex, &sw, &sh)
                     && swap_tex != nullptr;

    if (have_swap) {
        SDL_GPUColorTargetInfo tgt{};
        tgt.texture     = swap_tex;
        tgt.load_op     = SDL_GPU_LOADOP_CLEAR;
        tgt.store_op    = SDL_GPU_STOREOP_STORE;
        tgt.cycle       = false;
        tgt.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);

        if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
            SDL_GPUViewport vp{};
            vp.x = static_cast<float>(t->x_offset);
            vp.y = static_cast<float>(t->y_offset);
            vp.w = static_cast<float>(t->width);
            vp.h = static_cast<float>(t->height);
            vp.max_depth = 1.0f;
            SDL_SetGPUViewport(pass, &vp);

            SDL_BindGPUGraphicsPipeline(pass, g_crt_lottes_pipeline);
            SDL_GPUTextureSamplerBinding binding{};
            binding.texture = g_gpu.cpc_texture;
            binding.sampler = g_gpu.nearest_sampler;   // Lottes uses NEAREST
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

            CrtBasicUniforms uni{};                 // same layout as Basic/Full
            uni.input_size[0]  = static_cast<float>(vid->w);
            uni.input_size[1]  = static_cast<float>(t->half_pixels ? vid->h : vid->h / 2);
            uni.output_size[0] = static_cast<float>(t->width);
            uni.output_size[1] = static_cast<float>(t->height);
            SDL_PushGPUFragmentUniformData(cmd, 0, &uni, sizeof(uni));

            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

            SDL_GPUViewport full{};
            full.w = static_cast<float>(sw);
            full.h = static_cast<float>(sh);
            full.max_depth = 1.0f;
            SDL_SetGPUViewport(pass, &full);
        }

        ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);
        SDL_EndGPURenderPass(pass);
    }

    video_capture_if_pending();
    SDL_SubmitGPUCommandBuffer(cmd);
    g_gpu.pending_cmd = nullptr;
}

static void crt_lottes_gpu_close()
{
    if (g_gpu.device) SDL_WaitForGPUIdle(g_gpu.device);
    destroy_crt_lottes_pipeline();
    gpu_direct_close();
}

/* ------------------------------------------------------------------------------------ */
/* GL 3.2 function loader + offscreen texture cache ---------------------------------- */
/* ------------------------------------------------------------------------------------ */
// Portable GL 2.0+/3.0+ function loader used by video_offscreen_texture()
// for devtools panel rendering.  Originally introduced for the legacy CRT
// shader plugins (removed in Phase 7b); kept here because devtools-style
// ImGui-into-FBO rendering still needs these symbols on non-Apple GL
// contexts where they're not direct symbols.
//
// On macOS, <OpenGL/gl3.h> provides these as direct symbols.
// On Windows/Linux, loaded via SDL_GL_GetProcAddress at runtime.
static struct CrtGL {
    GLuint (*CreateShader)(GLenum) = nullptr;
    void   (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void   (*CompileShader)(GLuint) = nullptr;
    void   (*GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void   (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void   (*DeleteShader)(GLuint) = nullptr;
    GLuint (*CreateProgram)() = nullptr;
    void   (*AttachShader)(GLuint, GLuint) = nullptr;
    void   (*LinkProgram)(GLuint) = nullptr;
    void   (*GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void   (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void   (*UseProgram)(GLuint) = nullptr;
    void   (*DeleteProgram)(GLuint) = nullptr;
    GLint  (*GetUniformLocation)(GLuint, const GLchar*) = nullptr;
    void   (*Uniform1i)(GLint, GLint) = nullptr;
    void   (*Uniform1f)(GLint, GLfloat) = nullptr;
    void   (*Uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
    GLint  (*GetAttribLocation)(GLuint, const GLchar*) = nullptr;
    void   (*EnableVertexAttribArray)(GLuint) = nullptr;
    void   (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
    void   (*GenVertexArrays)(GLsizei, GLuint*) = nullptr;
    void   (*BindVertexArray)(GLuint) = nullptr;
    void   (*DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
    void   (*GenBuffers)(GLsizei, GLuint*) = nullptr;
    void   (*BindBuffer)(GLenum, GLuint) = nullptr;
    void   (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void   (*DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void   (*GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void   (*BindFramebuffer)(GLenum, GLuint) = nullptr;
    void   (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    void   (*DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    GLenum (*CheckFramebufferStatus)(GLenum) = nullptr;
    void   (*ActiveTexture)(GLenum) = nullptr;

    bool load() {
        #define CRT_LOAD(name) \
            name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress("gl" #name)); \
            if (!name) { LOG_ERROR("CRT: failed to load gl" #name); return false; }
        CRT_LOAD(CreateShader) CRT_LOAD(ShaderSource) CRT_LOAD(CompileShader)
        CRT_LOAD(GetShaderiv) CRT_LOAD(GetShaderInfoLog) CRT_LOAD(DeleteShader)
        CRT_LOAD(CreateProgram) CRT_LOAD(AttachShader) CRT_LOAD(LinkProgram)
        CRT_LOAD(GetProgramiv) CRT_LOAD(GetProgramInfoLog) CRT_LOAD(UseProgram)
        CRT_LOAD(DeleteProgram) CRT_LOAD(GetUniformLocation)
        CRT_LOAD(Uniform1i) CRT_LOAD(Uniform1f) CRT_LOAD(Uniform2f)
        CRT_LOAD(GetAttribLocation) CRT_LOAD(EnableVertexAttribArray)
        CRT_LOAD(VertexAttribPointer)
        CRT_LOAD(GenVertexArrays) CRT_LOAD(BindVertexArray) CRT_LOAD(DeleteVertexArrays)
        CRT_LOAD(GenBuffers) CRT_LOAD(BindBuffer) CRT_LOAD(BufferData) CRT_LOAD(DeleteBuffers)
        CRT_LOAD(GenFramebuffers) CRT_LOAD(BindFramebuffer) CRT_LOAD(FramebufferTexture2D)
        CRT_LOAD(DeleteFramebuffers) CRT_LOAD(CheckFramebufferStatus) CRT_LOAD(ActiveTexture)
        #undef CRT_LOAD
        return true;
    }
} gl3;

// ── video_offscreen_texture ────────────────────────────────────────────────
// Defined here so it can access the file-local `gl3` function table.

uintptr_t video_offscreen_texture(
    const char* key, int canvas_w, int canvas_h, size_t dirty_marker,
    const std::function<void(ImDrawList*, int, int)>& draw_fn)
{
    if (using_sdl_renderer) return 0;
    if (canvas_w <= 0 || canvas_h <= 0) return 0;

    // Lazily load GL3 function pointers (reuses the CRT-shader table).
    if (!gl3.GenFramebuffers && !gl3.load()) return 0;

    auto& e = g_offscreen_cache[key];

    const bool size_changed = (canvas_w != e.w || canvas_h != e.h);
    if (!size_changed && e.dirty == dirty_marker && e.tex)
        return static_cast<uintptr_t>(e.tex);

    // (Re-)create FBO + texture when dimensions change.
    if (size_changed || !e.fbo) {
        if (e.fbo) { gl3.DeleteFramebuffers(1, &e.fbo); e.fbo = 0; }
        if (e.tex) { glDeleteTextures(1, &e.tex);       e.tex = 0; }

        glGenTextures(1, &e.tex);
        glBindTexture(GL_TEXTURE_2D, e.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, canvas_w, canvas_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        gl3.GenFramebuffers(1, &e.fbo);
        gl3.BindFramebuffer(GL_FRAMEBUFFER, e.fbo);
        gl3.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, e.tex, 0);
        GLenum status = gl3.CheckFramebufferStatus(GL_FRAMEBUFFER);
        gl3.BindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("video_offscreen_texture: FBO '" << key
                      << "' incomplete (0x" << std::hex << status << ")");
            gl3.DeleteFramebuffers(1, &e.fbo); e.fbo = 0;
            glDeleteTextures(1, &e.tex);       e.tex = 0;
            return 0;
        }
        e.w = canvas_w;
        e.h = canvas_h;
        e.dirty = ~size_t(0); // force redraw at new size
    }

    // Build ImDrawList with caller-supplied content.
    ImDrawList draw_list(ImGui::GetDrawListSharedData());
    draw_list.PushClipRect(ImVec2(0, 0),
                           ImVec2(static_cast<float>(canvas_w),
                                  static_cast<float>(canvas_h)));
    draw_list.PushTextureID(ImGui::GetIO().Fonts->TexID);
    draw_fn(&draw_list, canvas_w, canvas_h);

    // Wrap in ImDrawData for the OpenGL3 backend.
    ImDrawData draw_data;
    draw_data.Valid            = true;
    draw_data.CmdLists.push_back(&draw_list);
    draw_data.CmdListsCount    = 1;
    draw_data.TotalVtxCount    = draw_list.VtxBuffer.Size;
    draw_data.TotalIdxCount    = draw_list.IdxBuffer.Size;
    draw_data.DisplayPos       = ImVec2(0, 0);
    draw_data.DisplaySize      = ImVec2(static_cast<float>(canvas_w),
                                        static_cast<float>(canvas_h));
    draw_data.FramebufferScale = ImVec2(1, 1);

    // Render into the FBO (save/restore framebuffer binding and viewport).
    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    gl3.BindFramebuffer(GL_FRAMEBUFFER, e.fbo);
    glViewport(0, 0, canvas_w, canvas_h);
    ImGui_ImplOpenGL3_RenderDrawData(&draw_data);
    gl3.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1],
               prev_viewport[2], prev_viewport[3]);

    e.dirty = dirty_marker;
    return static_cast<uintptr_t>(e.tex);
}


// Previously: lightweight Direct ↔ CRT switch without re-creating the
// GL context / ImGui backend.  After Phase 7b the legacy GL CRT plugins
// are gone, so there's no pair of plugins this optimisation applies to
// anymore — every scr_style change on the legacy GL path goes through a
// full re-init.  Kept as a stub so kon_cpc_ja.cpp still links; returns
// false to force the full-reinit path.
bool video_try_lightweight_switch() {
    return false;
}

/* ------------------------------------------------------------------------------------ */
/* SDL_Renderer video plugin (D3D11 on Windows, no OpenGL required) ------------------- */
/* ------------------------------------------------------------------------------------ */
void sdlr_close();
void sdlr_swscale_close();

SDL_Surface* sdlr_init(video_plugin* t, int scale, bool fs)
{
  mainSDLWindow = SDL_CreateWindow("konCePCja " VERSION_STRING,
      CPC_RENDER_WIDTH * scale, CPC_VISIBLE_SCR_HEIGHT * scale,
      (fs ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_RESIZABLE);
  if (!mainSDLWindow) return nullptr;

  renderer = SDL_CreateRenderer(mainSDLWindow, nullptr);
  if (!renderer) {
    SDL_DestroyWindow(mainSDLWindow); mainSDLWindow = nullptr;
    return nullptr;
  }

  // Initialize Dear ImGui
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = imgui_ini_path();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // ViewportsEnable not supported by SDL_Renderer backend
  ImGui::StyleColorsDark();
  imgui_init_ui();
  if (!ImGui_ImplSDL3_InitForSDLRenderer(mainSDLWindow, renderer)) {
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer); renderer = nullptr;
    SDL_DestroyWindow(mainSDLWindow); mainSDLWindow = nullptr;
    return nullptr;
  }
  if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer); renderer = nullptr;
    SDL_DestroyWindow(mainSDLWindow); mainSDLWindow = nullptr;
    return nullptr;
  }

  int surface_width = CPC_RENDER_WIDTH;
  int surface_height = (scale > 1) ? CPC_VISIBLE_SCR_HEIGHT * 2 : CPC_VISIBLE_SCR_HEIGHT;
  t->half_pixels = (scale <= 1) ? 1 : 0;
  vid = SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGBA32);
  if (!vid) { sdlr_close(); return nullptr; }

  cpc_sdl_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_STREAMING, surface_width, surface_height);
  if (!cpc_sdl_texture) { sdlr_close(); return nullptr; }
  SDL_SetTextureScaleMode(cpc_sdl_texture, SDL_SCALEMODE_NEAREST);

  {
    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(vid->format);
    SDL_Palette* pal = SDL_GetSurfacePalette(vid);
    SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(fmt, pal, 0, 0, 0));
  }
  using_sdl_renderer = true;
  compute_scale(t, surface_width, surface_height);
  return vid;
}

void sdlr_flip(video_plugin* t)
{
  // Recompute display area each frame (handles window resize, 4:3 aspect)
  compute_scale(t, vid->w, vid->h);

  // Update texture filtering: LINEAR for 4:3 stretch, NEAREST for square pixels
  SDL_SetTextureScaleMode(cpc_sdl_texture,
      CPC.scr_crt_aspect ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);

  // Upload CPC framebuffer to SDL texture
  SDL_UpdateTexture(cpc_sdl_texture, nullptr, vid->pixels, vid->pitch);

  // Start ImGui frame
  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  // Draw CPC framebuffer as background image via ImGui (classic mode only)
  if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList(vp)->AddImage(
        reinterpret_cast<ImTextureID>(cpc_sdl_texture),
        ImVec2(vp->Pos.x + t->x_offset, vp->Pos.y + t->y_offset),
        ImVec2(vp->Pos.x + t->x_offset + t->width, vp->Pos.y + t->y_offset + t->height));
  }

  // Render all ImGui windows
  imgui_render_ui();
  ImGui::Render();

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

  // Capture screenshot (emulator screen only)
  video_capture_if_pending();

  SDL_RenderPresent(renderer);
}

void sdlr_close()
{
  if (ImGui::GetCurrentContext()) {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
  if (cpc_sdl_texture) { SDL_DestroyTexture(cpc_sdl_texture); cpc_sdl_texture = nullptr; }
  if (vid) { SDL_DestroySurface(vid); vid = nullptr; }
  if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
  if (mainSDLWindow) { SDL_DestroyWindow(mainSDLWindow); mainSDLWindow = nullptr; }
  using_sdl_renderer = false;
}

/* SDL_Renderer swscale plugin -------------------------------------------------------- */
SDL_Surface* sdlr_swscale_init(video_plugin* t, int scale, bool fs)
{
  mainSDLWindow = SDL_CreateWindow("konCePCja " VERSION_STRING,
      CPC_RENDER_WIDTH * scale, CPC_VISIBLE_SCR_HEIGHT * scale,
      (fs ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_RESIZABLE);
  if (!mainSDLWindow) return nullptr;

  renderer = SDL_CreateRenderer(mainSDLWindow, nullptr);
  if (!renderer) {
    SDL_DestroyWindow(mainSDLWindow); mainSDLWindow = nullptr;
    return nullptr;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = imgui_ini_path();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  ImGui::StyleColorsDark();
  imgui_init_ui();
  if (!ImGui_ImplSDL3_InitForSDLRenderer(mainSDLWindow, renderer)) {
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer); renderer = nullptr;
    SDL_DestroyWindow(mainSDLWindow); mainSDLWindow = nullptr;
    return nullptr;
  }
  if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer); renderer = nullptr;
    SDL_DestroyWindow(mainSDLWindow); mainSDLWindow = nullptr;
    return nullptr;
  }

  // Software scaling plugins: render at native width, filter produces 2× output.
  int surface_width = CPC_RENDER_WIDTH;
  int surface_height = (scale > 1) ? CPC_VISIBLE_SCR_HEIGHT * 2 : CPC_VISIBLE_SCR_HEIGHT;
  t->half_pixels = (scale <= 1) ? 1 : 0;
  vid = SDL_CreateSurface(surface_width*2, surface_height*2, SDL_PIXELFORMAT_RGBA32);
  if (!vid) { sdlr_close(); return nullptr; }

  cpc_sdl_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_STREAMING, surface_width * 2, surface_height * 2);
  if (!cpc_sdl_texture) { sdlr_close(); return nullptr; }
  SDL_SetTextureScaleMode(cpc_sdl_texture, SDL_SCALEMODE_NEAREST);

  scaled = SDL_CreateSurface(surface_width*2, surface_height*2, SDL_PIXELFORMAT_RGB565);
  if (!scaled) { sdlr_swscale_close(); return nullptr; }
  {
    const SDL_PixelFormatDetails* s_fmt = SDL_GetPixelFormatDetails(scaled->format);
    if (!s_fmt || s_fmt->bits_per_pixel != 16)
    {
      LOG_ERROR(t->name << ": SDL didn't return a 16 bpp surface but a " << static_cast<int>(s_fmt ? s_fmt->bits_per_pixel : 0) << " bpp one.");
      sdlr_swscale_close(); return nullptr;
    }
  }
  {
    const SDL_PixelFormatDetails* v_fmt = SDL_GetPixelFormatDetails(vid->format);
    SDL_Palette* v_pal = SDL_GetSurfacePalette(vid);
    if (v_fmt)
      SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(v_fmt, v_pal, 0, 0, 0));
  }
  compute_scale(t, surface_width, surface_height);
  pub = SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGB565);
  if (!pub) { sdlr_swscale_close(); return nullptr; }
  {
    const SDL_PixelFormatDetails* p_fmt = SDL_GetPixelFormatDetails(pub->format);
    if (!p_fmt || p_fmt->bits_per_pixel != 16)
    {
      LOG_ERROR(t->name << ": SDL didn't return a 16 bpp surface but a " << static_cast<int>(p_fmt ? p_fmt->bits_per_pixel : 0) << " bpp one.");
      sdlr_swscale_close(); return nullptr;
    }
  }
  using_sdl_renderer = true;
  return pub;
}

void sdlr_swscale_blit(video_plugin* t)
{
  SDL_BlitSurface(scaled, nullptr, vid, nullptr);

  SDL_UpdateTexture(cpc_sdl_texture, nullptr, vid->pixels, vid->pitch);

  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList(vp)->AddImage(
        reinterpret_cast<ImTextureID>(cpc_sdl_texture),
        ImVec2(vp->Pos.x + t->x_offset, vp->Pos.y + t->y_offset),
        ImVec2(vp->Pos.x + t->x_offset + t->width, vp->Pos.y + t->y_offset + t->height));
  }

  imgui_render_ui();
  ImGui::Render();

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

  video_capture_if_pending();

  SDL_RenderPresent(renderer);
}

void sdlr_swscale_close()
{
  sdlr_close();
  if (scaled) { SDL_DestroySurface(scaled); scaled = nullptr; }
  if (pub) { SDL_DestroySurface(pub); pub = nullptr; }
}

/* ------------------------------------------------------------------------------------ */
/* Headless video plugin (no window, offscreen surface only) -------------------------- */
/* ------------------------------------------------------------------------------------ */
SDL_Surface* headless_init(video_plugin* t, int /*scale*/, bool /*fs*/)
{
  t->half_pixels = 1;  // dwYScale=1 for headless
  int surface_width = CPC_RENDER_WIDTH;
  int surface_height = CPC_VISIBLE_SCR_HEIGHT;
  vid = SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGBA32);
  if (!vid) return nullptr;
  {
    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(vid->format);
    SDL_Palette* pal = SDL_GetSurfacePalette(vid);
    SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(fmt, pal, 0, 0, 0));
  }
  return vid;
}

void headless_setpal(SDL_Color* /*c*/)
{
  // palette stored in CPC colours array; no GPU upload needed
}

void headless_flip(video_plugin* /*t*/)
{
  // no-op: nothing to present in headless mode
}

void headless_close()
{
  if (vid) { SDL_DestroySurface(vid); vid = nullptr; }
}



/* ------------------------------------------------------------------------------------ */
/* Common 2x software scaling code ---------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */

/* Computes the clipping of pub and scaled surfaces and put the result in src and dst accordingly.
 *
 * This provides the rectangles to clip to obtain a centered doubled CPC display
 * in the middle of the dst surface if it fits
 *
 * dst is the screen
 * src is the internal window
 *
 * Only exposed for testing purposes. Shouldn't be used outside of video.cpp
 */
static void compute_rects(SDL_Rect* src, SDL_Rect* dst, Uint8 half_pixels)
{
  // Software scaling filter output is 2× the render surface
  int surface_width = CPC_RENDER_WIDTH * 2;
  int surface_height = half_pixels ? CPC_VISIBLE_SCR_HEIGHT * 2 : CPC_VISIBLE_SCR_HEIGHT * 4;
  /* initialise the source rect to full source */
  src->x=0;
  src->y=0;
  src->w=pub->w;
  src->h=pub->h;
  
  dst->x=(scaled->w-surface_width)/2,
  dst->y=(scaled->h-surface_height)/2;
  dst->w=scaled->w;
  dst->h=scaled->h;
  
  int dw=src->w*2-dst->w;
  /* the src width is too big */
  if (dw>0)
  {
    // To ensure src is not bigger than dst for odd widths.
    dw += 1;
    src->w-=dw/2;
    src->x+=dw/4;

    dst->x=0;
    dst->w=scaled->w;
  }
  else
  {
    dst->w=surface_width;
  }
  int dh=src->h*2-dst->h;
  /* the src height is too big */
  if (dh>0)
  {
    // To ensure src is not bigger than dst for odd heights.
    dh += 1;
    src->h-=dh/2;
    src->y+=dh/4;
    
    dst->y=0;
    dst->h=scaled->h;
  }
  else
  {
    // Without this -=, the bottom of the screen has line with random pixels.
    // With this, they are black instead which is slightly better.
    // Investigating where this comes from and how to avoid it would be nice!
    src->h-=2*2;
    dst->h=surface_height;
  }
}

void compute_rects_for_tests(SDL_Rect* src, SDL_Rect* dst, Uint8 half_pixels)
{
  compute_rects(src, dst, half_pixels);
}

// Scale factor table shared with compute_scale and Options combo.
static const float video_scale_factors[] = { 0.f, 1.f, 1.5f, 2.f, 3.f };
static const int video_scale_factors_count = sizeof(video_scale_factors) / sizeof(video_scale_factors[0]);

// Compute window dimensions for the current scale + bars + 4:3 aspect.
// For Fit mode (scr_scale=0), returns false (don't resize — keep user's window).
static bool compute_window_size(int& out_w, int& out_h) {
  float f;
  if (CPC.scr_scale > 0 && static_cast<int>(CPC.scr_scale) < video_scale_factors_count)
    f = video_scale_factors[CPC.scr_scale];
  else
    return false;  // Fit mode — don't resize
  out_w = static_cast<int>(CPC_RENDER_WIDTH * f) + devtools_panel_width;
  int cpc_h = CPC.scr_crt_aspect
            ? static_cast<int>(CPC_RENDER_WIDTH * f * 3.f / 4.f)
            : static_cast<int>(CPC_VISIBLE_SCR_HEIGHT * f);
  out_h = max(cpc_h + topbar_height + bottombar_height, devtools_panel_height);
  return true;
}

void video_set_devtools_panel(SDL_Surface* surface, int width, int height, int scale)
{
  if (!mainSDLWindow || !surface) return;
  devtools_panel_surface = surface;
  devtools_panel_width = width * scale;
  devtools_panel_height = height * scale;
  devtools_panel_surface_width = surface->w;
  devtools_panel_surface_height = surface->h;
  int w, h;
  if (compute_window_size(w, h)) {
    devtools_cpc_height = h - topbar_height - bottombar_height;
    SDL_SetWindowSize(mainSDLWindow, w, h);
  }
  if (vid_plugin && vid) compute_scale(vid_plugin, vid->w, vid->h);
}

void video_clear_devtools_panel()
{
  devtools_panel_surface = nullptr;
  devtools_panel_width = 0;
  devtools_panel_height = 0;
  devtools_panel_surface_width = 0;
  devtools_panel_surface_height = 0;
  devtools_cpc_height = 0;
  if (mainSDLWindow) {
    int w, h;
    if (compute_window_size(w, h))
      SDL_SetWindowSize(mainSDLWindow, w, h);
  }
  if (vid_plugin && vid) compute_scale(vid_plugin, vid->w, vid->h);
}

void video_set_topbar(SDL_Surface* surface, int height)
{
  if (!mainSDLWindow) return;
  topbar_surface = surface;
  topbar_height = height;
  int w, h;
  if (compute_window_size(w, h))
    SDL_SetWindowSize(mainSDLWindow, w, h);
  if (vid_plugin && vid) compute_scale(vid_plugin, vid->w, vid->h);
}

void video_clear_topbar()
{
  topbar_surface = nullptr;
  topbar_height = 0;
  if (mainSDLWindow) {
    int w, h;
    if (compute_window_size(w, h))
      SDL_SetWindowSize(mainSDLWindow, w, h);
  }
  if (vid_plugin && vid) compute_scale(vid_plugin, vid->w, vid->h);
}

int video_get_devtools_panel_width()
{
  return devtools_panel_width;
}

int video_get_devtools_panel_height()
{
  return devtools_panel_height;
}

int video_get_devtools_panel_surface_width()
{
  return devtools_panel_surface_width;
}

int video_get_devtools_panel_surface_height()
{
  return devtools_panel_surface_height;
}

int video_get_topbar_height()
{
  return topbar_height;
}

void video_set_bottombar(int height)
{
  if (!mainSDLWindow) return;
  bottombar_height = height;
  int w, h;
  if (compute_window_size(w, h))
    SDL_SetWindowSize(mainSDLWindow, w, h);
  if (vid_plugin && vid) compute_scale(vid_plugin, vid->w, vid->h);
}

int video_get_bottombar_height()
{
  return bottombar_height;
}

SDL_Surface* swscale_init(video_plugin* t, int scale, bool fs)
{
  // Create OpenGL window
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

  mainSDLWindow = SDL_CreateWindow("konCePCja " VERSION_STRING,
      CPC_RENDER_WIDTH * scale, CPC_VISIBLE_SCR_HEIGHT * scale,
      (fs ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!mainSDLWindow) return nullptr;

  glcontext = SDL_GL_CreateContext(mainSDLWindow);
  if (!glcontext) return nullptr;
  SDL_GL_MakeCurrent(mainSDLWindow, glcontext);
  SDL_GL_SetSwapInterval(0);

  // Initialize Dear ImGui
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = imgui_ini_path();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  ImGui::StyleColorsDark();
  imgui_init_ui();
  ImGui_ImplSDL3_InitForOpenGL(mainSDLWindow, glcontext);
  if (!ImGui_ImplOpenGL3_Init("#version 150")) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(glcontext);
    glcontext = nullptr;
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
    return nullptr;
  }

  int surface_width = CPC_RENDER_WIDTH;
  int surface_height = (scale > 1) ? CPC_VISIBLE_SCR_HEIGHT * 2 : CPC_VISIBLE_SCR_HEIGHT;
  t->half_pixels = (scale <= 1) ? 1 : 0;
  vid = SDL_CreateSurface(surface_width*2, surface_height*2, SDL_PIXELFORMAT_RGBA32);
  if (!vid) return nullptr;

  // Create GL texture for CPC framebuffer (swscale uses 2x surfaces)
  glGenTextures(1, &cpc_gl_texture);
  glBindTexture(GL_TEXTURE_2D, cpc_gl_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface_width * 2, surface_height * 2, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

  scaled = SDL_CreateSurface(surface_width*2, surface_height*2, SDL_PIXELFORMAT_RGB565);
  if (!scaled) return nullptr;
  {
    const SDL_PixelFormatDetails* s_fmt = SDL_GetPixelFormatDetails(scaled->format);
    if (!s_fmt || s_fmt->bits_per_pixel != 16)
    {
      LOG_ERROR(t->name << ": SDL didn't return a 16 bpp surface but a " << static_cast<int>(s_fmt ? s_fmt->bits_per_pixel : 0) << " bpp one.");
      return nullptr;
    }
  }
  {
    const SDL_PixelFormatDetails* v_fmt = SDL_GetPixelFormatDetails(vid->format);
    SDL_Palette* v_pal = SDL_GetSurfacePalette(vid);
    SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(v_fmt, v_pal, 0, 0, 0));
  }
  compute_scale(t, surface_width, surface_height);
  pub = SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGB565);
  {
    const SDL_PixelFormatDetails* p_fmt = SDL_GetPixelFormatDetails(pub->format);
    if (!p_fmt || p_fmt->bits_per_pixel != 16)
    {
      LOG_ERROR(t->name << ": SDL didn't return a 16 bpp surface but a " << static_cast<int>(p_fmt ? p_fmt->bits_per_pixel : 0) << " bpp one.");
      return nullptr;
    }
  }

  return pub;
}

// Phase A: common software-scaler blit + ImGui render + main viewport.
// SDL_Renderer path handles everything itself (including the swap) and returns early.
void swscale_blit_a(video_plugin* t)
{
  // Dispatch to SDL_Renderer path if active — it handles the full render+swap itself.
  if (using_sdl_renderer) {
    sdlr_swscale_blit(t);
    return;
  }

  // Blit to convert from 16bpp to RGBA32 for GL upload
  SDL_BlitSurface(scaled, nullptr, vid, nullptr);

  // Upload CPC framebuffer to GL texture
  glBindTexture(GL_TEXTURE_2D, cpc_gl_texture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vid->w, vid->h,
                  GL_RGBA, GL_UNSIGNED_BYTE, vid->pixels);

  // Start ImGui frame
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  // Draw CPC framebuffer as background image via ImGui (classic mode only)
  if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList(vp)->AddImage(
        static_cast<ImTextureID>(cpc_gl_texture),
        ImVec2(vp->Pos.x + t->x_offset, vp->Pos.y + t->y_offset),
        ImVec2(vp->Pos.x + t->x_offset + t->width, vp->Pos.y + t->y_offset + t->height));
  }

  // Render all ImGui windows
  imgui_render_ui();
  ImGui::Render();

  // GL clear and render
  int display_w, display_h;
  SDL_GetWindowSizeInPixels(mainSDLWindow, &display_w, &display_h);
  glViewport(0, 0, display_w, display_h);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  // Capture screenshot (emulator screen only)
  video_capture_if_pending();
}

// Phase B: floating ImGui viewports + window swap.
// SDL_Renderer path already completed everything in swscale_blit_a — return early.
void swscale_blit_b([[maybe_unused]] video_plugin* t)
{
  if (using_sdl_renderer) return;

  // Multi-viewport: render platform windows only when they exist
  ImGuiIO& io = ImGui::GetIO();
  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    SDL_Window* backup_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext backup_context = SDL_GL_GetCurrentContext();
    ImGui::UpdatePlatformWindows();
    if (ImGui::GetPlatformIO().Viewports.Size > 1) {
      koncpc_order_viewports_above_main();
      ImGui::RenderPlatformWindowsDefault();
    }
    SDL_GL_MakeCurrent(backup_window, backup_context);
  }

  SDL_GL_SwapWindow(mainSDLWindow);
}

void swscale_setpal(SDL_Color* c)
{
  if (SDL_Palette* pal = SDL_GetSurfacePalette(scaled)) {
    SDL_SetPaletteColors(pal, c, 0, 32);
  }
  if (SDL_Palette* pal = SDL_GetSurfacePalette(pub)) {
    SDL_SetPaletteColors(pal, c, 0, 32);
  }
}

void swscale_close()
{
  if (using_sdl_renderer)
    sdlr_close();
  else
    direct_close();
  if (scaled) { SDL_DestroySurface(scaled); scaled = nullptr; }
  if (pub) { SDL_DestroySurface(pub); pub = nullptr; }
}

/* ------------------------------------------------------------------------------------ */
/* Super eagle video plugin ----------------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */

/* 2X SAI Filter */
static Uint32 colorMask = 0xF7DEF7DE;
static Uint32 lowPixelMask = 0x08210821;
static Uint32 qcolorMask = 0xE79CE79C;
static Uint32 qlowpixelMask = 0x18631863;
static Uint32 redblueMask = 0xF81F;
static Uint32 greenMask = 0x7E0;

__inline__ int GetResult1 (Uint32 A, Uint32 B, Uint32 C, Uint32 D)
{
  int x = 0;
  int y = 0;
  int r = 0;

  if (A == C)
    x += 1;
  else if (B == C)
    y += 1;
  if (A == D)
    x += 1;
  else if (B == D)
    y += 1;
  if (x <= 1)
    r += 1;
  if (y <= 1)
    r -= 1;
  return r;
}

__inline__ int GetResult2 (Uint32 A, Uint32 B, Uint32 C, Uint32 D)
{
  int x = 0;
  int y = 0;
  int r = 0;

  if (A == C)
    x += 1;
  else if (B == C)
    y += 1;
  if (A == D)
    x += 1;
  else if (B == D)
    y += 1;
  if (x <= 1)
    r -= 1;
  if (y <= 1)
    r += 1;
  return r;
}

__inline__ int GetResult (Uint32 A, Uint32 B, Uint32 C, Uint32 D)
{
  int x = 0;
  int y = 0;
  int r = 0;

  if (A == C)
    x += 1;
  else if (B == C)
    y += 1;
  if (A == D)
    x += 1;
  else if (B == D)
    y += 1;
  if (x <= 1)
    r += 1;
  if (y <= 1)
    r -= 1;
  return r;
}


__inline__ Uint32 INTERPOLATE (Uint32 A, Uint32 B)
{
  if (A != B)
  {
    return (((A & colorMask) >> 1) + ((B & colorMask) >> 1) +
        (A & B & lowPixelMask));
  }
  return A;
}

__inline__ Uint32 Q_INTERPOLATE (Uint32 A, Uint32 B, Uint32 C, Uint32 D)
{
  Uint32 x = ((A & qcolorMask) >> 2) +
    ((B & qcolorMask) >> 2) +
    ((C & qcolorMask) >> 2) + ((D & qcolorMask) >> 2);
  Uint32 y = (A & qlowpixelMask) +
    (B & qlowpixelMask) + (C & qlowpixelMask) + (D & qlowpixelMask);
  y = (y >> 2) & qlowpixelMask;
  return x + y;
}

void filter_supereagle(Uint8 *srcPtr, Uint32 srcPitch, /* Uint8 *deltaPtr,  */
     Uint8 *dstPtr, Uint32 dstPitch, int width, int height)
{
  Uint8  *dP;
  Uint16 *bP;
  Uint32 inc_bP;



  Uint32 finish;
  Uint32 Nextline = srcPitch >> 1;

  inc_bP = 1;

  for (; height ; height--)
  {
    bP = reinterpret_cast<Uint16 *>(srcPtr);
    dP = dstPtr;
    for (finish = width; finish; finish -= inc_bP)
    {
      Uint32 color4, color5, color6;
      Uint32 color1, color2, color3;
      Uint32 colorA1, colorA2, colorB1, colorB2, colorS1, colorS2;
      Uint32 product1a, product1b, product2a, product2b;
      colorB1 = *(bP - Nextline);
      colorB2 = *(bP - Nextline + 1);

      color4 = *(bP - 1);
      color5 = *(bP);
      color6 = *(bP + 1);
      colorS2 = *(bP + 2);

      color1 = *(bP + Nextline - 1);
      color2 = *(bP + Nextline);
      color3 = *(bP + Nextline + 1);
      colorS1 = *(bP + Nextline + 2);

      colorA1 = *(bP + Nextline + Nextline);
      colorA2 = *(bP + Nextline + Nextline + 1);
      // --------------------------------------
      if (color2 == color6 && color5 != color3)
      {
        product1b = product2a = color2;
        if ((color1 == color2) || (color6 == colorB2))
        {
          product1a = INTERPOLATE (color2, color5);
          product1a = INTERPOLATE (color2, product1a);
          //                       product1a = color2;
        }
        else
        {
          product1a = INTERPOLATE (color5, color6);
        }

        if ((color6 == colorS2) || (color2 == colorA1))
        {
          product2b = INTERPOLATE (color2, color3);
          product2b = INTERPOLATE (color2, product2b);
          //                       product2b = color2;
        }
        else
        {
          product2b = INTERPOLATE (color2, color3);
        }
      }
      else if (color5 == color3 && color2 != color6)
      {
        product2b = product1a = color5;

        if ((colorB1 == color5) || (color3 == colorS1))
        {
          product1b = INTERPOLATE (color5, color6);
          product1b = INTERPOLATE (color5, product1b);
          //                       product1b = color5;
        }
        else
        {
          product1b = INTERPOLATE (color5, color6);
        }

        if ((color3 == colorA2) || (color4 == color5))
        {
          product2a = INTERPOLATE (color5, color2);
          product2a = INTERPOLATE (color5, product2a);
          //                       product2a = color5;
        }
        else
        {
          product2a = INTERPOLATE (color2, color3);
        }

      }
      else if (color5 == color3 && color2 == color6)
      {
        int r = 0;

        r += GetResult (color6, color5, color1, colorA1);
        r += GetResult (color6, color5, color4, colorB1);
        r += GetResult (color6, color5, colorA2, colorS1);
        r += GetResult (color6, color5, colorB2, colorS2);

        if (r > 0)
        {
          product1b = product2a = color2;
          product1a = product2b = INTERPOLATE (color5, color6);
        }
        else if (r < 0)
        {
          product2b = product1a = color5;
          product1b = product2a = INTERPOLATE (color5, color6);
        }
        else
        {
          product2b = product1a = color5;
          product1b = product2a = color2;
        }
      }
      else
      {
        product2b = product1a = INTERPOLATE (color2, color6);
        product2b =
          Q_INTERPOLATE (color3, color3, color3, product2b);
        product1a =
          Q_INTERPOLATE (color5, color5, color5, product1a);

        product2a = product1b = INTERPOLATE (color5, color3);
        product2a =
          Q_INTERPOLATE (color2, color2, color2, product2a);
        product1b =
          Q_INTERPOLATE (color6, color6, color6, product1b);

        //                    product1a = color5;
        //                    product1b = color6;
        //                    product2a = color2;
        //                    product2b = color3;
      }
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
      product1a = product1a | (product1b << 16);
      product2a = product2a | (product2b << 16);
#else
      product1a = (product1a << 16) | product1b;
      product2a = (product2a << 16) | product2b;
#endif

      *(reinterpret_cast<Uint32 *>(dP)) = product1a;
      *(reinterpret_cast<Uint32 *>(dP + dstPitch)) = product2a;

      bP += inc_bP;
      dP += sizeof (Uint32);
    }      // end of for ( finish= width etc..)
    srcPtr += srcPitch;
    dstPtr += dstPitch * 2;
  }      // endof: for (height; height; height--)
}

void seagle_flip(video_plugin* t)
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst,t->half_pixels);
  filter_supereagle(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
     static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}

/* ------------------------------------------------------------------------------------ */
/* Scale2x video plugin --------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */
void filter_scale2x(Uint8 *srcPtr, Uint32 srcPitch, 
                      Uint8 *dstPtr, Uint32 dstPitch,
          int width, int height)
{
  unsigned int nextlineSrc = srcPitch / sizeof(short);
  short *p = reinterpret_cast<short *>(srcPtr);

  unsigned int nextlineDst = dstPitch / sizeof(short);
  short *q = reinterpret_cast<short *>(dstPtr);

  while(height--) {
    int i = 0, j = 0;
    for(i = 0; i < width; ++i, j += 2) {
      short B = *(p + i - nextlineSrc);
      short D = *(p + i - 1);
      short E = *(p + i);
      short F = *(p + i + 1);
      short H = *(p + i + nextlineSrc);

      *(q + j) = D == B && B != F && D != H ? D : E;
      *(q + j + 1) = B == F && B != D && F != H ? F : E;
      *(q + j + nextlineDst) = D == H && D != B && H != F ? D : E;
      *(q + j + nextlineDst + 1) = H == F && D != H && B != F ? F : E;
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

void scale2x_flip([[maybe_unused]] video_plugin* t)
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst,t->half_pixels);
  filter_scale2x(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
     static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}

/* ------------------------------------------------------------------------------------ */
/* ascale2x video plugin --------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */
void filter_ascale2x (Uint8 *srcPtr, Uint32 srcPitch,
       Uint8 *dstPtr, Uint32 dstPitch, int width, int height)
{
  Uint8  *dP;
  Uint16 *bP;
  Uint32 inc_bP;

  Uint32 finish;
  Uint32 Nextline = srcPitch >> 1;
  inc_bP = 1;

  for (; height; height--)
  {
    bP = reinterpret_cast<Uint16 *>(srcPtr);
    dP = dstPtr;

    for (finish = width; finish; finish -= inc_bP)
    {

      Uint32 colorA, colorB;
      Uint32 colorC, colorD,
             colorE, colorF, colorG, colorH,
             colorI, colorJ, colorK, colorL,

             colorM, colorN, colorO;
      Uint32 product, product1, product2;

      //---------------------------------------
      // Map of the pixels:                    I|E F|J
      //                                       G|A B|K
      //                                       H|C D|L
      //                                       M|N O|P
      colorI = *(bP - Nextline - 1);
      colorE = *(bP - Nextline);
      colorF = *(bP - Nextline + 1);
      colorJ = *(bP - Nextline + 2);

      colorG = *(bP - 1);
      colorA = *(bP);
      colorB = *(bP + 1);
      colorK = *(bP + 2);

      colorH = *(bP + Nextline - 1);
      colorC = *(bP + Nextline);
      colorD = *(bP + Nextline + 1);
      colorL = *(bP + Nextline + 2);

      colorM = *(bP + Nextline + Nextline - 1);
      colorN = *(bP + Nextline + Nextline);
      colorO = *(bP + Nextline + Nextline + 1);

      if ((colorA == colorD) && (colorB != colorC))
      {
        if (((colorA == colorE) && (colorB == colorL)) ||
            ((colorA == colorC) && (colorA == colorF)
             && (colorB != colorE) && (colorB == colorJ)))
        {
          product = colorA;
        }
        else
        {
          product = INTERPOLATE (colorA, colorB);
        }

        if (((colorA == colorG) && (colorC == colorO)) ||
            ((colorA == colorB) && (colorA == colorH)
             && (colorG != colorC) && (colorC == colorM)))
        {
          product1 = colorA;
        }
        else
        {
          product1 = INTERPOLATE (colorA, colorC);
        }
        product2 = colorA;
      }
      else if ((colorB == colorC) && (colorA != colorD))
      {
        if (((colorB == colorF) && (colorA == colorH)) ||
            ((colorB == colorE) && (colorB == colorD)
             && (colorA != colorF) && (colorA == colorI)))
        {
          product = colorB;
        }
        else
        {
          product = INTERPOLATE (colorA, colorB);
        }

        if (((colorC == colorH) && (colorA == colorF)) ||
            ((colorC == colorG) && (colorC == colorD)
             && (colorA != colorH) && (colorA == colorI)))
        {
          product1 = colorC;
        }
        else
        {
          product1 = INTERPOLATE (colorA, colorC);
        }
        product2 = colorB;
      }
      else if ((colorA == colorD) && (colorB == colorC))
      {
        if (colorA == colorB)
        {
          product = colorA;
          product1 = colorA;
          product2 = colorA;
        }
        else
        {
          int r = 0;

          product1 = INTERPOLATE (colorA, colorC);
          product = INTERPOLATE (colorA, colorB);

          r += GetResult1 (colorA, colorB, colorG, colorE);
          r += GetResult2 (colorB, colorA, colorK, colorF);
          r += GetResult2 (colorB, colorA, colorH, colorN);
          r += GetResult1 (colorA, colorB, colorL, colorO);

          if (r > 0)
            product2 = colorA;
          else if (r < 0)
            product2 = colorB;
          else
          {
            product2 =
              Q_INTERPOLATE (colorA, colorB, colorC,
                  colorD);
          }
        }
      }
      else
      {
        product2 = Q_INTERPOLATE (colorA, colorB, colorC, colorD);

        if ((colorA == colorC) && (colorA == colorF)
            && (colorB != colorE) && (colorB == colorJ))
        {
          product = colorA;
        }
        else
          if ((colorB == colorE) && (colorB == colorD)
              && (colorA != colorF) && (colorA == colorI))
          {
            product = colorB;
          }
          else
          {
            product = INTERPOLATE (colorA, colorB);
          }

        if ((colorA == colorB) && (colorA == colorH)
            && (colorG != colorC) && (colorC == colorM))
        {
          product1 = colorA;
        }
        else
          if ((colorC == colorG) && (colorC == colorD)
              && (colorA != colorH) && (colorA == colorI))
          {
            product1 = colorC;
          }
          else
          {
            product1 = INTERPOLATE (colorA, colorC);
          }
      }
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
      product = colorA | (product << 16);
      product1 = product1 | (product2 << 16);
#else
      product = (colorA << 16) | product;
      product1 = (product1 << 16) | product2;
#endif
      *(reinterpret_cast<Uint32 *>(dP)) = product;
      *(reinterpret_cast<Uint32 *>(dP + dstPitch)) = product1;

      bP += inc_bP;
      dP += sizeof (Uint32);
    }      // end of for ( finish= width etc..)

    srcPtr += srcPitch;
    dstPtr += dstPitch * 2;
  }      // endof: for (height; height; height--)
}



void ascale2x_flip([[maybe_unused]] video_plugin* t)
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst,t->half_pixels);
  filter_ascale2x(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}


/* ------------------------------------------------------------------------------------ */
/* tv2x video plugin ------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------ */
void filter_tv2x(Uint8 *srcPtr, Uint32 srcPitch, 
    Uint8 *dstPtr, Uint32 dstPitch, 
    int width, int height)
{
  unsigned int nextlineSrc = srcPitch / sizeof(Uint16);
  Uint16 *p = reinterpret_cast<Uint16 *>(srcPtr);

  unsigned int nextlineDst = dstPitch / sizeof(Uint16);
  Uint16 *q = reinterpret_cast<Uint16 *>(dstPtr);

  while(height--) {
    int i = 0, j = 0;
    for(; i < width; ++i, j += 2) {
      Uint16 p1 = *(p + i);
      Uint32 pi;

      pi = (((p1 & redblueMask) * 7) >> 3) & redblueMask;
      pi |= (((p1 & greenMask) * 7) >> 3) & greenMask;

      *(q + j) = p1;
      *(q + j + 1) = p1;
      *(q + j + nextlineDst) = pi;
      *(q + j + nextlineDst + 1) = pi;
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

void tv2x_flip([[maybe_unused]] video_plugin* t)
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst,t->half_pixels);
  filter_tv2x(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}

/* ------------------------------------------------------------------------------------ */
/* Software bilinear video plugin ----------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */
void filter_bilinear(Uint8 *srcPtr, Uint32 srcPitch, 
    Uint8 *dstPtr, Uint32 dstPitch, 
    int width, int height)
{
  unsigned int nextlineSrc = srcPitch / sizeof(Uint16);
  Uint16 *p = reinterpret_cast<Uint16 *>(srcPtr);
  unsigned int nextlineDst = dstPitch / sizeof(Uint16);
  Uint16 *q = reinterpret_cast<Uint16 *>(dstPtr);

  while(height--) {
    int i, ii;
    for(i = 0, ii = 0; i < width; ++i, ii += 2) {
      Uint16 A = *(p + i);
      Uint16 B = *(p + i + 1);
      Uint16 C = *(p + i + nextlineSrc);
      Uint16 D = *(p + i + nextlineSrc + 1);
      *(q + ii) = A;
      *(q + ii + 1) = INTERPOLATE(A, B);
      *(q + ii + nextlineDst) = INTERPOLATE(A, C);
      *(q + ii + nextlineDst + 1) = Q_INTERPOLATE(A, B, C, D);
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

void swbilin_flip([[maybe_unused]] video_plugin* t)
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst,t->half_pixels);
  filter_bilinear(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}

/* ------------------------------------------------------------------------------------ */
/* Software bicubic video plugin ------------------------------------------------------ */
/* ------------------------------------------------------------------------------------ */
#define BLUE_MASK565 0x001F001F
#define RED_MASK565 0xF800F800
#define GREEN_MASK565 0x07E007E0

#define BLUE_MASK555 0x001F001F
#define RED_MASK555 0x7C007C00
#define GREEN_MASK555 0x03E003E0

__inline__ static void MULT(Uint16 c, float* r, float* g, float* b, float alpha) {
  *r += alpha * ((c & RED_MASK565  ) >> 11);
  *g += alpha * ((c & GREEN_MASK565) >>  5);
  *b += alpha * ((c & BLUE_MASK565 ) >>  0);
}

__inline__ static Uint16 MAKE_RGB565(float r, float g, float b) {
  return 
    (((static_cast<Uint8>(r)) << 11) & RED_MASK565  ) |
    (((static_cast<Uint8>(g)) <<  5) & GREEN_MASK565) |
    (((static_cast<Uint8>(b)) <<  0) & BLUE_MASK565 );
}

__inline__ float CUBIC_WEIGHT(float x) {
  // P(x) = { x, x>0 | 0, x<=0 }
  // P(x + 2) ^ 3 - 4 * P(x + 1) ^ 3 + 6 * P(x) ^ 3 - 4 * P(x - 1) ^ 3
  double r = 0.;
  if(x + 2 > 0) r +=      pow(x + 2, 3);
  if(x + 1 > 0) r += -4 * pow(x + 1, 3);
  if(x     > 0) r +=  6 * pow(x    , 3);
  if(x - 1 > 0) r += -4 * pow(x - 1, 3);
  return static_cast<float>(r) / 6;
}

void filter_bicubic(Uint8 *srcPtr, Uint32 srcPitch, 
    Uint8 *dstPtr, Uint32 dstPitch, 
    int width, int height)
{
  unsigned int nextlineSrc = srcPitch / sizeof(Uint16);
  Uint16 *p = reinterpret_cast<Uint16 *>(srcPtr);
  unsigned int nextlineDst = dstPitch / sizeof(Uint16);
  Uint16 *q = reinterpret_cast<Uint16 *>(dstPtr);
  int dx = width << 1, dy = height << 1;
  float fsx = static_cast<float>(width) / dx;
  float fsy = static_cast<float>(height) / dy;
  float v = 0.0f;
  int j = 0;
  for(; j < dy; ++j) {
    float u = 0.0f;
    int iv = static_cast<int>(v);
    float decy = v - iv;
    int i = 0;
    for(; i < dx; ++i) {
      int iu = static_cast<int>(u);
      float decx = u - iu;
      float r, g, b;
      int m;
      r = g = b = 0.;
      for(m = -1; m <= 2; ++m) {
        float r1 = CUBIC_WEIGHT(decy - m);
        int n;
        for(n = -1; n <= 2; ++n) {
          float r2 = CUBIC_WEIGHT(n - decx);
          Uint16* pIn = p + (iu  + n) + (iv + m) * static_cast<int>(nextlineSrc);
          MULT(*pIn, &r, &g, &b, r1 * r2);
        }
      }
      *(q + i) = MAKE_RGB565(r, g, b);
      u += fsx;
    }
    q += nextlineDst;
    v += fsy;
  }
}

void swbicub_flip([[maybe_unused]] video_plugin* t)
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst,t->half_pixels);
  filter_bicubic(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}

/* ------------------------------------------------------------------------------------ */
/* Dot matrix video plugin ------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------ */
static Uint16 DOT_16(Uint16 c, int j, int i) {
  static constexpr Uint16 dotmatrix[16] = {
    0x01E0, 0x0007, 0x3800, 0x0000,
    0x39E7, 0x0000, 0x39E7, 0x0000,
    0x3800, 0x0000, 0x01E0, 0x0007,
    0x39E7, 0x0000, 0x39E7, 0x0000
  };
  return c - ((c >> 2) & *(dotmatrix + ((j & 3) << 2) + (i & 3)));
}

void filter_dotmatrix(Uint8 *srcPtr, Uint32 srcPitch, 
    Uint8 *dstPtr, Uint32 dstPitch,
    int width, int height)
{
  unsigned int nextlineSrc = srcPitch / sizeof(Uint16);
  Uint16 *p = reinterpret_cast<Uint16 *>(srcPtr);

  unsigned int nextlineDst = dstPitch / sizeof(Uint16);
  Uint16 *q = reinterpret_cast<Uint16 *>(dstPtr);

  int i, ii, j, jj;
  for(j = 0, jj = 0; j < height; ++j, jj += 2) {
    for(i = 0, ii = 0; i < width; ++i, ii += 2) {
      Uint16 c = *(p + i);
      *(q + ii) = DOT_16(c, jj, ii);
      *(q + ii + 1) = DOT_16(c, jj, ii + 1);
      *(q + ii + nextlineDst) = DOT_16(c, jj + 1, ii);
      *(q + ii + nextlineDst + 1) = DOT_16(c, jj + 1, ii + 1);
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

void dotmat_flip([[maybe_unused]] video_plugin* t)
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst,t->half_pixels);
  filter_dotmatrix(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}

/* ------------------------------------------------------------------------------------ */
/* GPU variants of the swscale family (P1.2b Phase 5) --------------------------------- */
/* ------------------------------------------------------------------------------------ */
//
// Same CPU-side pixel filters as their GL counterparts (scale2x_flip etc.),
// but the final blit + ImGui render happens through the SDL_GPU path built
// in Phase 4 rather than OpenGL.  This requires the GPU blit pipeline, so
// activation falls back to SDL_Renderer on backends without shader blobs
// (same semantics as Direct (GPU)).
//
// The CPU filter functions (filter_scale2x, filter_seagle, etc.) are reused
// verbatim — they only touch the `scaled` / `pub` SDL surfaces and don't
// care which renderer displays the result.

static SDL_Surface* swscale_gpu_init(video_plugin* t, int scale, bool fs)
{
    mainSDLWindow = SDL_CreateWindow("konCePCja " VERSION_STRING,
        CPC_RENDER_WIDTH * scale, CPC_VISIBLE_SCR_HEIGHT * scale,
        (fs ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_RESIZABLE);
    if (!mainSDLWindow) return nullptr;

    const int surface_width  = CPC_RENDER_WIDTH;
    const int surface_height = (scale > 1) ? CPC_VISIBLE_SCR_HEIGHT * 2
                                            : CPC_VISIBLE_SCR_HEIGHT;
    t->half_pixels = (scale <= 1) ? 1 : 0;

    // swscale surfaces are 2x the base CPC size — the CPU filter upscales.
    const uint32_t gpu_tex_w = static_cast<uint32_t>(surface_width  * 2);
    const uint32_t gpu_tex_h = static_cast<uint32_t>(surface_height * 2);

    if (!video_gpu_init(mainSDLWindow, gpu_tex_w, gpu_tex_h)
        || g_gpu.blit_pipeline == nullptr) {
        video_gpu_shutdown();
        SDL_DestroyWindow(mainSDLWindow);
        mainSDLWindow = nullptr;
        return nullptr;
    }

    // ImGui SDLGPU3 backend — viewports disabled (see Phase 4 rationale).
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename  = imgui_ini_path();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard
                    | ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    imgui_init_ui();
    ImGui_ImplSDL3_InitForSDLGPU(mainSDLWindow);
    ImGui_ImplSDLGPU3_InitInfo init_info{};
    init_info.Device               = g_gpu.device;
    init_info.ColorTargetFormat    = g_gpu.swapchain_fmt;
    init_info.MSAASamples          = SDL_GPU_SAMPLECOUNT_1;
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    init_info.PresentMode          = SDL_GPU_PRESENTMODE_VSYNC;
    if (!ImGui_ImplSDLGPU3_Init(&init_info)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        video_gpu_shutdown();
        SDL_DestroyWindow(mainSDLWindow);
        mainSDLWindow = nullptr;
        return nullptr;
    }

    // Same three surfaces as swscale_init:
    //   vid    — RGBA32, 2x — the CPU-side upload buffer the GPU reads from
    //   scaled — RGB565, 2x — what the CPU filter writes into
    //   pub    — RGB565, 1x — the surface returned to the CPC (half size)
    vid    = SDL_CreateSurface(gpu_tex_w, gpu_tex_h, SDL_PIXELFORMAT_RGBA32);
    scaled = SDL_CreateSurface(gpu_tex_w, gpu_tex_h, SDL_PIXELFORMAT_RGB565);
    pub    = SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGB565);

    if (!vid || !scaled || !pub) {
        if (pub)    { SDL_DestroySurface(pub);    pub    = nullptr; }
        if (scaled) { SDL_DestroySurface(scaled); scaled = nullptr; }
        if (vid)    { SDL_DestroySurface(vid);    vid    = nullptr; }
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        video_gpu_shutdown();
        SDL_DestroyWindow(mainSDLWindow);
        mainSDLWindow = nullptr;
        return nullptr;
    }

    // Sanity-check 16 bpp format (matches swscale_init).  Full rollback
    // on failure: every resource created above must be released before
    // returning nullptr, matching the error paths earlier in this init.
    {
        const SDL_PixelFormatDetails* s_fmt = SDL_GetPixelFormatDetails(scaled->format);
        const SDL_PixelFormatDetails* p_fmt = SDL_GetPixelFormatDetails(pub->format);
        if (!s_fmt || s_fmt->bits_per_pixel != 16 ||
            !p_fmt || p_fmt->bits_per_pixel != 16) {
            LOG_ERROR(t->name << ": SDL didn't return 16 bpp surfaces");
            SDL_DestroySurface(pub);    pub    = nullptr;
            SDL_DestroySurface(scaled); scaled = nullptr;
            SDL_DestroySurface(vid);    vid    = nullptr;
            ImGui_ImplSDLGPU3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            video_gpu_shutdown();
            SDL_DestroyWindow(mainSDLWindow);
            mainSDLWindow = nullptr;
            return nullptr;
        }
    }
    {
        const SDL_PixelFormatDetails* v_fmt = SDL_GetPixelFormatDetails(vid->format);
        SDL_Palette* v_pal = SDL_GetSurfacePalette(vid);
        SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(v_fmt, v_pal, 0, 0, 0));
    }
    compute_scale(t, surface_width, surface_height);
    LOG_INFO(t->name << ": GPU swscale plugin active");
    return pub;  // swscale plugins hand the CPC the half-sized `pub` surface
}

// GPU variant of swscale_blit_a: convert scaled(16bpp) → vid(RGBA32) via
// SDL_BlitSurface, then reuse gpu_flip_a (Phase 4) to upload + render +
// submit.  gpu_flip_a uses g_gpu.cpc_tex_{w,h} which match the 2x surface
// dims we gave video_gpu_init above.
static void swscale_gpu_blit_a(video_plugin* t)
{
    SDL_BlitSurface(scaled, nullptr, vid, nullptr);
    gpu_flip_a(t);
}

static void swscale_gpu_close()
{
    if (g_gpu.device) SDL_WaitForGPUIdle(g_gpu.device);

    if (ImGui::GetCurrentContext()) {
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (scaled) { SDL_DestroySurface(scaled); scaled = nullptr; }
    if (pub)    { SDL_DestroySurface(pub);    pub    = nullptr; }
    if (vid)    { SDL_DestroySurface(vid);    vid    = nullptr; }
    video_gpu_shutdown();
    if (mainSDLWindow) { SDL_DestroyWindow(mainSDLWindow); mainSDLWindow = nullptr; }
}

// Per-filter Phase A functions — each runs its CPU filter into `scaled`,
// then hands off to swscale_gpu_blit_a.  The filter setup is copied
// verbatim from the matching GL flip to keep the additive pattern
// (the existing GL flip functions are untouched).

static void seagle_gpu_flip(video_plugin* t)
{
    if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
    SDL_Rect src, dst; compute_rects(&src, &dst, t->half_pixels);
    filter_supereagle(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
                      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
    if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
    swscale_gpu_blit_a(t);
}

static void scale2x_gpu_flip(video_plugin* t)
{
    if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
    SDL_Rect src, dst; compute_rects(&src, &dst, t->half_pixels);
    filter_scale2x(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
                   static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
    if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
    swscale_gpu_blit_a(t);
}

static void ascale2x_gpu_flip(video_plugin* t)
{
    if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
    SDL_Rect src, dst; compute_rects(&src, &dst, t->half_pixels);
    filter_ascale2x(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
                    static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
    if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
    swscale_gpu_blit_a(t);
}

static void tv2x_gpu_flip(video_plugin* t)
{
    if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
    SDL_Rect src, dst; compute_rects(&src, &dst, t->half_pixels);
    filter_tv2x(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
                static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
    if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
    swscale_gpu_blit_a(t);
}

static void swbilin_gpu_flip(video_plugin* t)
{
    if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
    SDL_Rect src, dst; compute_rects(&src, &dst, t->half_pixels);
    filter_bilinear(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
                    static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
    if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
    swscale_gpu_blit_a(t);
}

static void swbicub_gpu_flip(video_plugin* t)
{
    if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
    SDL_Rect src, dst; compute_rects(&src, &dst, t->half_pixels);
    filter_bicubic(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
                   static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
    if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
    swscale_gpu_blit_a(t);
}

static void dotmat_gpu_flip(video_plugin* t)
{
    if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
    SDL_Rect src, dst; compute_rects(&src, &dst, t->half_pixels);
    filter_dotmatrix(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
                     static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
    if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
    swscale_gpu_blit_a(t);
}

/* ------------------------------------------------------------------------------------ */
/* End of video plugins --------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */

video_plugin video_headless_plugin()
{
  return {"Headless", true, headless_init, headless_setpal, headless_flip, headless_close, 1, 0, 0, 0, 0, 0, 0, nullptr};
}

std::vector<video_plugin> video_plugin_list =
{
  // Phase 7c.1b: GL plugins deleted.  The "Direct" / swscale family entries
  // below now point at the SDL3 GPU implementations (formerly named "Direct
  // (GPU)" / "Super eagle (GPU)" / etc.).  Names kept short so existing UI
  // labels and external references (scripts, configs) continue to work.
  // Hardware flip variants are the same as software ones since switch to SDL2.
  /* Name                     Hidden Init func          Palette func     Flip func (phase A)    Close func           Half size  X, Y offsets   X, Y scale  width, height  Flip B (phase B: viewports+swap) */
  {"Direct",                  false, gpu_direct_init,    direct_setpal,   gpu_flip_a,            gpu_direct_close,    1,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  {"Direct double",           true,  gpu_direct_init,    direct_setpal,   gpu_flip_a,            gpu_direct_close,    0,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  {"Half size",               true,  gpu_direct_init,    direct_setpal,   gpu_flip_a,            gpu_direct_close,    1,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  {"Double size",             true,  gpu_direct_init,    direct_setpal,   gpu_flip_a,            gpu_direct_close,    0,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  {"Super eagle",             false, swscale_gpu_init,   swscale_setpal,  seagle_gpu_flip,       swscale_gpu_close,   1,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  {"Scale2x",                 false, swscale_gpu_init,   swscale_setpal,  scale2x_gpu_flip,      swscale_gpu_close,   1,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  {"Advanced Scale2x",        false, swscale_gpu_init,   swscale_setpal,  ascale2x_gpu_flip,     swscale_gpu_close,   1,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  {"TV 2x",                   false, swscale_gpu_init,   swscale_setpal,  tv2x_gpu_flip,         swscale_gpu_close,   1,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  {"Software bilinear",       false, swscale_gpu_init,   swscale_setpal,  swbilin_gpu_flip,      swscale_gpu_close,   1,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  {"Software bicubic",        false, swscale_gpu_init,   swscale_setpal,  swbicub_gpu_flip,      swscale_gpu_close,   1,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  {"Dot matrix",              false, swscale_gpu_init,   swscale_setpal,  dotmat_gpu_flip,       swscale_gpu_close,   1,         0, 0,          0, 0, 0, 0,  gpu_flip_b },
  /* SDL_Renderer plugins — use D3D11 on Windows, Metal on macOS, GL on Linux.
     No OpenGL context required; no multi-viewport support. flip_b is null. */
  {"Direct (SDL)",            false, sdlr_init,          direct_setpal,   sdlr_flip,     sdlr_close,          1,  0, 0,  0, 0, 0, 0,  nullptr },
  {"Super eagle (SDL)",       false, sdlr_swscale_init,  swscale_setpal,  seagle_flip,   sdlr_swscale_close,  1,  0, 0,  0, 0, 0, 0,  nullptr },
  {"Scale2x (SDL)",           false, sdlr_swscale_init,  swscale_setpal,  scale2x_flip,  sdlr_swscale_close,  1,  0, 0,  0, 0, 0, 0,  nullptr },
  {"TV 2x (SDL)",             false, sdlr_swscale_init,  swscale_setpal,  tv2x_flip,     sdlr_swscale_close,  1,  0, 0,  0, 0, 0, 0,  nullptr },
  {"Bilinear (SDL)",          false, sdlr_swscale_init,  swscale_setpal,  swbilin_flip,  sdlr_swscale_close,  1,  0, 0,  0, 0, 0, 0,  nullptr },
  {"Bicubic (SDL)",           false, sdlr_swscale_init,  swscale_setpal,  swbicub_flip,  sdlr_swscale_close,  1,  0, 0,  0, 0, 0, 0,  nullptr },
  /* CRT (GPU) — SDL3 GPU CRT shader plugins.  Metal + Vulkan + D3D12 backends. */
  {"CRT Basic",               false, crt_basic_gpu_init, direct_setpal,   crt_basic_gpu_flip_a,  crt_basic_gpu_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b },
  {"CRT Full",                false, crt_full_gpu_init,  direct_setpal,   crt_full_gpu_flip_a,   crt_full_gpu_close,  1, 0, 0, 0, 0, 0, 0, gpu_flip_b },
  {"CRT Lottes",              false, crt_lottes_gpu_init, direct_setpal,  crt_lottes_gpu_flip_a, crt_lottes_gpu_close,1, 0, 0, 0, 0, 0, 0, gpu_flip_b },
};
