#pragma once

/* video_plugin — the presentation-backend vtable and registry, split out of
 * video_host.h so the SDL-free build (KONCPC_MODERN_UI=0, beads-29zx) can use
 * them without the SDL host. That build has a single entry, the headless
 * plugin (src/video_headless.cpp); the GUI build's registry lives in
 * src/video_host.cpp. */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "host_surface.h"

typedef struct video_plugin {
  /* the user-displayed name of this plugin */
  const char* name;
  /* whether the plugin should be hidden from UI (i.e. is deprecated) */
  bool hidden;
  /* initializes the video plugin ; returns the surface that you must draw into,
   * nullptr in the (unlikely ;) event of a failure */
  HostSurface* (*init)(video_plugin* t, int scale, bool fs);

  void (*set_palette)(HostColor* c);
  /* "flips" the video surface. Note that this might not always do a real flip
   */
  void (*flip)(video_plugin* t);
  /* closes the plugin */
  void (*close)();

  /* this plugin wants : 0 half sized pixels (320x200 screen)/1 full sized
   * pixels (640x200 screen)*/
  uint8_t half_pixels;

  /* mouse offset/scaling info */
  int x_offset, y_offset;
  float x_scale, y_scale;
  /* width & height of the surface to display */
  int width, height;

  /* Second phase of flip: renders floating ImGui viewports and swaps the
     window. Runs after audio push so the 30-60ms stall doesn't starve the audio
     queue. Null for SDL_Renderer, headless, and non-ImGui GL plugins. */
  void (*flip_b)(video_plugin* t);
} video_plugin;

extern std::vector<video_plugin> video_plugin_list;

video_plugin video_headless_plugin();

// IPC "repaint": the IPC thread sets the request, the main loop re-renders
// the frame from RAM (optionally writing a screenshot) and reports back.
extern std::atomic<bool> g_repaint_pending;
extern std::atomic<bool> g_repaint_done;
extern std::mutex g_repaint_mutex;
extern std::string g_repaint_screenshot_path;
extern std::string g_repaint_error;
