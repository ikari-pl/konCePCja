// drm_probe — prove and measure the drm_direct present path on the K230.
//
// The panel is 568x1232 portrait and canaan-drm is display-only (no render
// node, no GBM), so the port must scan out dumb buffers written by the CPU.
// This measures the two costs that decide Risk #3:
//   A) bare page-flip rate (the ceiling)
//   B) rotated + scaled blit of a 768x270 CPC frame, then flip (the real job)
//
// Requires DRM master: stop k230_phone_ui first.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define CPC_W 768
#define CPC_H 270
#define NFRAMES 120

struct fb { uint32_t handle, pitch, fb_id; uint64_t size; uint8_t *map; };

static double now(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static int make_fb(int fd, uint32_t w, uint32_t h, struct fb *f) {
  struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
  if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) { perror("CREATE_DUMB"); return -1; }
  f->handle = c.handle; f->pitch = c.pitch; f->size = c.size;
  if (drmModeAddFB(fd, w, h, 24, 32, f->pitch, f->handle, &f->fb_id)) { perror("AddFB"); return -1; }
  struct drm_mode_map_dumb m = { .handle = f->handle };
  if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) { perror("MAP_DUMB"); return -1; }
  f->map = mmap(0, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
  if (f->map == MAP_FAILED) { perror("mmap"); return -1; }
  memset(f->map, 0, f->size);
  return 0;
}

static volatile int flip_pending = 0;
static void on_flip(int fd, unsigned f, unsigned s, unsigned us, void *d) {
  (void)fd;(void)f;(void)s;(void)us;(void)d; flip_pending = 0;
}

static int flip(int fd, uint32_t crtc, uint32_t fb_id) {
  if (drmModePageFlip(fd, crtc, fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL)) return -1;
  flip_pending = 1;
  drmEventContext ev = { .version = 2, .page_flip_handler = on_flip };
  while (flip_pending) { if (drmHandleEvent(fd, &ev)) return -1; }
  return 0;
}

int main(void) {
  int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (fd < 0) { perror("open card0"); return 1; }

  drmModeRes *res = drmModeGetResources(fd);
  if (!res) { perror("GetResources"); return 1; }

  drmModeConnector *conn = NULL;
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
    if (c) drmModeFreeConnector(c);
  }
  if (!conn) { fprintf(stderr, "no connected connector\n"); return 1; }
  drmModeModeInfo mode = conn->modes[0];

  drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id ? conn->encoder_id : conn->encoders[0]);
  uint32_t crtc_id = enc && enc->crtc_id ? enc->crtc_id : res->crtcs[0];

  printf("connector %u  mode %ux%u@%u  crtc %u\n",
         conn->connector_id, mode.hdisplay, mode.vdisplay, mode.vrefresh, crtc_id);

  // The primary plane's rotation property is rotate-270 (value 8 = 1<<3), so
  // the display engine rotates for us and the CRTC expects a LANDSCAPE source.
  uint32_t FBW = mode.vdisplay, FBH = mode.hdisplay;   // 1232x568
  struct fb fb[2];
  if (make_fb(fd, FBW, FBH, &fb[0])) return 1;
  if (make_fb(fd, FBW, FBH, &fb[1])) return 1;
  printf("dumb buffers: %ux%u (landscape; plane rotates 270 in HW) pitch=%u size=%llu KiB each\n",
         FBW, FBH, fb[0].pitch, (unsigned long long)(fb[0].size / 1024));

  if (drmSetMaster(fd)) fprintf(stderr, "warning: SetMaster failed (%s)\n", strerror(errno));
  if (drmModeSetCrtc(fd, crtc_id, fb[0].fb_id, 0, 0, &conn->connector_id, 1, &mode)) {
    perror("SetCrtc (is k230_phone_ui still running?)"); return 1;
  }

  // ---- A) bare page-flip rate ----
  double t0 = now();
  for (int i = 0; i < NFRAMES; i++)
    if (flip(fd, crtc_id, fb[i & 1].fb_id)) { perror("PageFlip"); return 1; }
  double ta = now() - t0;
  printf("A) bare flips     : %d in %.3fs = %.1f FPS (%.2f ms/flip)\n",
         NFRAMES, ta, NFRAMES / ta, ta * 1000 / NFRAMES);

  // ---- B) rotated + scaled blit of a CPC frame, then flip ----
  // Source is landscape 768x270; panel is portrait, so rotate 90 deg and
  // scale to fit. Destination is (vdisplay x hdisplay) in rotated space.
  uint32_t *src = malloc((size_t)CPC_W * CPC_H * 4);
  for (int y = 0; y < CPC_H; y++)
    for (int x = 0; x < CPC_W; x++)  // coarse CPC-ish colour bars
      src[y * CPC_W + x] = ((x / 48) % 2 ? 0x00d0a000u : 0x000060c0u) | ((y & 7) << 4);

  int dw = (int)FBW, dh = (int)FBH;                  // 1232x568 landscape
  int sc_n = dw, sc_d = CPC_W;                       // fit width
  int bh = CPC_H * sc_n / sc_d; if (bh > dh) bh = dh;
  int y_off = (dh - bh) / 2;
  printf("B) blit target    : %dx%d landscape, image %dx%d, y offset %d\n",
         dw, dh, dw, bh, y_off);

  // Precompute the source column for each destination column once.
  int *xmap = malloc(sizeof(int) * dw);
  for (int dx = 0; dx < dw; dx++) xmap[dx] = dx * sc_d / sc_n;

  t0 = now();
  double blit_total = 0;
  for (int f = 0; f < NFRAMES; f++) {
    struct fb *b = &fb[f & 1];
    double b0 = now();
    for (int dy = 0; dy < bh; dy++) {
      int sy = dy * sc_d / sc_n;
      const uint32_t *srow = src + (size_t)sy * CPC_W;
      uint32_t *drow = (uint32_t *)(b->map + (size_t)(dy + y_off) * b->pitch);
      for (int dx = 0; dx < dw; dx++) drow[dx] = srow[xmap[dx]];
    }
    blit_total += now() - b0;
    if (flip(fd, crtc_id, b->fb_id)) { perror("PageFlip"); return 1; }
  }
  double tb = now() - t0;
  printf("B) blit+flip      : %d in %.3fs = %.1f FPS (%.2f ms/frame)\n",
         NFRAMES, tb, NFRAMES / tb, tb * 1000 / NFRAMES);
  printf("   blit alone     : %.2f ms/frame  (%.1f%% of a 20 ms CPC budget)\n",
         blit_total * 1000 / NFRAMES, blit_total * 1000 / NFRAMES / 20.0 * 100);

  printf("\nVERDICT: %s\n", (tb * 1000 / NFRAMES) < 20.0
         ? "fits inside the 20 ms/frame CPC budget"
         "" : "EXCEEDS the 20 ms CPC budget — needs optimisation (try -march=rv64gcv)");
  sleep(1);
  drmModeFreeConnector(conn);
  if (enc) drmModeFreeEncoder(enc);
  drmModeFreeResources(res);
  return 0;
}
