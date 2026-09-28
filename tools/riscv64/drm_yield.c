// drm_yield — give the vendor launcher the ability to hand over the display.
//
// k230_phone_ui holds DRM master for its whole lifetime and never calls
// drmDropMaster, so no other process can drive the panel (drmSetMaster returns
// EBUSY, drmModeSetPlane EACCES). Rebuilding it needs the full Buildroot SDK,
// so instead this is LD_PRELOADed into the shipped binary.
//
// DRM master belongs to the open file description, not the process, so all we
// need is the launcher's own fd for /dev/dri/card0 and two ioctls issued from
// inside its process:
//
//   kill -USR1 <pid>   ->  save the CRTC, drmDropMaster   (panel is free)
//   kill -USR2 <pid>   ->  drmSetMaster, restore the CRTC (panel is back)
//
// This is a working stand-in for lv_linux_drm_release()/acquire() upstream.
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int g_drm_fd = -1;
static drmModeCrtc *g_saved_crtc = NULL;
static uint32_t g_saved_conn = 0;

// Find the process's own fd for the DRM device. Interposing open() would need
// dlsym(), which glibc only moved into libc at 2.34 -- linking against it makes
// the library unloadable on this board's 2.33. Reading /proc/self/fd needs
// nothing but open/readlink and works on any glibc.
static int find_drm_fd(void) {
  if (g_drm_fd >= 0) return g_drm_fd;
  for (int fd = 0; fd < 256; fd++) {
    char link[64], target[256];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) continue;
    target[n] = 0;
    if (strncmp(target, "/dev/dri/card", 13) == 0) {
      g_drm_fd = fd;
      return fd;
    }
  }
  return -1;
}

static void on_release(int sig) {
  (void)sig;
  if (find_drm_fd() < 0) return;
  // Remember the mode so it can be restored: the app we hand over to will
  // reprogram the CRTC for its own framebuffer.
  drmModeRes *res = drmModeGetResources(g_drm_fd);
  if (res) {
    for (int i = 0; i < res->count_connectors && !g_saved_conn; i++) {
      drmModeConnector *c = drmModeGetConnector(g_drm_fd, res->connectors[i]);
      if (c && c->connection == DRM_MODE_CONNECTED) g_saved_conn = c->connector_id;
      if (c) drmModeFreeConnector(c);
    }
    if (!g_saved_crtc && res->count_crtcs > 0)
      g_saved_crtc = drmModeGetCrtc(g_drm_fd, res->crtcs[0]);
    drmModeFreeResources(res);
  }
  int rc = drmDropMaster(g_drm_fd);
  fprintf(stderr, "[drm_yield] drmDropMaster(fd=%d) rc=%d\n", g_drm_fd, rc);
}

static void on_acquire(int sig) {
  (void)sig;
  if (find_drm_fd() < 0) return;
  int rc = drmSetMaster(g_drm_fd);
  if (rc == 0 && g_saved_crtc && g_saved_conn) {
    drmModeSetCrtc(g_drm_fd, g_saved_crtc->crtc_id, g_saved_crtc->buffer_id,
                   g_saved_crtc->x, g_saved_crtc->y, &g_saved_conn, 1,
                   &g_saved_crtc->mode);
  }
  fprintf(stderr, "[drm_yield] drmSetMaster(fd=%d) rc=%d\n", g_drm_fd, rc);
}

__attribute__((constructor)) static void init(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_release;
  sigaction(SIGUSR1, &sa, NULL);
  sa.sa_handler = on_acquire;
  sigaction(SIGUSR2, &sa, NULL);
  fprintf(stderr, "[drm_yield] armed: SIGUSR1 releases, SIGUSR2 reacquires\n");
}
