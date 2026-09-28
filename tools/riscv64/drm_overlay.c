// Can we composite ON TOP of the running launcher using an idle overlay plane?
//
// k230_phone_ui drives only the primary plane (34). Planes 40/43 are overlays
// advertising the same 7 formats. If drmModeSetPlane succeeds while the
// launcher holds the display, konCePCja can present as an overlay instead of
// fighting for the primary -- no handover, no killing the launcher.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int main(int argc, char** argv) {
  uint32_t const plane_id = (argc > 1) ? (uint32_t)atoi(argv[1]) : 40;
  int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (fd < 0) { perror("open"); return 1; }
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

  drmModeRes* res = drmModeGetResources(fd);
  uint32_t crtc = res && res->count_crtcs ? res->crtcs[0] : 0;
  // use the CRTC the primary is actually on
  drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
  for (uint32_t i = 0; pr && i < pr->count_planes; i++) {
    drmModePlane* p = drmModeGetPlane(fd, pr->planes[i]);
    if (p && p->crtc_id) crtc = p->crtc_id;
    if (p) drmModeFreePlane(p);
  }
  printf("using crtc %u, overlay plane %u\n", crtc, plane_id);

  int const W = 320, H = 240;
  struct drm_mode_create_dumb c = { .width = W, .height = H, .bpp = 16 };
  if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) { perror("CREATE_DUMB"); return 1; }
  uint32_t handles[4] = {c.handle,0,0,0}, pitches[4] = {c.pitch,0,0,0}, offsets[4] = {0,0,0,0};
  uint32_t fb = 0;
  if (drmModeAddFB2(fd, W, H, DRM_FORMAT_RGB565, handles, pitches, offsets, &fb, 0)) {
    perror("AddFB2"); return 1;
  }
  struct drm_mode_map_dumb m = { .handle = c.handle };
  drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m);
  uint8_t* map = mmap(0, c.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, m.offset);
  if (map == MAP_FAILED) { perror("mmap"); return 1; }
  for (int y = 0; y < H; y++) {
    uint16_t* row = (uint16_t*)(map + (size_t)y * c.pitch);
    for (int x = 0; x < W; x++)
      row[x] = ((x / 20 + y / 20) % 2) ? 0x07E0 : 0xF800;  // green/red checks
  }

  // Plane programming requires DRM master. The launcher holds it, so see
  // whether we can take it and whether it survives -- that decides if
  // overlay compositing alongside the launcher is possible at all.
  int const sm = drmSetMaster(fd);
  printf("drmSetMaster rc=%d%s\n", sm, sm ? strerror(errno) : "");
  printf("drmModeSetPlane...\n");
  int rc = drmModeSetPlane(fd, plane_id, crtc, fb, 0,
                           60, 60, W, H,            // where on screen
                           0, 0, W << 16, H << 16); // source rect (16.16)
  if (rc) { printf("  FAILED rc=%d errno=%d (%s)\n", rc, errno, strerror(errno)); }
  else    { printf("  OK -- checkerboard should be ON TOP of the launcher\n"); }

  sleep(12);
  if (!rc) drmModeSetPlane(fd, plane_id, crtc, 0, 0, 0,0,0,0, 0,0,0,0);
  printf("cleaned up\n");
  return 0;
}
