// DRM/KMS direct presenter — see drm_present.h for why this exists.
//
// The display pipeline used here was verified on a LilyGO T-Display K230:
//   connector DSI-1, single mode 568x1232, driver canaan-drm, no render node.
// The primary plane's `rotation` property is rotate-270 (value 8 = 1<<3), so
// the display engine rotates for us and the CRTC expects a LANDSCAPE source.
// The framebuffer is therefore allocated 1232x568, which also makes the blit
// write whole rows sequentially instead of striding down columns.
//
// Measured on that hardware: blit 2.26 ms/frame, page flips vsync-bound at
// 52 FPS, so presentation costs ~11% of a 20 ms CPC frame.

#include "drm_present.h"

#include <cstdlib>
#include <string>
#include <vector>
#include <cstring>

#if defined(__linux__) && defined(KONCPC_HAVE_LIBDRM)

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <SDL3/SDL.h>

#include "log.h"

namespace {

struct DumbFb {
  uint32_t handle = 0, pitch = 0, fb_id = 0;
  uint64_t size = 0;
  uint8_t* map = nullptr;
};

struct DrmState {
  int fd = -1;
  uint32_t crtc_id = 0, conn_id = 0;
  drmModeModeInfo mode{};
  int fb_w = 0, fb_h = 0;   // landscape: mode.vdisplay x mode.hdisplay
  DumbFb fb[2];
  int front = 0;
  bool ready = false;
  bool failed = false;
  std::vector<int> xmap;    // destination column -> source column
  int xmap_src_w = -1, xmap_dst_w = -1;
};

DrmState g;

bool make_fb(int fd, uint32_t w, uint32_t h, DumbFb& f) {
  drm_mode_create_dumb creq{};
  creq.width = w; creq.height = h; creq.bpp = 32;
  if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) return false;
  f.handle = creq.handle; f.pitch = creq.pitch; f.size = creq.size;
  if (drmModeAddFB(fd, w, h, 24, 32, f.pitch, f.handle, &f.fb_id)) return false;
  drm_mode_map_dumb mreq{};
  mreq.handle = f.handle;
  if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) return false;
  f.map = static_cast<uint8_t*>(
      mmap(nullptr, f.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset));
  if (f.map == MAP_FAILED) { f.map = nullptr; return false; }
  std::memset(f.map, 0, f.size);
  return true;
}

bool init() {
  g.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (g.fd < 0) { LOG_ERROR("drm_present: cannot open /dev/dri/card0"); return false; }

  drmModeRes* res = drmModeGetResources(g.fd);
  if (!res) { LOG_ERROR("drm_present: drmModeGetResources failed"); return false; }

  drmModeConnector* conn = nullptr;
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector* c = drmModeGetConnector(g.fd, res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
    if (c) drmModeFreeConnector(c);
  }
  if (!conn) { drmModeFreeResources(res); LOG_ERROR("drm_present: no connected connector"); return false; }

  g.mode = conn->modes[0];
  g.conn_id = conn->connector_id;
  drmModeEncoder* enc =
      drmModeGetEncoder(g.fd, conn->encoder_id ? conn->encoder_id : conn->encoders[0]);
  g.crtc_id = (enc && enc->crtc_id) ? enc->crtc_id : res->crtcs[0];
  if (enc) drmModeFreeEncoder(enc);

  // Landscape source: the primary plane rotates in hardware.
  g.fb_w = g.mode.vdisplay;
  g.fb_h = g.mode.hdisplay;

  bool ok = make_fb(g.fd, g.fb_w, g.fb_h, g.fb[0]) && make_fb(g.fd, g.fb_w, g.fb_h, g.fb[1]);
  drmModeFreeConnector(conn);
  drmModeFreeResources(res);
  if (!ok) { LOG_ERROR("drm_present: dumb buffer allocation failed"); return false; }

  drmSetMaster(g.fd);  // best effort; fails harmlessly if already master
  if (drmModeSetCrtc(g.fd, g.crtc_id, g.fb[0].fb_id, 0, 0, &g.conn_id, 1, &g.mode)) {
    LOG_ERROR("drm_present: drmModeSetCrtc failed — is another compositor holding the display?");
    return false;
  }

  LOG_INFO("drm_present: " + std::to_string(g.fb_w) + "x" + std::to_string(g.fb_h) +
           " landscape on connector " + std::to_string(g.conn_id) +
           " (panel " + std::to_string(g.mode.hdisplay) + "x" +
           std::to_string(g.mode.vdisplay) + "@" + std::to_string(g.mode.vrefresh) + ")");
  return true;
}

volatile int flip_pending = 0;
void on_flip(int, unsigned, unsigned, unsigned, void*) { flip_pending = 0; }

void page_flip(uint32_t fb_id) {
  if (drmModePageFlip(g.fd, g.crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, nullptr)) return;
  flip_pending = 1;
  drmEventContext ev{};
  ev.version = 2;
  ev.page_flip_handler = on_flip;
  while (flip_pending) {
    if (drmHandleEvent(g.fd, &ev)) break;
  }
}

}  // namespace

bool drm_present_enabled() {
  const char* v = std::getenv("KONCPC_DRM");
  return v && v[0] == '1';
}

void drm_present_frame(SDL_Renderer* renderer) {
  if (!drm_present_enabled() || g.failed || !renderer) return;
  if (!g.ready) {
    if (!init()) { g.failed = true; return; }
    g.ready = true;
  }

  SDL_Surface* src = SDL_RenderReadPixels(renderer, nullptr);
  if (!src) return;
  SDL_Surface* rgb = (src->format == SDL_PIXELFORMAT_XRGB8888)
                         ? src
                         : SDL_ConvertSurface(src, SDL_PIXELFORMAT_XRGB8888);
  if (!rgb) { SDL_DestroySurface(src); return; }

  // Fit the rendered frame into the landscape framebuffer, preserving aspect.
  int const sw = rgb->w, sh = rgb->h;
  int dw = g.fb_w, dh = sh * g.fb_w / (sw ? sw : 1);
  if (dh > g.fb_h) { dh = g.fb_h; dw = sw * g.fb_h / (sh ? sh : 1); }
  int const x_off = (g.fb_w - dw) / 2, y_off = (g.fb_h - dh) / 2;

  if (g.xmap_src_w != sw || g.xmap_dst_w != dw) {
    g.xmap.resize(dw);
    for (int dx = 0; dx < dw; dx++) g.xmap[dx] = dx * sw / (dw ? dw : 1);
    g.xmap_src_w = sw; g.xmap_dst_w = dw;
  }

  DumbFb& back = g.fb[g.front ^ 1];
  std::memset(back.map, 0, back.size);           // letterbox bars
  for (int dy = 0; dy < dh; dy++) {
    auto const* srow = reinterpret_cast<uint32_t const*>(
        static_cast<uint8_t const*>(rgb->pixels) +
        static_cast<size_t>(dy * sh / (dh ? dh : 1)) * rgb->pitch);
    auto* drow = reinterpret_cast<uint32_t*>(
        back.map + static_cast<size_t>(dy + y_off) * back.pitch) + x_off;
    for (int dx = 0; dx < dw; dx++) drow[dx] = srow[g.xmap[dx]];
  }

  if (rgb != src) SDL_DestroySurface(rgb);
  SDL_DestroySurface(src);

  page_flip(back.fb_id);
  g.front ^= 1;
}

void drm_present_shutdown() {
  if (!g.ready) return;
  for (auto& f : g.fb) {
    if (f.map) munmap(f.map, f.size);
    if (f.fb_id) drmModeRmFB(g.fd, f.fb_id);
    if (f.handle) {
      drm_mode_destroy_dumb d{};
      d.handle = f.handle;
      drmIoctl(g.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
  }
  drmDropMaster(g.fd);
  close(g.fd);
  g = DrmState{};
}

#else  // !__linux__

bool drm_present_enabled() { return false; }
void drm_present_frame(SDL_Renderer*) {}
void drm_present_shutdown() {}

#endif
