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
#include <cstdio>
#include <cstring>
#include <ctime>

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
  // Triple buffered: with only two buffers the render thread must wait for the
  // in-flight flip to retire before it can draw again, serialising flip-wait
  // and render (~46 ms cycle for 36 ms of work). A third buffer means there is
  // always one that is neither scanned out nor pending.
  DumbFb fb[3];
  int front = 0;      // being scanned out
  int pending = -1;   // flip submitted, not yet retired
  int next = 1;       // safe to draw into
  bool ready = false;
  bool failed = false;
  std::vector<int> xmap;    // destination column -> source column
  int xmap_src_w = -1, xmap_dst_w = -1;
  int cleared_w = -1, cleared_h = -1;
  double last_submit_ms = 0;
  int last_flip_errno = 0;
  // instrumentation
  unsigned long n_frames = 0, n_flip_ok = 0, n_flip_fail = 0, n_drain_hits = 0;
  double drain_ms = 0, blit_ms = 0, read_ms = 0, conv_ms = 0, last_report = 0;
  int src_fmt = 0, src_w = 0, src_h = 0;
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

  bool ok = make_fb(g.fd, g.fb_w, g.fb_h, g.fb[0]) &&
            make_fb(g.fd, g.fb_w, g.fb_h, g.fb[1]) &&
            make_fb(g.fd, g.fb_w, g.fb_h, g.fb[2]);
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
volatile unsigned last_flip_seq = 0;
void on_flip(int, unsigned seq, unsigned, unsigned, void*) {
  flip_pending = 0;
  last_flip_seq = seq;
}

// Block until the in-flight page flip (if any) has retired. Until it does, the
// buffer we are about to draw into may still be the one being scanned out, and
// writing it produces tearing.
void drain_flip() {
  if (!flip_pending) return;
  g.n_drain_hits++;
  drmEventContext ev{};
  ev.version = 2;
  ev.page_flip_handler = on_flip;
  while (flip_pending) {
    if (drmHandleEvent(g.fd, &ev)) { flip_pending = 0; break; }
  }
}

// Pace to a whole number of panel refreshes. The render thread produces a
// frame roughly every 37 ms while the panel refreshes every ~19.2 ms, so
// unpaced flips land alternately 1 and 2 refreshes apart -- a visible judder
// even though every individual flip is vsync-latched. Waiting until at least
// KONCPC_DRM_DIVISOR refreshes have elapsed locks the cadence instead.
// Default 2 (26 Hz on a 52 Hz panel); 1 restores free-running.
int refresh_divisor() {
  static int cached = -1;
  if (cached < 0) {
    const char* v = std::getenv("KONCPC_DRM_DIVISOR");
    cached = v ? std::atoi(v) : 2;
    if (cached < 1) cached = 1;
  }
  return cached;
}

// Wait until the panel reaches an absolute vblank count. Targeting an absolute
// sequence (rather than sleeping a relative number of vblanks) waits only the
// remainder of the slot, so a render that already took most of it costs nothing
// extra -- the earlier relative version added a whole refresh and paced to 3.
void wait_until_vblank(unsigned target_seq) {
  drmVBlank vbl{};
  vbl.request.type = DRM_VBLANK_ABSOLUTE;
  vbl.request.sequence = target_seq;
  drmWaitVBlank(g.fd, &vbl);
}

// Returns true when the flip was accepted, so the caller only swaps buffers on
// success. An unchecked failure here was the other half of the tearing: the
// front index advanced anyway and the next frame drew into the live buffer.
bool page_flip(uint32_t fb_id) {
  int rc = drmModePageFlip(g.fd, g.crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, nullptr);
  if (rc) { g.n_flip_fail++; g.last_flip_errno = -rc; return false; }
  flip_pending = 1;
  g.n_flip_ok++;
  return true;
}

double now_ms() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
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

  double const t_read0 = now_ms();
  SDL_Surface* src = SDL_RenderReadPixels(renderer, nullptr);
  if (!src) return;
  double const t_conv0 = now_ms();
  g.read_ms += t_conv0 - t_read0;
  SDL_Surface* rgb = (src->format == SDL_PIXELFORMAT_XRGB8888)
                         ? src
                         : SDL_ConvertSurface(src, SDL_PIXELFORMAT_XRGB8888);
  g.conv_ms += now_ms() - t_conv0;
  if (g.n_frames == 0) g.src_fmt = (int)src->format;
  if (!rgb) { SDL_DestroySurface(src); return; }

  // Fit the rendered frame into the landscape framebuffer, preserving aspect.
  int const sw = rgb->w, sh = rgb->h;
  g.src_w = sw; g.src_h = sh;
  int dw = g.fb_w, dh = sh * g.fb_w / (sw ? sw : 1);
  if (dh > g.fb_h) { dh = g.fb_h; dw = sw * g.fb_h / (sh ? sh : 1); }
  int const x_off = (g.fb_w - dw) / 2, y_off = (g.fb_h - dh) / 2;

  if (g.xmap_src_w != sw || g.xmap_dst_w != dw) {
    g.xmap.resize(dw);
    for (int dx = 0; dx < dw; dx++) g.xmap[dx] = dx * sw / (dw ? dw : 1);
    g.xmap_src_w = sw; g.xmap_dst_w = dw;
  }

  // DRM allows only one pending flip per CRTC, so the previous event must be
  // consumed before another can be submitted -- leaving it queued makes every
  // later flip fail with ENOMEM. Because a render takes longer than a refresh
  // the flip has almost always retired already, so this costs nothing; the
  // third buffer is what stops it from serialising when it has not.
  double const t_drain0 = now_ms();
  drain_flip();
  g.pending = -1;
  double const t_blit0 = now_ms();
  g.drain_ms += t_blit0 - t_drain0;

  DumbFb& back = g.fb[g.next];
  // Clear only when the letterbox geometry changes; a full 2.7 MB memset every
  // frame is pure cost once the bars are already black.
  if (g.cleared_w != dw || g.cleared_h != dh) {
    std::memset(g.fb[0].map, 0, g.fb[0].size);
    std::memset(g.fb[1].map, 0, g.fb[1].size);
    g.cleared_w = dw; g.cleared_h = dh;
  }
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

  g.blit_ms += now_ms() - t_blit0;
  // Pace BEFORE submitting, not after. Waiting after the flip serialises the
  // flip-wait with the next render and costs a whole extra refresh; holding
  // here only burns the remainder of the slot the render did not use.
  int const div = refresh_divisor();
  if (div > 1 && g.last_submit_ms > 0) {
    double const period = 1000.0 / (g.mode.vrefresh ? g.mode.vrefresh : 60);
    double const due = g.last_submit_ms + period * div;
    double slack = due - now_ms();
    while (slack > 0.5) {
      struct timespec ts {};
      ts.tv_sec = 0;
      ts.tv_nsec = (long)(slack * 1e6);
      nanosleep(&ts, nullptr);
      slack = due - now_ms();
    }
  }
  if (page_flip(back.fb_id)) {
    g.last_submit_ms = now_ms();
    g.pending = g.next;
    g.front = g.next;
    g.next = (g.next + 1) % 3;
  }

  g.n_frames++;
  double const t_now = now_ms();
  if (g.last_report == 0) g.last_report = t_now;
  if (t_now - g.last_report >= 1000.0) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "drm_present: %lu fps  flips ok=%lu fail=%lu (errno %d)  "
             "readback %.1f  convert %.1f  drain %.2f  blit %.2f ms/f  src %dx%d fmt 0x%x",
             g.n_frames, g.n_flip_ok, g.n_flip_fail, g.last_flip_errno,
             g.read_ms / g.n_frames, g.conv_ms / g.n_frames,
             g.drain_ms / g.n_frames, g.blit_ms / g.n_frames,
             g.src_w, g.src_h, g.src_fmt);
    LOG_INFO(std::string(buf));
    g.n_frames = g.n_flip_ok = g.n_flip_fail = g.n_drain_hits = 0;
    g.drain_ms = g.blit_ms = g.read_ms = g.conv_ms = 0;
    g.last_report = t_now;
  }
}

void drm_present_shutdown() {
  if (!g.ready) return;
  drain_flip();
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
