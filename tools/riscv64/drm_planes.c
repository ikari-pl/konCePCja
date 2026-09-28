// Enumerate DRM planes and their properties — specifically 'rotation'.
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int main(void) {
  int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (fd < 0) { perror("open"); return 1; }
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
  if (!pr) { perror("GetPlaneResources"); return 1; }
  printf("planes: %u\n", pr->count_planes);
  for (uint32_t i = 0; i < pr->count_planes; i++) {
    drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
    if (!p) continue;
    printf("\nplane %u: crtc=%u fb=%u possible_crtcs=0x%x formats=%u\n",
           p->plane_id, p->crtc_id, p->fb_id, p->possible_crtcs, p->count_formats);
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
    if (props) {
      for (uint32_t j = 0; j < props->count_props; j++) {
        drmModePropertyRes *pp = drmModeGetProperty(fd, props->props[j]);
        if (!pp) continue;
        printf("   %-16s = %llu", pp->name, (unsigned long long)props->prop_values[j]);
        if (pp->count_enums > 0) {
          printf("   [");
          for (int k = 0; k < pp->count_enums; k++)
            printf("%s%s=%llu", k ? " " : "", pp->enums[k].name,
                   (unsigned long long)pp->enums[k].value);
          printf("]");
        }
        printf("\n");
        drmModeFreeProperty(pp);
      }
      drmModeFreeObjectProperties(props);
    }
    drmModeFreePlane(p);
  }
  return 0;
}
