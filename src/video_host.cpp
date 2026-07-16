/* video_host — the host presentation layer over subcycle::Machine's
 * framebuffer: plugin registry (GPU / SDL_Renderer / headless), window and
 * texture plumbing, scaling and aspect, palette handoff, devtools panel and
 * topbar composition. Authored here; the classic third-party scaling kernels
 * it drives live in src/scalers/ as a declared dependency
 * (docs/replacement-ledger.md).
 */

#include "video_host.h"

#include <math.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_ui.h"
#include "koncepcja.h"
#include "log.h"
#include "macos_menu.h"
#include "savepng.h"
#include "scalers/cpc_scalers.h"
#include "shaders/blit_shaders.h"
#include "video_gpu.h"

extern SDL_Window* mainSDLWindow;
SDL_Window* mainSDLWindow = nullptr;
namespace {
SDL_Renderer* renderer = nullptr;
}  // namespace

// SDL texture for CPC framebuffer (used by SDL_Renderer/D3D rendering path)
namespace {
SDL_Texture* cpc_sdl_texture = nullptr;
}  // namespace
// Which ImGui rendering backend is active
namespace {
bool using_sdl_renderer = false;
}  // namespace

// Returns path for imgui.ini in the same directory as koncepcja.cfg.
// Uses a static string so the c_str() pointer remains valid for io.IniFilename.
namespace {
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
}  // namespace

// the video surface ready to display
namespace {
SDL_Surface* vid = nullptr;
}  // namespace
// the video surface scaled with same format as pub
extern SDL_Surface* scaled;
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
SDL_Surface* scaled = nullptr;
// the video surface shown by the plugin to the application
extern SDL_Surface* pub;
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
SDL_Surface* pub = nullptr;

namespace {
int devtools_panel_width = 0;
}  // namespace
namespace {
int devtools_panel_height = 0;
}  // namespace
namespace {
int devtools_cpc_height = 0;
}  // namespace

namespace {
SDL_Surface* topbar_surface = nullptr;
}  // namespace
namespace {
int topbar_height = 0;
}  // namespace
namespace {
int bottombar_height = 0;
}  // namespace

extern t_CPC CPC;
extern video_plugin* vid_plugin;

// ── Triple-buffered CPC frame ring (decouples Z80 from render) ───────────────
// One writer (Z80 thread) and one reader (render thread).  The Z80 renders each
// frame into its private write buffer (back_surface ==
// g_cpc_ring[g_ring_write]) and publishes it without blocking.  The render
// thread copies the latest published buffer into the plugin's stable front-end
// surface (g_frontend, i.e. the `vid`/`pub` surface every flip already reads)
// and uploads as before.  The copy is a sub-millisecond same-format blit on the
// render thread — never on the Z80 critical path — so emulation pacing is
// untouched.
//
// Lock-free triple buffer: a single atomic `g_ring_shared` holds the published
// index (low 2 bits) plus a DIRTY bit (a frame is waiting).  BOTH threads
// atomically exchange against it, so {g_ring_write, g_ring_front, shared-index}
// always stay a permutation of {0,1,2} — the producer's write index and the
// consumer's read index are therefore always distinct, with no lock and no
// read-then-claim TOCTOU window.  The DIRTY bit lets the consumer present
// idempotently: re-presenting with no new frame re-blits g_ring_front (needed
// by the macOS menu-tracking driver, which ticks faster than the Z80 produces).
static constexpr int RING_INDEX_MASK = 0x3;
static constexpr int RING_DIRTY = 0x4;
namespace {
SDL_Surface* g_frontend = nullptr;  // plugin CPC source = copy dest
}  // namespace
namespace {
SDL_Surface* g_cpc_ring[3] = {nullptr, nullptr, nullptr};
}  // namespace
namespace {
std::atomic<int> g_ring_shared{2};  // published index | DIRTY
}  // namespace
namespace {
int g_ring_write = 0;  // Z80-thread-private write (back_surface) index
}  // namespace
namespace {
int g_ring_front = 1;  // render-thread-private last-read index
}  // namespace
namespace {
bool g_ring_active = false;
}  // namespace

// Called once from video_init() after the plugin's surface exists.  `frontend`
// is the plugin's CPC source surface (the Z80's historical write target).  We
// keep it as the render-private copy destination and allocate three matching
// write buffers.  Returns the Z80's initial write surface.
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
SDL_Surface* video_ring_init(SDL_Surface* frontend) {
  video_ring_shutdown();  // free any prior ring (restyle path)
  if (!frontend) return frontend;
  g_frontend = frontend;
  SDL_Palette* pal = SDL_GetSurfacePalette(frontend);
  for (auto& i : g_cpc_ring) {
    i = SDL_CreateSurface(frontend->w, frontend->h, frontend->format);
    if (!i) {
      LOG_ERROR(
          "video_ring_init: SDL_CreateSurface failed: " << SDL_GetError());
      // Degrade gracefully to single-buffer: free any partial ring but keep
      // g_frontend so video_render_surface() stays valid (== back_surface).
      for (auto& j : g_cpc_ring) {
        if (j) {
          SDL_DestroySurface(j);
          j = nullptr;
        }
      }
      g_ring_active = false;
      return frontend;
    }
    if (pal) SDL_SetSurfacePalette(i, pal);
  }
  g_ring_write = 0;
  g_ring_front = 1;
  g_ring_shared.store(2, std::memory_order_relaxed);  // permutation {0,1,2}
  g_ring_active = true;
  return g_cpc_ring[0];
}

// Z80 thread, at frame boundary: publish the just-written buffer (set DIRTY)
// and take the previously-published buffer as the next write target.  The
// atomic exchange keeps {write, front, shared} a permutation of {0,1,2}, so the
// new write buffer is never the one the render thread holds.  Returns the new
// write surface; the caller assigns back_surface to it.
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
SDL_Surface* video_ring_publish() {
  if (!g_ring_active) return g_frontend;
  int const prev = g_ring_shared.exchange(g_ring_write | RING_DIRTY,
                                          std::memory_order_acq_rel);
  g_ring_write = prev & RING_INDEX_MASK;
  return g_cpc_ring[g_ring_write];
}

// Render thread, before the flip: if a new frame was published (DIRTY), swap
// our front buffer in for it (atomic exchange, clears DIRTY); then copy the
// front buffer into the stable front-end surface the flip reads.  Idempotent —
// calling it again with no new frame re-blits the same front buffer (no
// tearing, no stale-buffer flicker for the menu-tracking driver).
void video_ring_present() {
  if (!g_ring_active) return;
  if (g_ring_shared.load(std::memory_order_acquire) & RING_DIRTY) {
    int const prev =
        g_ring_shared.exchange(g_ring_front, std::memory_order_acq_rel);
    g_ring_front = prev & RING_INDEX_MASK;
  }
  SDL_BlitSurface(g_cpc_ring[g_ring_front], nullptr, g_frontend, nullptr);
}

// The surface the render thread should read/write for the displayed frame
// (OSD text, screenshots, dock preview, thumbnails).  Equals the plugin's
// front-end surface, now holding the most-recently-presented frame.
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
SDL_Surface* video_render_surface() { return g_frontend; }

// The most recently PUBLISHED frame — the Z80 thread's actual output —
// without involving the render thread.  For screenshots/diagnostics: the
// presented g_frontend goes stale whenever the present path stalls (occluded
// macOS window, remote desktop) even though emulation keeps running.  The
// returned buffer becomes the writer's target again after its next publish,
// so a capture can tear mid-frame at worst; pause the emulation first for an
// exact frame.  Null when the ring is inactive (headless single-buffer).
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
SDL_Surface* video_ring_published_peek() {
  if (!g_ring_active) return nullptr;
  return g_cpc_ring[g_ring_shared.load(std::memory_order_acquire) &
                    RING_INDEX_MASK];
}

void video_ring_shutdown() {
  for (auto& i : g_cpc_ring) {
    if (i) {
      SDL_DestroySurface(i);
      i = nullptr;
    }
  }
  g_frontend = nullptr;
  g_ring_active = false;
}

// Window screenshot: set a pending path for capture by the main thread.
// The capture happens in direct_flip() after ImGui is rendered.
#include <mutex>
#include <vector>
namespace {
std::mutex g_wss_mutex;
}  // namespace
namespace {
std::string g_wss_pending_path;
}  // namespace

std::atomic<bool> g_repaint_pending{false};
std::atomic<bool> g_repaint_done{false};
std::mutex g_repaint_mutex;
std::string g_repaint_screenshot_path;
std::string g_repaint_error;

void video_request_window_screenshot(const std::string& path) {
  std::scoped_lock const lock(g_wss_mutex);
  g_wss_pending_path = path;
}

// Returns the CPC screen texture as an opaque ImTextureID-compatible value.
// The actual type is backend-dependent: SDL_Texture* for SDL_Renderer plugins,
// SDL_GPUTexture* for SDL3 GPU plugins.  Callers only use it as an ImTextureID.
uintptr_t video_get_cpc_texture() {
  if (cpc_sdl_texture) return reinterpret_cast<uintptr_t>(cpc_sdl_texture);
  // GPU plugin active — ImGui's SDLGPU3 backend accepts SDL_GPUTexture* as
  // an ImTextureID for Docked-mode ImGui::Image() calls on the CPC Screen.
  if (g_gpu.cpc_texture) return reinterpret_cast<uintptr_t>(g_gpu.cpc_texture);
  return 0;
}

void video_get_cpc_size(int& w, int& h) {
  if (vid) {
    w = vid->w;
    h = vid->h;
  } else {
    w = 0;
    h = 0;
  }
}

bool video_is_sdl_renderer() { return using_sdl_renderer; }

// ── Save-state slot thumbnails ──────────────────────────────────────────────
// A ".kthm" file is a 16-byte header followed by h*w*4 RGBA8 bytes:
//   struct { char magic[4]="KTHM"; int32_t w; int32_t h; int32_t reserved; }
// Stored little-endian (the only platforms we target are LE).

namespace {
constexpr char kThmMagic[4] = {'K', 'T', 'H', 'M'};
constexpr int kThmMaxDim = 1024;

struct ThmHeader {
  char magic[4];
  int32_t w;
  int32_t h;
  int32_t reserved;
};
}  // namespace

uintptr_t video_make_rgba_texture(const unsigned char* rgba, int w, int h) {
  if (!rgba || w <= 0 || h <= 0) return 0;

  if (using_sdl_renderer && renderer) {
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) {
      LOG_ERROR("video_make_rgba_texture: SDL_CreateTexture failed: "
                << SDL_GetError());
      return 0;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    if (!SDL_UpdateTexture(tex, nullptr, rgba, w * 4)) {
      LOG_ERROR("video_make_rgba_texture: SDL_UpdateTexture failed: "
                << SDL_GetError());
      SDL_DestroyTexture(tex);
      return 0;
    }
    return reinterpret_cast<uintptr_t>(tex);
  }

  if (g_gpu.device) {
    return video_gpu_make_rgba_texture(rgba, w, h);
  }

  return 0;  // headless or no backend
}

void video_free_rgba_texture(uintptr_t tex) {
  if (!tex) return;
  if (using_sdl_renderer && renderer) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr): intentional integer->pointer
    // for an opaque host handle
    SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(tex));
    return;
  }
  if (g_gpu.device) {
    video_gpu_free_rgba_texture(tex);
  }
}

bool video_capture_cpc_thumbnail(const std::string& path, int max_w) {
  if (!vid || !vid->pixels || vid->w <= 0 || vid->h <= 0 || max_w <= 0) {
    return false;
  }
  // The downscale loop below indexes pixels as 4-byte RGBA; bail rather than
  // read out of bounds if the back surface is ever not 32-bit.
  if (SDL_BYTESPERPIXEL(vid->format) != 4) {
    return false;
  }

  const int src_w = vid->w;
  const int src_h = vid->h;
  int dst_w = std::min(max_w, src_w);
  dst_w = std::max(dst_w, 1);
  // Rasterize to the CPC's 4:3 monitor ratio rather than the raw (very wide,
  // ~768:270) framebuffer aspect, so the preview matches how the screen
  // actually looks and isn't vertically stretched in the 4:3 grid cell.  The
  // downscale loop below samples each axis independently, so this just maps the
  // raw frame into a 4:3 target.
  int dst_h = dst_w * 3 / 4;
  dst_h = std::max(dst_h, 1);
  dst_w = std::min(dst_w, kThmMaxDim);
  dst_h = std::min(dst_h, kThmMaxDim);

  // Nearest-neighbor downscale.  vid is RGBA32 (4 bytes/pixel) with pitch.
  std::vector<unsigned char> out(static_cast<size_t>(dst_w) * dst_h * 4);
  const auto* src = static_cast<const unsigned char*>(vid->pixels);
  const int pitch = vid->pitch;
  for (int y = 0; y < dst_h; ++y) {
    int sy = static_cast<int>(static_cast<long long>(y) * src_h / dst_h);
    if (sy >= src_h) sy = src_h - 1;
    const unsigned char* srow = src + (static_cast<size_t>(sy) * pitch);
    unsigned char* drow = out.data() + (static_cast<size_t>(y) * dst_w * 4);
    for (int x = 0; x < dst_w; ++x) {
      int sx = static_cast<int>(static_cast<long long>(x) * src_w / dst_w);
      if (sx >= src_w) sx = src_w - 1;
      std::memcpy(drow + (x * 4), srow + (sx * 4), 4);
    }
  }

  ThmHeader hdr{};
  std::memcpy(hdr.magic, kThmMagic, 4);
  hdr.w = dst_w;
  hdr.h = dst_h;
  hdr.reserved = 0;

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    LOG_ERROR("video_capture_cpc_thumbnail: cannot open " << path);
    return false;
  }
  f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
  f.write(reinterpret_cast<const char*>(out.data()),
          static_cast<std::streamsize>(out.size()));
  if (!f.good()) {
    LOG_ERROR("video_capture_cpc_thumbnail: write failed for " << path);
    return false;
  }
  return true;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
bool video_load_rgba_thumbnail(const std::string& path,
                               std::vector<unsigned char>& rgba, int& w,
                               int& h) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;

  ThmHeader hdr{};
  f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
  if (!f.good() || std::memcmp(hdr.magic, kThmMagic, 4) != 0) return false;
  if (hdr.w < 1 || hdr.w > kThmMaxDim || hdr.h < 1 || hdr.h > kThmMaxDim) {
    return false;
  }

  const size_t bytes = static_cast<size_t>(hdr.w) * hdr.h * 4;
  rgba.resize(bytes);
  f.read(reinterpret_cast<char*>(rgba.data()),
         static_cast<std::streamsize>(bytes));
  if (static_cast<size_t>(f.gcount()) != bytes) return false;

  w = hdr.w;
  h = hdr.h;
  return true;
}

// Offscreen FBO-into-texture rendering used to be backed by an OpenGL
// FBO cache (see Phase 7c.1b deletion).  After the GL plugins were
// removed there's no GL context to host an FBO, so this entry point
// is a stub returning 0 — callers (imgui_ui.cpp plotter canvas) have
// a per-frame ImDrawList fallback for the case where no FBO texture
// is available.
uintptr_t video_offscreen_texture(
    const char* /*key*/, int /*canvas_w*/, int /*canvas_h*/,
    size_t /*dirty_marker*/,
    const std::function<void(ImDrawList*, int, int)>& /*draw_fn*/) {
  return 0;
}

// Called from direct_flip_a() and swscale_blit_a() to capture the current frame
namespace {
void video_capture_if_pending() {
  std::string wss_path;
  {
    std::scoped_lock const lock(g_wss_mutex);
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
}  // namespace

void video_take_pending_window_screenshot() {
  // no-op: capture happens inside flip handlers via video_capture_if_pending
}

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

// Returns a bpp compatible with the renderer
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
int renderer_bpp(SDL_Renderer* sdl_renderer) {
  (void)sdl_renderer;
  return 32;
}

// TODO: Cleanup sw_scaling if really not needed
namespace {
void compute_scale(video_plugin* t, int w, int h) {
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
    static const float scale_factors[] = {0.f, 1.f, 1.5f, 2.f, 3.f};
    // Target aspect ratio: 4:3 for CRT monitors, or raw pixel ratio
    float const target_aspect =
        CPC.scr_crt_aspect ? (4.f / 3.f) : (static_cast<float>(w) / h);
    if (CPC.scr_scale > 0 &&
        CPC.scr_scale < sizeof(scale_factors) / sizeof(scale_factors[0])) {
      // Fixed scale: exact pixel multiple. If window is smaller, image is
      // cropped (centered) — never scaled down.  Options resizes the window
      // to fit when the user picks a new scale.
      float const sf = scale_factors[CPC.scr_scale];
      disp_w = static_cast<int>(CPC_RENDER_WIDTH * sf);
      disp_h = static_cast<int>(disp_w / target_aspect);
    } else {
      // Fit window: fill available space preserving target aspect ratio.
      float const win_aspect = static_cast<float>(win_width) / win_height;
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
    t->x_offset = 0;
    t->y_offset = 0;
    t->x_scale = w / static_cast<float>(win_width);
    t->y_scale = h / static_cast<float>(win_height);
    t->width = win_width;
    t->height = win_height;
  }
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* Half size video plugin
 * ------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------
 */

namespace {
void direct_setpal(SDL_Color* c) {
  if (SDL_Palette* pal = SDL_GetSurfacePalette(vid)) {
    SDL_SetPaletteColors(pal, c, 0, 32);
  }
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* "Direct (GPU)" plugin — SDL3 GPU path, additive (P1.2b Phase 4)
 * --------------------- */
/* ------------------------------------------------------------------------------------
 */
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
//   - Multi-viewport ENABLED — secondary windows are rendered by the
//     renderer hooks we added to imgui_impl_sdlgpu3.cpp (ClaimWindowForGPU
//     in Renderer_CreateWindow, AcquireSwapchain+RenderDrawData+Submit in
//     Renderer_RenderWindow / Renderer_SwapBuffers).
//   - Non-blocking swapchain acquire — never blocks the render thread.

namespace {
SDL_Surface* gpu_direct_init(video_plugin* t, int scale, bool fs) {
  // HIGH_PIXEL_DENSITY: on a Retina display the GPU swapchain is created at the
  // full backing pixel size (2x) instead of macOS upscaling a low-res drawable,
  // so both ImGui and the CPC blit render crisply. The window size stays in
  // logical points, so "1x" is the same physical size — just sharp. The
  // CPC-blit viewport is scaled to the swapchain's pixel size in the flip
  // below; ImGui handles its own framebuffer scale. (Point-space input mapping
  // is unaffected.)
  mainSDLWindow =
      SDL_CreateWindow("konCePCja " VERSION_STRING, CPC_RENDER_WIDTH * scale,
                       CPC_VISIBLE_SCR_HEIGHT * scale,
                       (fs ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_RESIZABLE |
                           SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!mainSDLWindow) return nullptr;

  const int surface_width = CPC_RENDER_WIDTH;
  const int surface_height =
      (scale > 1) ? CPC_VISIBLE_SCR_HEIGHT * 2 : CPC_VISIBLE_SCR_HEIGHT;
  t->half_pixels = (scale <= 1) ? 1 : 0;

  if (!video_gpu_init(mainSDLWindow, static_cast<uint32_t>(surface_width),
                      static_cast<uint32_t>(surface_height)) ||
      g_gpu.blit_pipeline == nullptr) {
    video_gpu_shutdown();
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
    return nullptr;
  }

  // video.vsync escape hatch: switch the MAIN window off VSYNC when requested.
  // Viewport windows keep VSYNC via init_info.PresentMode below.
  video_gpu_set_main_present_mode(CPC.scr_vsync != 0);

  // ImGui — SDLGPU3 backend with multi-viewport ENABLED.  The renderer
  // hooks live in vendor/imgui/backends/imgui_impl_sdlgpu3.cpp; they
  // claim each secondary window for g_gpu.device on creation and submit
  // a per-viewport command buffer on render.  ImGui_ImplSDLGPU3_Init
  // checks io.ConfigFlags after we set the flag and registers the
  // hooks itself, so order matters: set flags BEFORE Init.
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = imgui_ini_path();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                    ImGuiConfigFlags_DockingEnable |
                    ImGuiConfigFlags_ViewportsEnable;
  ImGui::StyleColorsDark();
  imgui_init_ui();
  ImGui_ImplSDL3_InitForSDLGPU(mainSDLWindow);
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
    video_gpu_shutdown();
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
    return nullptr;
  }

  vid =
      SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGBA32);
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
    SDL_Palette const* pal = SDL_GetSurfacePalette(vid);
    SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(fmt, pal, 0, 0, 0));
  }
  compute_scale(t, surface_width, surface_height);
  LOG_INFO("Direct (GPU) plugin active — device created, blit pipeline ready");
  return vid;
}
}  // namespace

namespace {
void gpu_flip_a(video_plugin* t) {
  compute_scale(t, vid->w, vid->h);

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_gpu.device);
  if (!cmd) return;  // device lost or OOM — drop the frame

  // 1. Upload CPC framebuffer via transfer buffer (cycle=true on both).
  const uint32_t row_bytes = g_gpu.cpc_tex_w * 4;
  void* dst =
      SDL_MapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload, /*cycle=*/true);
  if (dst) {
    if (static_cast<uint32_t>(vid->pitch) == row_bytes) {
      std::memcpy(dst, vid->pixels, row_bytes * g_gpu.cpc_tex_h);
    } else {
      auto* d = static_cast<uint8_t*>(dst);
      auto* s = static_cast<const uint8_t*>(vid->pixels);
      for (uint32_t y = 0; y < g_gpu.cpc_tex_h; ++y) {
        std::memcpy(d + (y * row_bytes), s + (y * vid->pitch), row_bytes);
      }
    }
    SDL_UnmapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src_info{};
    src_info.transfer_buffer = g_gpu.cpc_upload;
    src_info.offset = 0;
    src_info.pixels_per_row = g_gpu.cpc_tex_w;
    src_info.rows_per_layer = g_gpu.cpc_tex_h;

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
  bool const have_swap =
      SDL_AcquireGPUSwapchainTexture(cmd, mainSDLWindow, &swap_tex, &sw, &sh) &&
      swap_tex != nullptr;

  if (have_swap) {
    SDL_GPUColorTargetInfo tgt{};
    tgt.texture = swap_tex;
    tgt.load_op = SDL_GPU_LOADOP_CLEAR;
    tgt.store_op = SDL_GPU_STOREOP_STORE;
    tgt.cycle = false;
    tgt.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);

    if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
      SDL_GPUViewport vp{};
      // t->* come from compute_scale() in logical points; the swapchain (sw x
      // sh) is in pixels. On a HiDPI window point size != pixel size, so scale
      // the CPC-image viewport by the per-axis density. Non-HiDPI windows have
      // sw==point width, making this a 1.0 no-op.
      int win_pt_w = 0, win_pt_h = 0;
      SDL_GetWindowSize(mainSDLWindow, &win_pt_w, &win_pt_h);
      const float vp_ds_x =
          win_pt_w > 0 ? static_cast<float>(sw) / win_pt_w : 1.0f;
      const float vp_ds_y =
          win_pt_h > 0 ? static_cast<float>(sh) / win_pt_h : 1.0f;
      vp.x = t->x_offset * vp_ds_x;
      vp.y = t->y_offset * vp_ds_y;
      vp.w = t->width * vp_ds_x;
      vp.h = t->height * vp_ds_y;
      vp.max_depth = 1.0f;
      SDL_SetGPUViewport(pass, &vp);

      SDL_BindGPUGraphicsPipeline(pass, g_gpu.blit_pipeline);
      SDL_GPUTextureSamplerBinding binding{};
      binding.texture = g_gpu.cpc_texture;
      binding.sampler =
          CPC.scr_crt_aspect ? g_gpu.linear_sampler : g_gpu.nearest_sampler;
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

  // 7. Multi-viewport: render every secondary ImGui window.  The renderer
  //    hooks in imgui_impl_sdlgpu3.cpp acquire + submit one command buffer
  //    per viewport.  RenderPlatformWindowsDefault skips the main viewport
  //    (already rendered above) so there's no double-render race.
  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
  }
}
}  // namespace

namespace {
void gpu_flip_b([[maybe_unused]] video_plugin* t) {
  // Intentionally empty for the GPU plugin:
  //   - Command buffer was already submitted in gpu_flip_a.
  //   - ImGui multi-viewport is disabled, so no per-viewport work needed.
  // The render loop's "skip on quit" optimisation is therefore harmless.
}
}  // namespace

namespace {
void gpu_direct_close() {
  // Teardown order is critical — see the plan's teardown-order table.
  // INVARIANT: no pending_cmd exists here (submitted in gpu_flip_a).

  if (g_gpu.device) SDL_WaitForGPUIdle(g_gpu.device);

  if (ImGui::GetCurrentContext()) {
    ImGui_ImplSDLGPU3_Shutdown();  // releases bd state, still needs device
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
  if (vid) {
    SDL_DestroySurface(vid);
    vid = nullptr;
  }
  video_gpu_shutdown();  // destroys device, samplers, texture…
  if (mainSDLWindow) {
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
  }
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* "CRT Basic (GPU)" plugin — SDL3 GPU path (P1.2b Phase 6b)
 * --------------------------- */
/* ------------------------------------------------------------------------------------
 */
//
// GPU variant of CRT Basic.  Single-pass: instead of blit_pipeline, the
// main render pass binds a CRT shader pipeline that samples the CPC
// texture through barrel distortion, scanline mixing, and an RGB
// phosphor mask.  Uniforms (input_size, output_size) pushed per frame.
//
// MSL source only; SPIRV/DXBC deferred.  On non-Metal backends the
// init returns nullptr and video_init falls through.

namespace {
struct CrtBasicUniforms {
  float input_size[2];
  float output_size[2];
};
}  // namespace

namespace {
SDL_GPUShader* g_crt_basic_vertex_shader = nullptr;
}  // namespace
namespace {
SDL_GPUShader* g_crt_basic_fragment_shader = nullptr;
}  // namespace
namespace {
SDL_GPUGraphicsPipeline* g_crt_basic_pipeline = nullptr;
}  // namespace

namespace {
bool create_crt_basic_pipeline() {
  if (!g_gpu.device) return false;
  const char* driver = SDL_GetGPUDeviceDriver(g_gpu.device);
  if (!driver) return false;

  SDL_GPUShaderCreateInfo vsi{};
  vsi.stage = SDL_GPU_SHADERSTAGE_VERTEX;

  SDL_GPUShaderCreateInfo fsi{};
  fsi.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fsi.num_samplers = 1;
  fsi.num_uniform_buffers = 1;

  if (std::strcmp(driver, "metal") == 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_MSL;
    vsi.code = reinterpret_cast<const Uint8*>(kCrtBasicMSLSource);
    vsi.code_size = std::strlen(kCrtBasicMSLSource);
    vsi.entrypoint = "vert_main";
    fsi.format = SDL_GPU_SHADERFORMAT_MSL;
    fsi.code = reinterpret_cast<const Uint8*>(kCrtBasicMSLSource);
    fsi.code_size = std::strlen(kCrtBasicMSLSource);
    fsi.entrypoint = "frag_main";
  } else if (std::strcmp(driver, "vulkan") == 0 && kBlitVertexSPIRVSize > 0 &&
             kCrtBasicFragmentSPIRVSize > 0) {
    // Reuses the blit vertex shader — both emit v_uv at location 0 from
    // a gl_VertexIndex-driven fullscreen triangle.
    vsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vsi.code = kBlitVertexSPIRV;
    vsi.code_size = kBlitVertexSPIRVSize;
    vsi.entrypoint = "main";
    fsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fsi.code = kCrtBasicFragmentSPIRV;
    fsi.code_size = kCrtBasicFragmentSPIRVSize;
    fsi.entrypoint = "main";
  } else if (std::strcmp(driver, "direct3d12") == 0 &&
             kBlitVertexDXBCSize > 0 && kCrtBasicFragmentDXBCSize > 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_DXBC;
    vsi.code = kBlitVertexDXBC;
    vsi.code_size = kBlitVertexDXBCSize;
    vsi.entrypoint = "main";
    fsi.format = SDL_GPU_SHADERFORMAT_DXBC;
    fsi.code = kCrtBasicFragmentDXBC;
    fsi.code_size = kCrtBasicFragmentDXBCSize;
    fsi.entrypoint = "main";
  } else {
    return false;  // no shader blob available for this backend
  }

  g_crt_basic_vertex_shader = SDL_CreateGPUShader(g_gpu.device, &vsi);
  g_crt_basic_fragment_shader = SDL_CreateGPUShader(g_gpu.device, &fsi);
  if (!g_crt_basic_vertex_shader || !g_crt_basic_fragment_shader) {
    LOG_ERROR("CRT Basic (GPU) shader create failed: " << SDL_GetError());
    if (g_crt_basic_vertex_shader) {
      SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_vertex_shader);
      g_crt_basic_vertex_shader = nullptr;
    }
    if (g_crt_basic_fragment_shader) {
      SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_fragment_shader);
      g_crt_basic_fragment_shader = nullptr;
    }
    return false;
  }

  SDL_GPUColorTargetDescription color_target{};
  color_target.format = g_gpu.swapchain_fmt;

  SDL_GPUGraphicsPipelineTargetInfo target_info{};
  target_info.num_color_targets = 1;
  target_info.color_target_descriptions = &color_target;
  target_info.has_depth_stencil_target = false;

  SDL_GPUGraphicsPipelineCreateInfo info{};
  info.vertex_shader = g_crt_basic_vertex_shader;
  info.fragment_shader = g_crt_basic_fragment_shader;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.target_info = target_info;

  g_crt_basic_pipeline = SDL_CreateGPUGraphicsPipeline(g_gpu.device, &info);
  if (!g_crt_basic_pipeline) {
    LOG_ERROR("CRT Basic (GPU) pipeline create failed: " << SDL_GetError());
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_vertex_shader);
    g_crt_basic_vertex_shader = nullptr;
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_fragment_shader);
    g_crt_basic_fragment_shader = nullptr;
    return false;
  }
  return true;
}
}  // namespace

namespace {
void destroy_crt_basic_pipeline() {
  if (!g_gpu.device) return;
  if (g_crt_basic_pipeline) {
    SDL_ReleaseGPUGraphicsPipeline(g_gpu.device, g_crt_basic_pipeline);
    g_crt_basic_pipeline = nullptr;
  }
  if (g_crt_basic_fragment_shader) {
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_fragment_shader);
    g_crt_basic_fragment_shader = nullptr;
  }
  if (g_crt_basic_vertex_shader) {
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_basic_vertex_shader);
    g_crt_basic_vertex_shader = nullptr;
  }
}
}  // namespace

namespace {
SDL_Surface* crt_basic_gpu_init(video_plugin* t, int scale, bool fs) {
  SDL_Surface* surf = gpu_direct_init(t, scale, fs);  // reuses Phase 4 setup
  if (!surf) return nullptr;
  if (!create_crt_basic_pipeline()) {
    gpu_direct_close();
    return nullptr;
  }
  LOG_INFO("CRT Basic (GPU) plugin active");
  return surf;
}
}  // namespace

namespace {
void crt_basic_gpu_flip_a(video_plugin* t) {
  compute_scale(t, vid->w, vid->h);

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_gpu.device);
  if (!cmd) return;

  // 1. Upload CPC framebuffer (identical to gpu_flip_a).
  const uint32_t row_bytes = g_gpu.cpc_tex_w * 4;
  void* dst =
      SDL_MapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload, /*cycle=*/true);
  if (dst) {
    if (static_cast<uint32_t>(vid->pitch) == row_bytes) {
      std::memcpy(dst, vid->pixels, row_bytes * g_gpu.cpc_tex_h);
    } else {
      auto* d = static_cast<uint8_t*>(dst);
      auto* s = static_cast<const uint8_t*>(vid->pixels);
      for (uint32_t y = 0; y < g_gpu.cpc_tex_h; ++y)
        std::memcpy(d + (y * row_bytes), s + (y * vid->pitch), row_bytes);
    }
    SDL_UnmapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src_info{};
    src_info.transfer_buffer = g_gpu.cpc_upload;
    src_info.pixels_per_row = g_gpu.cpc_tex_w;
    src_info.rows_per_layer = g_gpu.cpc_tex_h;
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
  bool const have_swap =
      SDL_AcquireGPUSwapchainTexture(cmd, mainSDLWindow, &swap_tex, &sw, &sh) &&
      swap_tex != nullptr;

  if (have_swap) {
    SDL_GPUColorTargetInfo tgt{};
    tgt.texture = swap_tex;
    tgt.load_op = SDL_GPU_LOADOP_CLEAR;
    tgt.store_op = SDL_GPU_STOREOP_STORE;
    tgt.cycle = false;
    tgt.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);

    if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
      SDL_GPUViewport vp{};
      // t->* come from compute_scale() in logical points; the swapchain (sw x
      // sh) is in pixels. On a HiDPI window point size != pixel size, so scale
      // the CPC-image viewport by the per-axis density. Non-HiDPI windows have
      // sw==point width, making this a 1.0 no-op.
      int win_pt_w = 0, win_pt_h = 0;
      SDL_GetWindowSize(mainSDLWindow, &win_pt_w, &win_pt_h);
      const float vp_ds_x =
          win_pt_w > 0 ? static_cast<float>(sw) / win_pt_w : 1.0f;
      const float vp_ds_y =
          win_pt_h > 0 ? static_cast<float>(sh) / win_pt_h : 1.0f;
      vp.x = t->x_offset * vp_ds_x;
      vp.y = t->y_offset * vp_ds_y;
      vp.w = t->width * vp_ds_x;
      vp.h = t->height * vp_ds_y;
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
      uni.input_size[0] = static_cast<float>(vid->w);
      uni.input_size[1] =
          static_cast<float>(t->half_pixels ? vid->h : vid->h / 2);
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

  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
  }
}
}  // namespace

namespace {
void crt_basic_gpu_close() {
  if (g_gpu.device) SDL_WaitForGPUIdle(g_gpu.device);
  destroy_crt_basic_pipeline();
  gpu_direct_close();
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* "CRT Full (GPU)" plugin — SDL3 GPU port of crt_frag_full (Phase 6c)
 * ----------------- */
/* ------------------------------------------------------------------------------------
 */
//
// Same uniform struct as CRT Basic (input_size / output_size); the
// additional curvature / scanline / mask / bloom / vignette knobs
// (which the GL path passed as 5 float uniforms but always with the
// same hard-coded values) are inlined as constants in the shader.

namespace {
SDL_GPUShader* g_crt_full_vertex_shader = nullptr;
}  // namespace
namespace {
SDL_GPUShader* g_crt_full_fragment_shader = nullptr;
}  // namespace
namespace {
SDL_GPUGraphicsPipeline* g_crt_full_pipeline = nullptr;
}  // namespace

namespace {
bool create_crt_full_pipeline() {
  if (!g_gpu.device) return false;
  const char* driver = SDL_GetGPUDeviceDriver(g_gpu.device);
  if (!driver) return false;

  SDL_GPUShaderCreateInfo vsi{};
  vsi.stage = SDL_GPU_SHADERSTAGE_VERTEX;

  SDL_GPUShaderCreateInfo fsi{};
  fsi.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fsi.num_samplers = 1;
  fsi.num_uniform_buffers = 1;

  if (std::strcmp(driver, "metal") == 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_MSL;
    vsi.code = reinterpret_cast<const Uint8*>(kCrtFullMSLSource);
    vsi.code_size = std::strlen(kCrtFullMSLSource);
    vsi.entrypoint = "vert_main";
    fsi.format = SDL_GPU_SHADERFORMAT_MSL;
    fsi.code = reinterpret_cast<const Uint8*>(kCrtFullMSLSource);
    fsi.code_size = std::strlen(kCrtFullMSLSource);
    fsi.entrypoint = "frag_main";
  } else if (std::strcmp(driver, "vulkan") == 0 && kBlitVertexSPIRVSize > 0 &&
             kCrtFullFragmentSPIRVSize > 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vsi.code = kBlitVertexSPIRV;
    vsi.code_size = kBlitVertexSPIRVSize;
    vsi.entrypoint = "main";
    fsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fsi.code = kCrtFullFragmentSPIRV;
    fsi.code_size = kCrtFullFragmentSPIRVSize;
    fsi.entrypoint = "main";
  } else if (std::strcmp(driver, "direct3d12") == 0 &&
             kBlitVertexDXBCSize > 0 && kCrtFullFragmentDXBCSize > 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_DXBC;
    vsi.code = kBlitVertexDXBC;
    vsi.code_size = kBlitVertexDXBCSize;
    vsi.entrypoint = "main";
    fsi.format = SDL_GPU_SHADERFORMAT_DXBC;
    fsi.code = kCrtFullFragmentDXBC;
    fsi.code_size = kCrtFullFragmentDXBCSize;
    fsi.entrypoint = "main";
  } else {
    return false;
  }

  g_crt_full_vertex_shader = SDL_CreateGPUShader(g_gpu.device, &vsi);
  g_crt_full_fragment_shader = SDL_CreateGPUShader(g_gpu.device, &fsi);
  if (!g_crt_full_vertex_shader || !g_crt_full_fragment_shader) {
    LOG_ERROR("CRT Full (GPU) shader create failed: " << SDL_GetError());
    if (g_crt_full_vertex_shader) {
      SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_vertex_shader);
      g_crt_full_vertex_shader = nullptr;
    }
    if (g_crt_full_fragment_shader) {
      SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_fragment_shader);
      g_crt_full_fragment_shader = nullptr;
    }
    return false;
  }

  SDL_GPUColorTargetDescription color_target{};
  color_target.format = g_gpu.swapchain_fmt;

  SDL_GPUGraphicsPipelineTargetInfo target_info{};
  target_info.num_color_targets = 1;
  target_info.color_target_descriptions = &color_target;
  target_info.has_depth_stencil_target = false;

  SDL_GPUGraphicsPipelineCreateInfo info{};
  info.vertex_shader = g_crt_full_vertex_shader;
  info.fragment_shader = g_crt_full_fragment_shader;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.target_info = target_info;

  g_crt_full_pipeline = SDL_CreateGPUGraphicsPipeline(g_gpu.device, &info);
  if (!g_crt_full_pipeline) {
    LOG_ERROR("CRT Full (GPU) pipeline create failed: " << SDL_GetError());
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_vertex_shader);
    g_crt_full_vertex_shader = nullptr;
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_fragment_shader);
    g_crt_full_fragment_shader = nullptr;
    return false;
  }
  return true;
}
}  // namespace

namespace {
void destroy_crt_full_pipeline() {
  if (!g_gpu.device) return;
  if (g_crt_full_pipeline) {
    SDL_ReleaseGPUGraphicsPipeline(g_gpu.device, g_crt_full_pipeline);
    g_crt_full_pipeline = nullptr;
  }
  if (g_crt_full_fragment_shader) {
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_fragment_shader);
    g_crt_full_fragment_shader = nullptr;
  }
  if (g_crt_full_vertex_shader) {
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_full_vertex_shader);
    g_crt_full_vertex_shader = nullptr;
  }
}
}  // namespace

namespace {
SDL_Surface* crt_full_gpu_init(video_plugin* t, int scale, bool fs) {
  SDL_Surface* surf = gpu_direct_init(t, scale, fs);
  if (!surf) return nullptr;
  if (!create_crt_full_pipeline()) {
    gpu_direct_close();
    return nullptr;
  }
  LOG_INFO("CRT Full (GPU) plugin active");
  return surf;
}
}  // namespace

// CRT Full flip_a: same copy-pass + render flow as crt_basic_gpu_flip_a,
// only differs in which pipeline is bound.  Factored inline to keep
// the per-plugin lifecycle explicit.
namespace {
void crt_full_gpu_flip_a(video_plugin* t) {
  compute_scale(t, vid->w, vid->h);

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_gpu.device);
  if (!cmd) return;

  const uint32_t row_bytes = g_gpu.cpc_tex_w * 4;
  void* dst =
      SDL_MapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload, /*cycle=*/true);
  if (dst) {
    if (static_cast<uint32_t>(vid->pitch) == row_bytes) {
      std::memcpy(dst, vid->pixels, row_bytes * g_gpu.cpc_tex_h);
    } else {
      auto* d = static_cast<uint8_t*>(dst);
      auto* s = static_cast<const uint8_t*>(vid->pixels);
      for (uint32_t y = 0; y < g_gpu.cpc_tex_h; ++y)
        std::memcpy(d + (y * row_bytes), s + (y * vid->pitch), row_bytes);
    }
    SDL_UnmapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src_info{};
    src_info.transfer_buffer = g_gpu.cpc_upload;
    src_info.pixels_per_row = g_gpu.cpc_tex_w;
    src_info.rows_per_layer = g_gpu.cpc_tex_h;
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
  bool const have_swap =
      SDL_AcquireGPUSwapchainTexture(cmd, mainSDLWindow, &swap_tex, &sw, &sh) &&
      swap_tex != nullptr;

  if (have_swap) {
    SDL_GPUColorTargetInfo tgt{};
    tgt.texture = swap_tex;
    tgt.load_op = SDL_GPU_LOADOP_CLEAR;
    tgt.store_op = SDL_GPU_STOREOP_STORE;
    tgt.cycle = false;
    tgt.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);

    if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
      SDL_GPUViewport vp{};
      // t->* come from compute_scale() in logical points; the swapchain (sw x
      // sh) is in pixels. On a HiDPI window point size != pixel size, so scale
      // the CPC-image viewport by the per-axis density. Non-HiDPI windows have
      // sw==point width, making this a 1.0 no-op.
      int win_pt_w = 0, win_pt_h = 0;
      SDL_GetWindowSize(mainSDLWindow, &win_pt_w, &win_pt_h);
      const float vp_ds_x =
          win_pt_w > 0 ? static_cast<float>(sw) / win_pt_w : 1.0f;
      const float vp_ds_y =
          win_pt_h > 0 ? static_cast<float>(sh) / win_pt_h : 1.0f;
      vp.x = t->x_offset * vp_ds_x;
      vp.y = t->y_offset * vp_ds_y;
      vp.w = t->width * vp_ds_x;
      vp.h = t->height * vp_ds_y;
      vp.max_depth = 1.0f;
      SDL_SetGPUViewport(pass, &vp);

      SDL_BindGPUGraphicsPipeline(pass, g_crt_full_pipeline);
      SDL_GPUTextureSamplerBinding binding{};
      binding.texture = g_gpu.cpc_texture;
      binding.sampler = g_gpu.linear_sampler;
      SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

      CrtBasicUniforms uni{};  // same layout as CRT Basic
      uni.input_size[0] = static_cast<float>(vid->w);
      uni.input_size[1] =
          static_cast<float>(t->half_pixels ? vid->h : vid->h / 2);
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

  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
  }
}
}  // namespace

namespace {
void crt_full_gpu_close() {
  if (g_gpu.device) SDL_WaitForGPUIdle(g_gpu.device);
  destroy_crt_full_pipeline();
  gpu_direct_close();
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* "CRT Lottes (GPU)" plugin — SDL3 GPU port of crt_frag_lottes (Phase 6d)
 * ------------- */
/* ------------------------------------------------------------------------------------
 */
//
// Timothy Lottes' CRT shader (public domain) — Gaussian beam profile,
// sRGB-linear blending, curvature warp, slot mask.  Samples 11 input
// pixels per output pixel (3-5-3 horizontal kernel × 3 scanlines), so
// this is the heaviest of the three CRT tiers.  CPC texture is sampled
// with NEAREST filtering (matches the GL path — the Gaussian kernel
// does its own pixel-centre snapping via fetch()).
// Uniforms identical to Basic/Full: { float2 input_size; float2 output_size; }.

namespace {
SDL_GPUShader* g_crt_lottes_vertex_shader = nullptr;
}  // namespace
namespace {
SDL_GPUShader* g_crt_lottes_fragment_shader = nullptr;
}  // namespace
namespace {
SDL_GPUGraphicsPipeline* g_crt_lottes_pipeline = nullptr;
}  // namespace

namespace {
bool create_crt_lottes_pipeline() {
  if (!g_gpu.device) return false;
  const char* driver = SDL_GetGPUDeviceDriver(g_gpu.device);
  if (!driver) return false;

  SDL_GPUShaderCreateInfo vsi{};
  vsi.stage = SDL_GPU_SHADERSTAGE_VERTEX;

  SDL_GPUShaderCreateInfo fsi{};
  fsi.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fsi.num_samplers = 1;
  fsi.num_uniform_buffers = 1;

  if (std::strcmp(driver, "metal") == 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_MSL;
    vsi.code = reinterpret_cast<const Uint8*>(kCrtLottesMSLSource);
    vsi.code_size = std::strlen(kCrtLottesMSLSource);
    vsi.entrypoint = "vert_main";
    fsi.format = SDL_GPU_SHADERFORMAT_MSL;
    fsi.code = reinterpret_cast<const Uint8*>(kCrtLottesMSLSource);
    fsi.code_size = std::strlen(kCrtLottesMSLSource);
    fsi.entrypoint = "frag_main";
  } else if (std::strcmp(driver, "vulkan") == 0 && kBlitVertexSPIRVSize > 0 &&
             kCrtLottesFragmentSPIRVSize > 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vsi.code = kBlitVertexSPIRV;
    vsi.code_size = kBlitVertexSPIRVSize;
    vsi.entrypoint = "main";
    fsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fsi.code = kCrtLottesFragmentSPIRV;
    fsi.code_size = kCrtLottesFragmentSPIRVSize;
    fsi.entrypoint = "main";
  } else if (std::strcmp(driver, "direct3d12") == 0 &&
             kBlitVertexDXBCSize > 0 && kCrtLottesFragmentDXBCSize > 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_DXBC;
    vsi.code = kBlitVertexDXBC;
    vsi.code_size = kBlitVertexDXBCSize;
    vsi.entrypoint = "main";
    fsi.format = SDL_GPU_SHADERFORMAT_DXBC;
    fsi.code = kCrtLottesFragmentDXBC;
    fsi.code_size = kCrtLottesFragmentDXBCSize;
    fsi.entrypoint = "main";
  } else {
    return false;
  }

  g_crt_lottes_vertex_shader = SDL_CreateGPUShader(g_gpu.device, &vsi);
  g_crt_lottes_fragment_shader = SDL_CreateGPUShader(g_gpu.device, &fsi);
  if (!g_crt_lottes_vertex_shader || !g_crt_lottes_fragment_shader) {
    LOG_ERROR("CRT Lottes (GPU) shader create failed: " << SDL_GetError());
    if (g_crt_lottes_vertex_shader) {
      SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_vertex_shader);
      g_crt_lottes_vertex_shader = nullptr;
    }
    if (g_crt_lottes_fragment_shader) {
      SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_fragment_shader);
      g_crt_lottes_fragment_shader = nullptr;
    }
    return false;
  }

  SDL_GPUColorTargetDescription color_target{};
  color_target.format = g_gpu.swapchain_fmt;

  SDL_GPUGraphicsPipelineTargetInfo target_info{};
  target_info.num_color_targets = 1;
  target_info.color_target_descriptions = &color_target;
  target_info.has_depth_stencil_target = false;

  SDL_GPUGraphicsPipelineCreateInfo info{};
  info.vertex_shader = g_crt_lottes_vertex_shader;
  info.fragment_shader = g_crt_lottes_fragment_shader;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.target_info = target_info;

  g_crt_lottes_pipeline = SDL_CreateGPUGraphicsPipeline(g_gpu.device, &info);
  if (!g_crt_lottes_pipeline) {
    LOG_ERROR("CRT Lottes (GPU) pipeline create failed: " << SDL_GetError());
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_vertex_shader);
    g_crt_lottes_vertex_shader = nullptr;
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_fragment_shader);
    g_crt_lottes_fragment_shader = nullptr;
    return false;
  }
  return true;
}
}  // namespace

namespace {
void destroy_crt_lottes_pipeline() {
  if (!g_gpu.device) return;
  if (g_crt_lottes_pipeline) {
    SDL_ReleaseGPUGraphicsPipeline(g_gpu.device, g_crt_lottes_pipeline);
    g_crt_lottes_pipeline = nullptr;
  }
  if (g_crt_lottes_fragment_shader) {
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_fragment_shader);
    g_crt_lottes_fragment_shader = nullptr;
  }
  if (g_crt_lottes_vertex_shader) {
    SDL_ReleaseGPUShader(g_gpu.device, g_crt_lottes_vertex_shader);
    g_crt_lottes_vertex_shader = nullptr;
  }
}
}  // namespace

namespace {
SDL_Surface* crt_lottes_gpu_init(video_plugin* t, int scale, bool fs) {
  SDL_Surface* surf = gpu_direct_init(t, scale, fs);
  if (!surf) return nullptr;
  if (!create_crt_lottes_pipeline()) {
    gpu_direct_close();
    return nullptr;
  }
  LOG_INFO("CRT Lottes (GPU) plugin active");
  return surf;
}
}  // namespace

// CRT Lottes flip_a: same copy-pass + render flow as Basic/Full, differs
// in the bound pipeline and uses NEAREST sampling on cpc_texture because
// the Gaussian kernel re-quantises via floor()/fract() inside the shader.
namespace {
void crt_lottes_gpu_flip_a(video_plugin* t) {
  compute_scale(t, vid->w, vid->h);

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_gpu.device);
  if (!cmd) return;

  const uint32_t row_bytes = g_gpu.cpc_tex_w * 4;
  void* dst =
      SDL_MapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload, /*cycle=*/true);
  if (dst) {
    if (static_cast<uint32_t>(vid->pitch) == row_bytes) {
      std::memcpy(dst, vid->pixels, row_bytes * g_gpu.cpc_tex_h);
    } else {
      auto* d = static_cast<uint8_t*>(dst);
      auto* s = static_cast<const uint8_t*>(vid->pixels);
      for (uint32_t y = 0; y < g_gpu.cpc_tex_h; ++y)
        std::memcpy(d + (y * row_bytes), s + (y * vid->pitch), row_bytes);
    }
    SDL_UnmapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src_info{};
    src_info.transfer_buffer = g_gpu.cpc_upload;
    src_info.pixels_per_row = g_gpu.cpc_tex_w;
    src_info.rows_per_layer = g_gpu.cpc_tex_h;
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
  bool const have_swap =
      SDL_AcquireGPUSwapchainTexture(cmd, mainSDLWindow, &swap_tex, &sw, &sh) &&
      swap_tex != nullptr;

  if (have_swap) {
    SDL_GPUColorTargetInfo tgt{};
    tgt.texture = swap_tex;
    tgt.load_op = SDL_GPU_LOADOP_CLEAR;
    tgt.store_op = SDL_GPU_STOREOP_STORE;
    tgt.cycle = false;
    tgt.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);

    if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic) {
      SDL_GPUViewport vp{};
      // t->* come from compute_scale() in logical points; the swapchain (sw x
      // sh) is in pixels. On a HiDPI window point size != pixel size, so scale
      // the CPC-image viewport by the per-axis density. Non-HiDPI windows have
      // sw==point width, making this a 1.0 no-op.
      int win_pt_w = 0, win_pt_h = 0;
      SDL_GetWindowSize(mainSDLWindow, &win_pt_w, &win_pt_h);
      const float vp_ds_x =
          win_pt_w > 0 ? static_cast<float>(sw) / win_pt_w : 1.0f;
      const float vp_ds_y =
          win_pt_h > 0 ? static_cast<float>(sh) / win_pt_h : 1.0f;
      vp.x = t->x_offset * vp_ds_x;
      vp.y = t->y_offset * vp_ds_y;
      vp.w = t->width * vp_ds_x;
      vp.h = t->height * vp_ds_y;
      vp.max_depth = 1.0f;
      SDL_SetGPUViewport(pass, &vp);

      SDL_BindGPUGraphicsPipeline(pass, g_crt_lottes_pipeline);
      SDL_GPUTextureSamplerBinding binding{};
      binding.texture = g_gpu.cpc_texture;
      binding.sampler = g_gpu.nearest_sampler;  // Lottes uses NEAREST
      SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

      CrtBasicUniforms uni{};  // same layout as Basic/Full
      uni.input_size[0] = static_cast<float>(vid->w);
      uni.input_size[1] =
          static_cast<float>(t->half_pixels ? vid->h : vid->h / 2);
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

  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
  }
}
}  // namespace

namespace {
void crt_lottes_gpu_close() {
  if (g_gpu.device) SDL_WaitForGPUIdle(g_gpu.device);
  destroy_crt_lottes_pipeline();
  gpu_direct_close();
}
}  // namespace

// Previously: lightweight Direct ↔ CRT switch without re-creating the
// GL context / ImGui backend.  After Phase 7b the legacy GL CRT plugins
// are gone, so there's no pair of plugins this optimisation applies to
// anymore — every scr_style change on the legacy GL path goes through a
// full re-init.  Kept as a stub so kon_cpc_ja.cpp still links; returns
// false to force the full-reinit path.
bool video_try_lightweight_switch() { return false; }

/* ------------------------------------------------------------------------------------
 */
/* SDL_Renderer video plugin (D3D11 on Windows, no OpenGL required)
 * ------------------- */
/* ------------------------------------------------------------------------------------
 */
namespace {
void sdlr_close();
}  // namespace
namespace {
void sdlr_swscale_close();
}  // namespace

namespace {
SDL_Surface* sdlr_init(video_plugin* t, int scale, bool fs) {
  mainSDLWindow =
      SDL_CreateWindow("konCePCja " VERSION_STRING, CPC_RENDER_WIDTH * scale,
                       CPC_VISIBLE_SCR_HEIGHT * scale,
                       (fs ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_RESIZABLE);
  if (!mainSDLWindow) return nullptr;

  renderer = SDL_CreateRenderer(mainSDLWindow, nullptr);
  if (!renderer) {
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
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
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
    return nullptr;
  }
  if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
    return nullptr;
  }

  int const surface_width = CPC_RENDER_WIDTH;
  int const surface_height =
      (scale > 1) ? CPC_VISIBLE_SCR_HEIGHT * 2 : CPC_VISIBLE_SCR_HEIGHT;
  t->half_pixels = (scale <= 1) ? 1 : 0;
  vid =
      SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGBA32);
  if (!vid) {
    sdlr_close();
    return nullptr;
  }

  cpc_sdl_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      surface_width, surface_height);
  if (!cpc_sdl_texture) {
    sdlr_close();
    return nullptr;
  }
  SDL_SetTextureScaleMode(cpc_sdl_texture, SDL_SCALEMODE_NEAREST);

  {
    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(vid->format);
    SDL_Palette const* pal = SDL_GetSurfacePalette(vid);
    SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(fmt, pal, 0, 0, 0));
  }
  using_sdl_renderer = true;
  compute_scale(t, surface_width, surface_height);
  return vid;
}
}  // namespace

namespace {
void sdlr_flip(video_plugin* t) {
  // Recompute display area each frame (handles window resize, 4:3 aspect)
  compute_scale(t, vid->w, vid->h);

  // Update texture filtering: LINEAR for 4:3 stretch, NEAREST for square pixels
  SDL_SetTextureScaleMode(cpc_sdl_texture, CPC.scr_crt_aspect
                                               ? SDL_SCALEMODE_LINEAR
                                               : SDL_SCALEMODE_NEAREST);

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
        ImVec2(vp->Pos.x + t->x_offset + t->width,
               vp->Pos.y + t->y_offset + t->height));
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
}  // namespace

namespace {
void sdlr_close() {
  if (ImGui::GetCurrentContext()) {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
  if (cpc_sdl_texture) {
    SDL_DestroyTexture(cpc_sdl_texture);
    cpc_sdl_texture = nullptr;
  }
  if (vid) {
    SDL_DestroySurface(vid);
    vid = nullptr;
  }
  if (renderer) {
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
  }
  if (mainSDLWindow) {
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
  }
  using_sdl_renderer = false;
}
}  // namespace

/* SDL_Renderer swscale plugin
 * -------------------------------------------------------- */
namespace {
SDL_Surface* sdlr_swscale_init(video_plugin* t, int scale, bool fs) {
  mainSDLWindow =
      SDL_CreateWindow("konCePCja " VERSION_STRING, CPC_RENDER_WIDTH * scale,
                       CPC_VISIBLE_SCR_HEIGHT * scale,
                       (fs ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_RESIZABLE);
  if (!mainSDLWindow) return nullptr;

  renderer = SDL_CreateRenderer(mainSDLWindow, nullptr);
  if (!renderer) {
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
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
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
    return nullptr;
  }
  if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
    return nullptr;
  }

  // Software scaling plugins: render at native width, filter produces 2×
  // output.
  int const surface_width = CPC_RENDER_WIDTH;
  int const surface_height =
      (scale > 1) ? CPC_VISIBLE_SCR_HEIGHT * 2 : CPC_VISIBLE_SCR_HEIGHT;
  t->half_pixels = (scale <= 1) ? 1 : 0;
  vid = SDL_CreateSurface(surface_width * 2, surface_height * 2,
                          SDL_PIXELFORMAT_RGBA32);
  if (!vid) {
    sdlr_close();
    return nullptr;
  }

  cpc_sdl_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      surface_width * 2, surface_height * 2);
  if (!cpc_sdl_texture) {
    sdlr_close();
    return nullptr;
  }
  SDL_SetTextureScaleMode(cpc_sdl_texture, SDL_SCALEMODE_NEAREST);

  scaled = SDL_CreateSurface(surface_width * 2, surface_height * 2,
                             SDL_PIXELFORMAT_RGB565);
  if (!scaled) {
    sdlr_swscale_close();
    return nullptr;
  }
  {
    const SDL_PixelFormatDetails* s_fmt =
        SDL_GetPixelFormatDetails(scaled->format);
    if (!s_fmt || s_fmt->bits_per_pixel != 16) {
      LOG_ERROR(t->name << ": SDL didn't return a 16 bpp surface but a "
                        << static_cast<int>(s_fmt ? s_fmt->bits_per_pixel : 0)
                        << " bpp one.");
      sdlr_swscale_close();
      return nullptr;
    }
  }
  {
    const SDL_PixelFormatDetails* v_fmt =
        SDL_GetPixelFormatDetails(vid->format);
    SDL_Palette const* v_pal = SDL_GetSurfacePalette(vid);
    if (v_fmt)
      SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(v_fmt, v_pal, 0, 0, 0));
  }
  compute_scale(t, surface_width, surface_height);
  pub =
      SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGB565);
  if (!pub) {
    sdlr_swscale_close();
    return nullptr;
  }
  {
    const SDL_PixelFormatDetails* p_fmt =
        SDL_GetPixelFormatDetails(pub->format);
    if (!p_fmt || p_fmt->bits_per_pixel != 16) {
      LOG_ERROR(t->name << ": SDL didn't return a 16 bpp surface but a "
                        << static_cast<int>(p_fmt ? p_fmt->bits_per_pixel : 0)
                        << " bpp one.");
      sdlr_swscale_close();
      return nullptr;
    }
  }
  using_sdl_renderer = true;
  return pub;
}
}  // namespace

namespace {
void sdlr_swscale_blit(video_plugin* t) {
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
        ImVec2(vp->Pos.x + t->x_offset + t->width,
               vp->Pos.y + t->y_offset + t->height));
  }

  imgui_render_ui();
  ImGui::Render();

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

  video_capture_if_pending();

  SDL_RenderPresent(renderer);
}
}  // namespace

namespace {
void sdlr_swscale_close() {
  sdlr_close();
  if (scaled) {
    SDL_DestroySurface(scaled);
    scaled = nullptr;
  }
  if (pub) {
    SDL_DestroySurface(pub);
    pub = nullptr;
  }
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* Headless video plugin (no window, offscreen surface only)
 * -------------------------- */
/* ------------------------------------------------------------------------------------
 */
namespace {
SDL_Surface* headless_init(video_plugin* t, int /*scale*/, bool /*fs*/) {
  t->half_pixels = 1;  // dwYScale=1 for headless
  int const surface_width = CPC_RENDER_WIDTH;
  int const surface_height = CPC_VISIBLE_SCR_HEIGHT;
  vid =
      SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGBA32);
  if (!vid) return nullptr;
  {
    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(vid->format);
    SDL_Palette const* pal = SDL_GetSurfacePalette(vid);
    SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(fmt, pal, 0, 0, 0));
  }
  return vid;
}
}  // namespace

namespace {
void headless_setpal(SDL_Color* /*c*/) {
  // palette stored in CPC colours array; no GPU upload needed
}
}  // namespace

namespace {
void headless_flip(video_plugin* /*t*/) {
  // no-op: nothing to present in headless mode
}
}  // namespace

namespace {
void headless_close() {
  if (vid) {
    SDL_DestroySurface(vid);
    vid = nullptr;
  }
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* Common 2x software scaling code
 * ---------------------------------------------------- */
/* ------------------------------------------------------------------------------------
 */

/* Computes the clipping of pub and scaled surfaces and put the result in src
 * and dst accordingly.
 *
 * This provides the rectangles to clip to obtain a centered doubled CPC display
 * in the middle of the dst surface if it fits
 *
 * dst is the screen
 * src is the internal window
 *
 * Only exposed for testing purposes. Shouldn't be used outside of video.cpp
 */
namespace {
// NOLINTNEXTLINE(readability-non-const-parameter): pointer written through a
// cast or passed to a non-const callee
void compute_rects(SDL_Rect* src, SDL_Rect* dst, Uint8 half_pixels) {
  // Software scaling filter output is 2× the render surface
  int const surface_width = CPC_RENDER_WIDTH * 2;
  int const surface_height =
      half_pixels ? CPC_VISIBLE_SCR_HEIGHT * 2 : CPC_VISIBLE_SCR_HEIGHT * 4;
  /* initialise the source rect to full source */
  src->x = 0;
  src->y = 0;
  src->w = pub->w;
  src->h = pub->h;

  dst->x = (scaled->w - surface_width) / 2,
  dst->y = (scaled->h - surface_height) / 2;
  dst->w = scaled->w;
  dst->h = scaled->h;

  int dw = (src->w * 2) - dst->w;
  /* the src width is too big */
  if (dw > 0) {
    // To ensure src is not bigger than dst for odd widths.
    dw += 1;
    src->w -= dw / 2;
    src->x += dw / 4;

    dst->x = 0;
    dst->w = scaled->w;
  } else {
    dst->w = surface_width;
  }
  int dh = (src->h * 2) - dst->h;
  /* the src height is too big */
  if (dh > 0) {
    // To ensure src is not bigger than dst for odd heights.
    dh += 1;
    src->h -= dh / 2;
    src->y += dh / 4;

    dst->y = 0;
    dst->h = scaled->h;
  } else {
    // Without this -=, the bottom of the screen has line with random pixels.
    // With this, they are black instead which is slightly better.
    // Investigating where this comes from and how to avoid it would be nice!
    src->h -= 2 * 2;
    dst->h = surface_height;
  }
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
void compute_rects_for_tests(SDL_Rect* src, SDL_Rect* dst, Uint8 half_pixels) {
  compute_rects(src, dst, half_pixels);
}

// Scale factor table shared with compute_scale and Options combo.
static const float video_scale_factors[] = {0.f, 1.f, 1.5f, 2.f, 3.f};
static const int video_scale_factors_count =
    sizeof(video_scale_factors) / sizeof(video_scale_factors[0]);

// Compute window dimensions for the current scale + bars + 4:3 aspect.
// For Fit mode (scr_scale=0), returns false (don't resize — keep user's
// window).
namespace {
bool compute_window_size(int& out_w, int& out_h) {
  float f;
  if (CPC.scr_scale > 0 &&
      static_cast<int>(CPC.scr_scale) < video_scale_factors_count)
    f = video_scale_factors[CPC.scr_scale];
  else
    return false;  // Fit mode — don't resize
  out_w = static_cast<int>(CPC_RENDER_WIDTH * f) + devtools_panel_width;
  int const cpc_h = CPC.scr_crt_aspect
                        ? static_cast<int>(CPC_RENDER_WIDTH * f * 3.f / 4.f)
                        : static_cast<int>(CPC_VISIBLE_SCR_HEIGHT * f);
  out_h = max(cpc_h + topbar_height + bottombar_height, devtools_panel_height);
  return true;
}
}  // namespace

// The docked-devtools side panel setters/getters that used to live here died
// with the legacy renderer integration — the ImGui viewport windows own that
// surface now. The devtools_panel_* variables stay: compute_window_size still
// folds them into the main-window geometry.

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
void video_set_topbar(SDL_Surface* surface, int height) {
  if (!mainSDLWindow) return;
  topbar_surface = surface;
  topbar_height = height;
  int w, h;
  if (compute_window_size(w, h)) SDL_SetWindowSize(mainSDLWindow, w, h);
  if (vid_plugin && vid) compute_scale(vid_plugin, vid->w, vid->h);
}

void video_clear_topbar() {
  topbar_surface = nullptr;
  topbar_height = 0;
  if (mainSDLWindow) {
    int w, h;
    if (compute_window_size(w, h)) SDL_SetWindowSize(mainSDLWindow, w, h);
  }
  if (vid_plugin && vid) compute_scale(vid_plugin, vid->w, vid->h);
}

int video_get_topbar_height() { return topbar_height; }

void video_set_bottombar(int height) {
  if (!mainSDLWindow) return;
  bottombar_height = height;
  int w, h;
  if (compute_window_size(w, h)) SDL_SetWindowSize(mainSDLWindow, w, h);
  if (vid_plugin && vid) compute_scale(vid_plugin, vid->w, vid->h);
}

int video_get_bottombar_height() { return bottombar_height; }

// Phase A: common software-scaler blit — only SDL_Renderer plugins reach
// this entry point now (the GL swscale plugins were deleted in Phase 7c.1b;
// the GPU swscale plugins use seagle_gpu_flip / scale2x_gpu_flip / etc.
// which call gpu_flip_a directly).
namespace {
void swscale_blit_a(video_plugin* t) {
  if (using_sdl_renderer) {
    sdlr_swscale_blit(t);
  }
  // No else branch — the GL upload + ImGui render path was deleted in
  // Phase 7c.1b along with the corresponding GL plugins.
}
}  // namespace

namespace {
void swscale_setpal(SDL_Color* c) {
  if (SDL_Palette* pal = SDL_GetSurfacePalette(scaled)) {
    SDL_SetPaletteColors(pal, c, 0, 32);
  }
  if (SDL_Palette* pal = SDL_GetSurfacePalette(pub)) {
    SDL_SetPaletteColors(pal, c, 0, 32);
  }
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* Super eagle video plugin
 * ----------------------------------------------------------- */
/* ------------------------------------------------------------------------------------
 */

/* 2X SAI Filter */
namespace {
void seagle_flip(video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_supereagle(static_cast<Uint8*>(pub->pixels) +
                        ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                    pub->pitch,
                    static_cast<Uint8*>(scaled->pixels) +
                        ((2 * dst.x) + (dst.y * scaled->pitch)),
                    scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* Scale2x video plugin
 * --------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------
 */
namespace {
void scale2x_flip([[maybe_unused]] video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_scale2x(static_cast<Uint8*>(pub->pixels) +
                     ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                 pub->pitch,
                 static_cast<Uint8*>(scaled->pixels) +
                     ((2 * dst.x) + (dst.y * scaled->pitch)),
                 scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* ascale2x video plugin
 * --------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------
 */
namespace {
void ascale2x_flip([[maybe_unused]] video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_ascale2x(static_cast<Uint8*>(pub->pixels) +
                      ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                  pub->pitch,
                  static_cast<Uint8*>(scaled->pixels) +
                      ((2 * dst.x) + (dst.y * scaled->pitch)),
                  scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* tv2x video plugin
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------
 */
namespace {
void tv2x_flip([[maybe_unused]] video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_tv2x(static_cast<Uint8*>(pub->pixels) +
                  ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
              pub->pitch,
              static_cast<Uint8*>(scaled->pixels) +
                  ((2 * dst.x) + (dst.y * scaled->pitch)),
              scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* Software bilinear video plugin
 * ----------------------------------------------------- */
/* ------------------------------------------------------------------------------------
 */
namespace {
void swbilin_flip([[maybe_unused]] video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_bilinear(static_cast<Uint8*>(pub->pixels) +
                      ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                  pub->pitch,
                  static_cast<Uint8*>(scaled->pixels) +
                      ((2 * dst.x) + (dst.y * scaled->pitch)),
                  scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* Software bicubic video plugin
 * ------------------------------------------------------ */
/* ------------------------------------------------------------------------------------
 */
namespace {
void swbicub_flip([[maybe_unused]] video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_bicubic(static_cast<Uint8*>(pub->pixels) +
                     ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                 pub->pitch,
                 static_cast<Uint8*>(scaled->pixels) +
                     ((2 * dst.x) + (dst.y * scaled->pitch)),
                 scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* Dot matrix video plugin
 * ------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------
 */
namespace {
void dotmat_flip([[maybe_unused]] video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_dotmatrix(static_cast<Uint8*>(pub->pixels) +
                       ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                   pub->pitch,
                   static_cast<Uint8*>(scaled->pixels) +
                       ((2 * dst.x) + (dst.y * scaled->pitch)),
                   scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_blit_a(t);
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* GPU variants of the swscale family (P1.2b Phase 5)
 * --------------------------------- */
/* ------------------------------------------------------------------------------------
 */
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

namespace {
SDL_Surface* swscale_gpu_init(video_plugin* t, int scale, bool fs) {
  mainSDLWindow =
      SDL_CreateWindow("konCePCja " VERSION_STRING, CPC_RENDER_WIDTH * scale,
                       CPC_VISIBLE_SCR_HEIGHT * scale,
                       (fs ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_RESIZABLE);
  if (!mainSDLWindow) return nullptr;

  const int surface_width = CPC_RENDER_WIDTH;
  const int surface_height =
      (scale > 1) ? CPC_VISIBLE_SCR_HEIGHT * 2 : CPC_VISIBLE_SCR_HEIGHT;
  t->half_pixels = (scale <= 1) ? 1 : 0;

  // swscale surfaces are 2x the base CPC size — the CPU filter upscales.
  const uint32_t gpu_tex_w = static_cast<uint32_t>(surface_width * 2);
  const uint32_t gpu_tex_h = static_cast<uint32_t>(surface_height * 2);

  if (!video_gpu_init(mainSDLWindow, gpu_tex_w, gpu_tex_h) ||
      g_gpu.blit_pipeline == nullptr) {
    video_gpu_shutdown();
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
    return nullptr;
  }

  // video.vsync escape hatch (main window only; this GPU path has viewports
  // disabled, but the call is harmless and keeps both GPU inits consistent).
  video_gpu_set_main_present_mode(CPC.scr_vsync != 0);

  // ImGui SDLGPU3 backend — viewports disabled (see Phase 4 rationale).
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = imgui_ini_path();
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
  ImGui::StyleColorsDark();
  imgui_init_ui();
  ImGui_ImplSDL3_InitForSDLGPU(mainSDLWindow);
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
    video_gpu_shutdown();
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
    return nullptr;
  }

  // Same three surfaces as swscale_init:
  //   vid    — RGBA32, 2x — the CPU-side upload buffer the GPU reads from
  //   scaled — RGB565, 2x — what the CPU filter writes into
  //   pub    — RGB565, 1x — the surface returned to the CPC (half size)
  vid = SDL_CreateSurface(gpu_tex_w, gpu_tex_h, SDL_PIXELFORMAT_RGBA32);
  scaled = SDL_CreateSurface(gpu_tex_w, gpu_tex_h, SDL_PIXELFORMAT_RGB565);
  pub =
      SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_RGB565);

  if (!vid || !scaled || !pub) {
    if (pub) {
      SDL_DestroySurface(pub);
      pub = nullptr;
    }
    if (scaled) {
      SDL_DestroySurface(scaled);
      scaled = nullptr;
    }
    if (vid) {
      SDL_DestroySurface(vid);
      vid = nullptr;
    }
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
    const SDL_PixelFormatDetails* s_fmt =
        SDL_GetPixelFormatDetails(scaled->format);
    const SDL_PixelFormatDetails* p_fmt =
        SDL_GetPixelFormatDetails(pub->format);
    if (!s_fmt || s_fmt->bits_per_pixel != 16 || !p_fmt ||
        p_fmt->bits_per_pixel != 16) {
      LOG_ERROR(t->name << ": SDL didn't return 16 bpp surfaces");
      SDL_DestroySurface(pub);
      pub = nullptr;
      SDL_DestroySurface(scaled);
      scaled = nullptr;
      SDL_DestroySurface(vid);
      vid = nullptr;
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
    const SDL_PixelFormatDetails* v_fmt =
        SDL_GetPixelFormatDetails(vid->format);
    SDL_Palette const* v_pal = SDL_GetSurfacePalette(vid);
    SDL_FillSurfaceRect(vid, nullptr, SDL_MapRGB(v_fmt, v_pal, 0, 0, 0));
  }
  compute_scale(t, surface_width, surface_height);
  LOG_INFO(t->name << ": GPU swscale plugin active");
  return pub;  // swscale plugins hand the CPC the half-sized `pub` surface
}
}  // namespace

// GPU variant of swscale_blit_a: convert scaled(16bpp) → vid(RGBA32) via
// SDL_BlitSurface, then reuse gpu_flip_a (Phase 4) to upload + render +
// submit.  gpu_flip_a uses g_gpu.cpc_tex_{w,h} which match the 2x surface
// dims we gave video_gpu_init above.
namespace {
void swscale_gpu_blit_a(video_plugin* t) {
  SDL_BlitSurface(scaled, nullptr, vid, nullptr);
  gpu_flip_a(t);
}
}  // namespace

namespace {
void swscale_gpu_close() {
  if (g_gpu.device) SDL_WaitForGPUIdle(g_gpu.device);

  if (ImGui::GetCurrentContext()) {
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
  if (scaled) {
    SDL_DestroySurface(scaled);
    scaled = nullptr;
  }
  if (pub) {
    SDL_DestroySurface(pub);
    pub = nullptr;
  }
  if (vid) {
    SDL_DestroySurface(vid);
    vid = nullptr;
  }
  video_gpu_shutdown();
  if (mainSDLWindow) {
    SDL_DestroyWindow(mainSDLWindow);
    mainSDLWindow = nullptr;
  }
}
}  // namespace

// Per-filter Phase A functions — each runs its CPU filter into `scaled`,
// then hands off to swscale_gpu_blit_a.  The filter setup is copied
// verbatim from the matching GL flip to keep the additive pattern
// (the existing GL flip functions are untouched).

namespace {
void seagle_gpu_flip(video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src, dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_supereagle(static_cast<Uint8*>(pub->pixels) +
                        ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                    pub->pitch,
                    static_cast<Uint8*>(scaled->pixels) +
                        ((2 * dst.x) + (dst.y * scaled->pitch)),
                    scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_gpu_blit_a(t);
}
}  // namespace

namespace {
void scale2x_gpu_flip(video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src, dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_scale2x(static_cast<Uint8*>(pub->pixels) +
                     ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                 pub->pitch,
                 static_cast<Uint8*>(scaled->pixels) +
                     ((2 * dst.x) + (dst.y * scaled->pitch)),
                 scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_gpu_blit_a(t);
}
}  // namespace

namespace {
void ascale2x_gpu_flip(video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src, dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_ascale2x(static_cast<Uint8*>(pub->pixels) +
                      ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                  pub->pitch,
                  static_cast<Uint8*>(scaled->pixels) +
                      ((2 * dst.x) + (dst.y * scaled->pitch)),
                  scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_gpu_blit_a(t);
}
}  // namespace

namespace {
void tv2x_gpu_flip(video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src, dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_tv2x(static_cast<Uint8*>(pub->pixels) +
                  ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
              pub->pitch,
              static_cast<Uint8*>(scaled->pixels) +
                  ((2 * dst.x) + (dst.y * scaled->pitch)),
              scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_gpu_blit_a(t);
}
}  // namespace

namespace {
void swbilin_gpu_flip(video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src, dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_bilinear(static_cast<Uint8*>(pub->pixels) +
                      ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                  pub->pitch,
                  static_cast<Uint8*>(scaled->pixels) +
                      ((2 * dst.x) + (dst.y * scaled->pitch)),
                  scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_gpu_blit_a(t);
}
}  // namespace

namespace {
void swbicub_gpu_flip(video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src, dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_bicubic(static_cast<Uint8*>(pub->pixels) +
                     ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                 pub->pitch,
                 static_cast<Uint8*>(scaled->pixels) +
                     ((2 * dst.x) + (dst.y * scaled->pitch)),
                 scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_gpu_blit_a(t);
}
}  // namespace

namespace {
void dotmat_gpu_flip(video_plugin* t) {
  if (SDL_MUSTLOCK(scaled)) SDL_LockSurface(scaled);
  SDL_Rect src, dst;
  compute_rects(&src, &dst, t->half_pixels);
  filter_dotmatrix(static_cast<Uint8*>(pub->pixels) +
                       ((2 * src.x) + (src.y * pub->pitch)) + (pub->pitch),
                   pub->pitch,
                   static_cast<Uint8*>(scaled->pixels) +
                       ((2 * dst.x) + (dst.y * scaled->pitch)),
                   scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled)) SDL_UnlockSurface(scaled);
  swscale_gpu_blit_a(t);
}
}  // namespace

/* ------------------------------------------------------------------------------------
 */
/* End of video plugins
 * --------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------
 */

video_plugin video_headless_plugin() {
  return {"Headless",
          true,
          headless_init,
          headless_setpal,
          headless_flip,
          headless_close,
          1,
          0,
          0,
          0,
          0,
          0,
          0,
          nullptr};
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
std::vector<video_plugin> video_plugin_list = {
    // Phase 7c.1b: GL plugins deleted.  The "Direct" / swscale family entries
    // below now point at the SDL3 GPU implementations (formerly named "Direct
    // (GPU)" / "Super eagle (GPU)" / etc.).  Names kept short so existing UI
    // labels and external references (scripts, configs) continue to work.
    // Hardware flip variants are the same as software ones since switch to
    // SDL2.
    /* Name                     Hidden Init func          Palette func     Flip
       func (phase A)    Close func           Half size  X, Y offsets   X, Y
       scale  width, height  Flip B (phase B: viewports+swap) */
    {"Direct", false, gpu_direct_init, direct_setpal, gpu_flip_a,
     gpu_direct_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"Direct double", true, gpu_direct_init, direct_setpal, gpu_flip_a,
     gpu_direct_close, 0, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"Half size", true, gpu_direct_init, direct_setpal, gpu_flip_a,
     gpu_direct_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"Double size", true, gpu_direct_init, direct_setpal, gpu_flip_a,
     gpu_direct_close, 0, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"Super eagle", false, swscale_gpu_init, swscale_setpal, seagle_gpu_flip,
     swscale_gpu_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"Scale2x", false, swscale_gpu_init, swscale_setpal, scale2x_gpu_flip,
     swscale_gpu_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"Advanced Scale2x", false, swscale_gpu_init, swscale_setpal,
     ascale2x_gpu_flip, swscale_gpu_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"TV 2x", false, swscale_gpu_init, swscale_setpal, tv2x_gpu_flip,
     swscale_gpu_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"Software bilinear", false, swscale_gpu_init, swscale_setpal,
     swbilin_gpu_flip, swscale_gpu_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"Software bicubic", false, swscale_gpu_init, swscale_setpal,
     swbicub_gpu_flip, swscale_gpu_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"Dot matrix", false, swscale_gpu_init, swscale_setpal, dotmat_gpu_flip,
     swscale_gpu_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    /* SDL_Renderer plugins — use D3D11 on Windows, Metal on macOS, GL on Linux.
       No OpenGL context required; no multi-viewport support. flip_b is null. */
    {"Direct (SDL)", false, sdlr_init, direct_setpal, sdlr_flip, sdlr_close, 1,
     0, 0, 0, 0, 0, 0, nullptr},
    {"Super eagle (SDL)", false, sdlr_swscale_init, swscale_setpal, seagle_flip,
     sdlr_swscale_close, 1, 0, 0, 0, 0, 0, 0, nullptr},
    {"Scale2x (SDL)", false, sdlr_swscale_init, swscale_setpal, scale2x_flip,
     sdlr_swscale_close, 1, 0, 0, 0, 0, 0, 0, nullptr},
    {"TV 2x (SDL)", false, sdlr_swscale_init, swscale_setpal, tv2x_flip,
     sdlr_swscale_close, 1, 0, 0, 0, 0, 0, 0, nullptr},
    {"Bilinear (SDL)", false, sdlr_swscale_init, swscale_setpal, swbilin_flip,
     sdlr_swscale_close, 1, 0, 0, 0, 0, 0, 0, nullptr},
    {"Bicubic (SDL)", false, sdlr_swscale_init, swscale_setpal, swbicub_flip,
     sdlr_swscale_close, 1, 0, 0, 0, 0, 0, 0, nullptr},
    /* CRT (GPU) — SDL3 GPU CRT shader plugins.  Metal + Vulkan + D3D12
       backends. */
    {"CRT Basic", false, crt_basic_gpu_init, direct_setpal,
     crt_basic_gpu_flip_a, crt_basic_gpu_close, 1, 0, 0, 0, 0, 0, 0,
     gpu_flip_b},
    {"CRT Full", false, crt_full_gpu_init, direct_setpal, crt_full_gpu_flip_a,
     crt_full_gpu_close, 1, 0, 0, 0, 0, 0, 0, gpu_flip_b},
    {"CRT Lottes", false, crt_lottes_gpu_init, direct_setpal,
     crt_lottes_gpu_flip_a, crt_lottes_gpu_close, 1, 0, 0, 0, 0, 0, 0,
     gpu_flip_b},
    // SDL_Renderer fallbacks for the two styles the section above forgot —
    // their flip kernels existed unreferenced (GCC -Werror=unused-function
    // caught the gap). Appended at the END: scr_style indices are positional,
    // inserting mid-table would shift every later style in existing configs.
    {"Advanced Scale2x (SDL)", false, sdlr_swscale_init, swscale_setpal,
     ascale2x_flip, sdlr_swscale_close, 1, 0, 0, 0, 0, 0, 0, nullptr},
    {"Dot matrix (SDL)", false, sdlr_swscale_init, swscale_setpal, dotmat_flip,
     sdlr_swscale_close, 1, 0, 0, 0, 0, 0, 0, nullptr},
};
