/* video_headless — the presentation layer of the SDL-free build
 * (KONCPC_MODERN_UI=0, beads-29zx): one plugin, the headless offscreen
 * surface, and the IPC repaint state. The GUI build gets all of this, and its
 * windowed plugins, from src/video_host.cpp; this file compiles to nothing
 * there.
 *
 * The surface matches the GUI build's headless plugin byte for byte:
 * CPC_RENDER_WIDTH x CPC_VISIBLE_SCR_HEIGHT, RGBA32, cleared to opaque black,
 * half_pixels=1. `hash vram` and screenshots of the same frame therefore agree
 * between `koncepcja -H` built with and without SDL. */

#ifndef KONCPC_SDL

#include "koncepcja.h"
#include "video_plugin.h"

std::atomic<bool> g_repaint_pending{false};
std::atomic<bool> g_repaint_done{false};
std::mutex g_repaint_mutex;
std::string g_repaint_screenshot_path;
std::string g_repaint_error;

namespace {
HostSurface* headless_surface = nullptr;

HostSurface* headless_init(video_plugin* t, int /*scale*/, bool /*fs*/) {
  t->half_pixels = 1;  // dwYScale=1 for headless
  headless_surface =
      host_surface_create_rgba32(CPC_RENDER_WIDTH, CPC_VISIBLE_SCR_HEIGHT);
  return headless_surface;
}

void headless_setpal(HostColor* /*c*/) {
  // palette stored in CPC colours array; nothing to upload
}

void headless_flip(video_plugin* /*t*/) {
  // no-op: nothing to present in headless mode
}

void headless_close() {
  host_surface_destroy(headless_surface);
  headless_surface = nullptr;
}
}  // namespace

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

std::vector<video_plugin> video_plugin_list = {video_headless_plugin()};

#endif  // !KONCPC_SDL
