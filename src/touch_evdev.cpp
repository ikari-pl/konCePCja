#include "touch_evdev.h"

#if defined(__linux__)

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "imgui.h"

namespace {

struct TouchState {
  int fd = -1;
  bool tried = false;
  int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
  int raw_x = -1, raw_y = -1;
  bool down = false;          // live contact state from the device
  bool saw_press = false;     // a press arrived during this poll
  bool hold_release = false;  // release deferred so ImGui sees the down-frame
  int rot = -1;  // 0/90/180/270: panel orientation relative to the framebuffer
};

TouchState g;

int rotation() {
  if (g.rot < 0) {
    const char* v = std::getenv("KONCPC_TOUCH_ROT");
    g.rot = v ? std::atoi(v) : 270;  // matches the plane's rotate-270 default
  }
  return g.rot;
}

void open_device() {
  g.tried = true;
  const char* dev = std::getenv("KONCPC_TOUCH");
  if (!dev || !*dev) return;
  g.fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (g.fd < 0) return;
  input_absinfo ai{};
  if (ioctl(g.fd, EVIOCGABS(ABS_X), &ai) == 0) { g.min_x = ai.minimum; g.max_x = ai.maximum; }
  if (ioctl(g.fd, EVIOCGABS(ABS_Y), &ai) == 0) { g.min_y = ai.minimum; g.max_y = ai.maximum; }
  if (g.max_x <= g.min_x) g.max_x = g.min_x + 1;
  if (g.max_y <= g.min_y) g.max_y = g.min_y + 1;
}

}  // namespace

void touch_evdev_poll(int surface_w, int surface_h) {
  if (!g.tried) open_device();
  if (g.fd < 0) return;

  input_event ev[64];
  for (;;) {
    ssize_t const n = read(g.fd, ev, sizeof(ev));
    if (n <= 0) break;
    size_t const count = static_cast<size_t>(n) / sizeof(input_event);
    for (size_t i = 0; i < count; i++) {
      if (ev[i].type == EV_ABS) {
        if (ev[i].code == ABS_X || ev[i].code == ABS_MT_POSITION_X) g.raw_x = ev[i].value;
        if (ev[i].code == ABS_Y || ev[i].code == ABS_MT_POSITION_Y) g.raw_y = ev[i].value;
        if (ev[i].code == ABS_MT_TRACKING_ID) {
          bool const d = (ev[i].value >= 0);
          if (d && !g.down) g.saw_press = true;
          g.down = d;
        }
      } else if (ev[i].type == EV_KEY && ev[i].code == BTN_TOUCH) {
        bool const d = (ev[i].value != 0);
        if (d && !g.down) g.saw_press = true;
        g.down = d;
      }
    }
  }
  if (g.raw_x < 0 || g.raw_y < 0) return;

  // Normalise to 0..1 in the panel's own (portrait) coordinate space, then
  // rotate into the landscape framebuffer the UI is laid out in.
  float const nx = static_cast<float>(g.raw_x - g.min_x) /
                   static_cast<float>(g.max_x - g.min_x);
  float const ny = static_cast<float>(g.raw_y - g.min_y) /
                   static_cast<float>(g.max_y - g.min_y);
  float fx = nx, fy = ny;
  switch (rotation()) {
    case 90:  fx = ny;         fy = 1.0f - nx; break;
    case 180: fx = 1.0f - nx;  fy = 1.0f - ny; break;
    case 270: fx = 1.0f - ny;  fy = nx;        break;
    default:  break;
  }

  // A tap can begin and end inside one frame at this refresh rate, which would
  // net out as "never pressed" and only move the cursor -- the button would
  // highlight but never activate. Report the press for a whole frame and let
  // the release land on the next one.
  bool report_down = g.down;
  if (g.hold_release) {
    report_down = false;
    g.hold_release = false;
  } else if (g.saw_press) {
    report_down = true;
    if (!g.down) g.hold_release = true;  // press+release collapsed into one poll
  }
  g.saw_press = false;

  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(fx * static_cast<float>(surface_w),
                      fy * static_cast<float>(surface_h));
  io.AddMouseButtonEvent(0, report_down);
}

void touch_evdev_shutdown() {
  if (g.fd >= 0) close(g.fd);
  g = TouchState{};
}

#else

void touch_evdev_poll(int, int) {}
void touch_evdev_shutdown() {}

#endif
