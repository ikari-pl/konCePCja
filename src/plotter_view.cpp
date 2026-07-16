/* plotter_view.cpp — see plotter_view.h. */

#include "plotter_view.h"

#include <cstring>

#include "hw/plotter_hp7470a.h"
#include "subcycle/machine.h"
#include "subcycle_bridge.h"

namespace {

// The Device page converted to the legacy PlotSegment shape, rebuilt only
// when page_rev moves (the preview runs every frame; the page rarely does).
std::vector<PlotSegment> g_dev_cache;
uint32_t g_dev_cache_rev = 0xFFFFFFFFu;

// The live Device, or nullptr when the legacy path owns the page.
const Device* live_device(subcycle::Machine** m_out = nullptr) {
  subcycle::Machine* m = subcycle_bridge_machine();
  if (!m) return nullptr;
  const Device* dev = m->plotter();
  PlotterRegs r;
  plotter_hp7470a_peek(dev, &r);
  if (!r.plugged) return nullptr;
  if (m_out) *m_out = m;
  return dev;
}

PlotPrimitive prim_of(uint8_t t) {
  switch (t) {
    case 1:
      return PlotPrimitive::Circle;
    case 2:
      return PlotPrimitive::Arc;
    case 3:
      return PlotPrimitive::Label;
    default:
      return PlotPrimitive::Line;
  }
}

const std::vector<PlotSegment>& device_segments(const Device* dev) {
  PlotterRegs r;
  plotter_hp7470a_peek(dev, &r);
  if (r.page_rev == g_dev_cache_rev) return g_dev_cache;
  const PlotSeg* segs = nullptr;
  const size_t n = plotter_hp7470a_segments(dev, &segs);
  g_dev_cache.clear();
  g_dev_cache.reserve(n);
  for (size_t i = 0; i < n; i++) {
    PlotSegment s;
    s.type = prim_of(segs[i].type);
    s.pen = segs[i].pen;
    s.x1 = segs[i].x1;
    s.y1 = segs[i].y1;
    s.x2 = segs[i].x2;
    s.y2 = segs[i].y2;
    s.radius = segs[i].radius;
    s.start_angle = segs[i].start_angle;
    s.sweep_angle = segs[i].sweep_angle;
    s.line_type = segs[i].line_type;
    s.text.assign(segs[i].text, strnlen(segs[i].text, sizeof(segs[i].text)));
    g_dev_cache.push_back(std::move(s));
  }
  g_dev_cache_rev = r.page_rev;
  return g_dev_cache;
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
const std::vector<PlotSegment>& plotter_view_segments() {
  if (const Device* dev = live_device()) return device_segments(dev);
  return g_plotter.segments();
}

PlotterViewStatus plotter_view_status() {
  PlotterViewStatus st;
  if (const Device* dev = live_device()) {
    PlotterRegs r;
    plotter_hp7470a_peek(dev, &r);
    st.pen = r.selected_pen;
    st.pen_down = r.pen_down != 0;
    st.x = r.pen_x;
    st.y = r.pen_y;
    return st;
  }
  st.pen = g_plotter.selected_pen();
  st.pen_down = g_plotter.pen_down();
  st.x = g_plotter.pen_x();
  st.y = g_plotter.pen_y();
  return st;
}

void plotter_view_clear() {
  if (const Device* dev = live_device()) {
    plotter_hp7470a_clear_page(dev);
    g_dev_cache_rev = 0xFFFFFFFFu;  // force reconvert
    return;
  }
  g_plotter.clear();
}

bool plotter_view_export_svg(const std::string& path) {
  if (const Device* dev = live_device()) {
    // The Device carries no character-size state host-side; labels export at
    // the power-on default height (0.269 cm), matching a fresh HpglPlotter.
    return plotter_export_svg_segments(device_segments(dev), 0.269f, path);
  }
  return g_plotter.export_svg(path);
}
