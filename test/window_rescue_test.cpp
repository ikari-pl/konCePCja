// Pure off-screen-window rescue geometry (src/window_rescue.h).
//
// The rescue only fires when a window overlaps no display by at least
// `min_visible` points on BOTH axes, and then clamps (never resizes) the window
// into the nearest display's usable area. These are the overlap/clamp edge
// cases where off-by-one bugs hide: exact-edge touching, a sliver peeking on,
// windows bigger than the display, negative origins, and nearest-of-N displays.

#include "window_rescue.h"

#include "gtest/gtest.h"

using koncpc::compute_onscreen_position;
using koncpc::Rect;

namespace {
// A single 1920x1080 primary display at the origin, usable area unindented for
// simplicity (a real macOS usable area excludes the menu bar, but the geometry
// is identical).
const Rect kPrimary{0, 0, 1920, 1080};
}  // namespace

// A window comfortably inside the display never moves.
TEST(WindowRescue, FullyVisibleNoMove) {
  int nx = -1, ny = -1;
  EXPECT_FALSE(compute_onscreen_position(Rect{100, 100, 800, 600}, &kPrimary, 1,
                                         48, nx, ny));
  EXPECT_EQ(nx, -1);  // outputs untouched
  EXPECT_EQ(ny, -1);
}

// A window entirely to the right of the display is rescued, clamped so its
// right edge sits on the display's right edge (x = 1920 - width).
TEST(WindowRescue, FullyRightIsClampedToRightEdge) {
  int nx = 0, ny = 0;
  EXPECT_TRUE(compute_onscreen_position(Rect{3000, 200, 800, 600}, &kPrimary, 1,
                                        48, nx, ny));
  EXPECT_EQ(nx, 1920 - 800);
  EXPECT_EQ(ny, 200);  // y already inside, left as-is
}

// A window entirely above/left (negative origin) pins to the display origin.
TEST(WindowRescue, NegativeOriginClampsToZero) {
  int nx = 5, ny = 5;
  EXPECT_TRUE(compute_onscreen_position(Rect{-900, -700, 800, 600}, &kPrimary,
                                        1, 48, nx, ny));
  EXPECT_EQ(nx, 0);
  EXPECT_EQ(ny, 0);
}

// Overlap of exactly min_visible on both axes counts as visible (>=), no move.
TEST(WindowRescue, ExactMinVisibleCountsAsOnScreen) {
  // Window's left edge at x = 1920 - 48 leaves exactly 48px overlapping in x;
  // y fully inside. Should be considered visible.
  int nx = -1, ny = -1;
  EXPECT_FALSE(compute_onscreen_position(Rect{1920 - 48, 100, 800, 200},
                                         &kPrimary, 1, 48, nx, ny));
  EXPECT_EQ(nx, -1);
}

// One point less than min_visible is a lost sliver — rescued.
TEST(WindowRescue, SliverBelowMinVisibleIsRescued) {
  int nx = 0, ny = 0;
  EXPECT_TRUE(compute_onscreen_position(Rect{1920 - 47, 100, 800, 200},
                                        &kPrimary, 1, 48, nx, ny));
  EXPECT_EQ(nx, 1920 - 800);
  EXPECT_EQ(ny, 100);
}

// With min_visible=1, a window still overlapping by 1px is NOT rescued (so an
// intentional partial drag off one edge is never fought).
TEST(WindowRescue, MinVisibleOneKeepsOnePixelOverlap) {
  int nx = -1, ny = -1;
  // 1px of the window's left edge remains over the display's right edge.
  EXPECT_FALSE(compute_onscreen_position(Rect{1919, 100, 800, 200}, &kPrimary,
                                         1, 1, nx, ny));
  EXPECT_EQ(nx, -1);
}

// Zero overlap with min_visible=1 IS rescued.
TEST(WindowRescue, MinVisibleOneRescuesZeroOverlap) {
  int nx = 0, ny = 0;
  EXPECT_TRUE(compute_onscreen_position(Rect{1920, 100, 800, 200}, &kPrimary, 1,
                                        1, nx, ny));
  EXPECT_EQ(nx, 1920 - 800);
}

// A window larger than the display pins its top-left to the display origin
// (max_x clamps to nearest.x, never negative), keeping the titlebar reachable.
TEST(WindowRescue, LargerThanDisplayPinsTopLeft) {
  const Rect small{0, 0, 800, 600};
  int nx = 0, ny = 0;
  EXPECT_TRUE(compute_onscreen_position(Rect{5000, 5000, 1200, 900}, &small, 1,
                                        48, nx, ny));
  EXPECT_EQ(nx, 0);
  EXPECT_EQ(ny, 0);
}

// Two displays side by side: a window lost off the right of the second display
// is rescued onto the second (nearest), not the first.
TEST(WindowRescue, PicksNearestOfMultipleDisplays) {
  const Rect displays[2] = {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}};
  int nx = 0, ny = 0;
  EXPECT_TRUE(compute_onscreen_position(Rect{4000, 300, 800, 600}, displays, 2,
                                        48, nx, ny));
  // Nearest is the right display (origin x=1920); clamp to its right edge.
  EXPECT_EQ(nx, 1920 + 1920 - 800);
  EXPECT_EQ(ny, 300);
}

// A window visible on the SECOND display is not moved even though it misses the
// first (the visible-anywhere check must scan all displays, not just the
// first).
TEST(WindowRescue, VisibleOnSecondDisplayNoMove) {
  const Rect displays[2] = {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}};
  int nx = -1, ny = -1;
  EXPECT_FALSE(compute_onscreen_position(Rect{2200, 300, 800, 600}, displays, 2,
                                         48, nx, ny));
  EXPECT_EQ(nx, -1);
}

// Degenerate inputs are safe no-ops.
TEST(WindowRescue, DegenerateInputsNoMove) {
  int nx = -1, ny = -1;
  EXPECT_FALSE(compute_onscreen_position(Rect{100, 100, 0, 0}, &kPrimary, 1, 48,
                                         nx, ny));  // zero-size window
  EXPECT_FALSE(compute_onscreen_position(Rect{100, 100, 800, 600}, nullptr, 0,
                                         48, nx, ny));  // no displays
  EXPECT_EQ(nx, -1);
}
