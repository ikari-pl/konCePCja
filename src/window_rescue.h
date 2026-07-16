// konCePCja - Amstrad CPC Emulator
// Pure geometry for pulling an off-screen window back onto a display.
//
// Extracted from the SDL-coupled rescue in kon_cpc_ja.cpp so the overlap/clamp
// math (which is exactly the kind of layout arithmetic that hides off-by-one
// bugs) can be unit-tested without a window or a display. The SDL wrapper just
// gathers window/display rects, calls this, and applies the result.

#pragma once

namespace koncpc {

// A rectangle in the global desktop coordinate space (points), matching the
// x/y/w/h layout of SDL_Rect but carrying no SDL dependency.
struct Rect {
  int x, y, w, h;
};

// Decide whether `win` is "lost" — i.e. it overlaps no display's usable area by
// at least `min_visible` points on BOTH axes — and, if so, compute a rescued
// top-left that clamps the window into the nearest display's usable area WITHOUT
// resizing it. A window larger than the target display pins its top-left to the
// display origin so the titlebar stays reachable.
//
// Returns true and writes out_x/out_y when the window must move; returns false
// (and leaves the outputs untouched) when the window is already visible enough,
// or when there is no display information to rescue toward.
//
// `min_visible`: pass a titlebar-ish value (~48) to guarantee the window is
// grabbable, or 1 to rescue only a truly-lost, zero-overlap window (so an
// intentional partial drag off one edge is never fought).
inline bool compute_onscreen_position(const Rect& win, const Rect* displays,
                                      int n, int min_visible, int& out_x,
                                      int& out_y) {
  if (win.w <= 0 || win.h <= 0 || displays == nullptr || n <= 0) return false;

  auto imin = [](int a, int b) { return a < b ? a : b; };
  auto imax = [](int a, int b) { return a > b ? a : b; };

  bool have_nearest = false;
  Rect nearest{};
  long long nearest_dist = 0;

  for (int i = 0; i < n; ++i) {
    const Rect& b = displays[i];
    // Overlap of the window with this display's usable area, per axis.
    const int ox = imin(win.x + win.w, b.x + b.w) - imax(win.x, b.x);
    const int oy = imin(win.y + win.h, b.y + b.h) - imax(win.y, b.y);
    if (ox >= min_visible && oy >= min_visible) return false;  // visible enough

    // Not visible on this display — remember the nearest (by centre distance)
    // as the rescue target in case no display turns out to be visible.
    const long long dx =
        static_cast<long long>(win.x + (win.w / 2)) - (b.x + (b.w / 2));
    const long long dy =
        static_cast<long long>(win.y + (win.h / 2)) - (b.y + (b.h / 2));
    const long long d = (dx * dx) + (dy * dy);
    if (!have_nearest || d < nearest_dist) {
      nearest_dist = d;
      nearest = b;
      have_nearest = true;
    }
  }
  if (!have_nearest) return false;

  // Clamp the current position into the nearest usable area (no resize).
  const int max_x = imax(nearest.x, nearest.x + nearest.w - win.w);
  const int max_y = imax(nearest.y, nearest.y + nearest.h - win.h);
  // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): nested conditional kept intentionally; no clang-tidy auto-fix
  out_x = win.x < nearest.x ? nearest.x : (win.x > max_x ? max_x : win.x);
  // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): nested conditional kept intentionally; no clang-tidy auto-fix
  out_y = win.y < nearest.y ? nearest.y : (win.y > max_y ? max_y : win.y);
  return true;
}

}  // namespace koncpc
