#include <gtest/gtest.h>

#include "koncepcja.h"
#include "menu_bridge.h"
#include "video_host.h"

extern SDL_Surface* pub;
extern SDL_Surface* scaled;
extern t_CPC CPC;

namespace {

// The plugin renders into CPC_RENDER_WIDTH x (CPC_VISIBLE_SCR_HEIGHT * 2):
// video_init() inits every plugin at scale 2, doubling the scanlines.  The
// window geometry must follow that surface, not the undoubled height.
class WindowGeometryTest : public testing::Test {
 protected:
  void SetUp() override { saved_ = CPC; }
  void TearDown() override { CPC = saved_; }

 private:
  t_CPC saved_;
};

TEST_F(WindowGeometryTest, FixedScaleCrtAspectIsFourThirds) {
  CPC.scr_scale = 1;
  CPC.scr_crt_aspect = 1;
  int w = 0;
  int h = 0;
  ASSERT_TRUE(video_derived_window_size(w, h));
  EXPECT_EQ(w, CPC_RENDER_WIDTH);
  EXPECT_EQ(h, CPC_RENDER_WIDTH * 3 / 4);
}

TEST_F(WindowGeometryTest, FixedScaleRawAspectMatchesTheRenderSurface) {
  CPC.scr_scale = 1;
  CPC.scr_crt_aspect = 0;
  int w = 0;
  int h = 0;
  ASSERT_TRUE(video_derived_window_size(w, h));
  EXPECT_EQ(w, CPC_RENDER_WIDTH);
  // Not CPC_VISIBLE_SCR_HEIGHT: that would be a 2.84:1 letterbox window.
  EXPECT_EQ(h, CPC_VISIBLE_SCR_HEIGHT * 2);
}

TEST_F(WindowGeometryTest, FitModeHasNoDerivedSize) {
  CPC.scr_scale = 0;
  int w = 0;
  int h = 0;
  EXPECT_FALSE(video_derived_window_size(w, h));
}

TEST_F(WindowGeometryTest, FitModeDefaultSizeIsTheOneXGeometry) {
  CPC.scr_scale = 0;
  CPC.scr_crt_aspect = 0;
  int w = 0;
  int h = 0;
  video_default_window_size(w, h);
  EXPECT_EQ(w, CPC_RENDER_WIDTH);
  EXPECT_EQ(h, CPC_VISIBLE_SCR_HEIGHT * 2);
}

// ─── Persisted win_w/win_h sanity gate ───────────────────────────────────
//
// win_w/win_h come straight from the user's config with only a > 0 bounds
// check.  A stale degenerate value left there by an earlier bug (the real one
// was 1536x540 — exactly the raw plugin-init letterbox) used to be restored
// verbatim on the first fullscreen exit of a fresh launch, reproducing the
// squished window the geometry fix above was meant to eliminate.

TEST_F(WindowGeometryTest, PersistedSizeRejectsTheRealWorldLetterbox) {
  // 2.84:1 — the value found in the user's config that triggered the fix.
  EXPECT_FALSE(video_persisted_window_size_is_sane(1536, 540));
  // The same shape at 1x, in case the plugin-init size ever leaks at scale 1.
  EXPECT_FALSE(video_persisted_window_size_is_sane(768, 270));
  // And at 4x: the letterbox shape is rejected however tall it has grown —
  // a real 2.84:1 window of that height would be a 32:9 display's problem.
  EXPECT_FALSE(video_persisted_window_size_is_sane(3072, 1080));
}

// The wide side of the band is bounded by height, not ratio: a Fit-mode
// window maximised on an ultrawide display is wider than any CPC image and
// still exactly the size the user chose.  Rejecting it reset the window to 1x
// on every launch and fullscreen exit.
TEST_F(WindowGeometryTest, PersistedSizeAcceptsAMaximisedUltrawideWindow) {
  EXPECT_TRUE(video_persisted_window_size_is_sane(3440, 1387));  // 21:9 - bar
  EXPECT_TRUE(video_persisted_window_size_is_sane(2560, 1080));  // 21:9
  EXPECT_TRUE(video_persisted_window_size_is_sane(5120, 1440));  // 32:9
}

TEST_F(WindowGeometryTest, PersistedSizeRejectsAWideButShortWindow) {
  // Wider than the band and no taller than the 1x image plus chrome: a
  // letterbox of some other ratio, never a maximised window.
  EXPECT_FALSE(video_persisted_window_size_is_sane(1400, 600));
  EXPECT_FALSE(video_persisted_window_size_is_sane(2000, 604));
  // One pixel taller than that floor is a window a user could have.
  EXPECT_TRUE(video_persisted_window_size_is_sane(2000, 605));
}

TEST_F(WindowGeometryTest, PersistedSizeRejectsZeroDimensions) {
  EXPECT_FALSE(video_persisted_window_size_is_sane(0, 0));
  EXPECT_FALSE(video_persisted_window_size_is_sane(768, 0));
  EXPECT_FALSE(video_persisted_window_size_is_sane(0, 576));
}

TEST_F(WindowGeometryTest, PersistedSizeAcceptsWindowsAUserWouldActuallyHave) {
  // 4:3 CRT geometry at 1x and at a hand-dragged size.
  EXPECT_TRUE(video_persisted_window_size_is_sane(768, 576));
  EXPECT_TRUE(video_persisted_window_size_is_sane(1024, 768));
  // The native doubled-scanline surface (768:540 ~ 1.42:1) and 2x of it.
  EXPECT_TRUE(video_persisted_window_size_is_sane(768, 540));
  EXPECT_TRUE(video_persisted_window_size_is_sane(1536, 1080));
  // A window widened for the DevTools side panel still sits inside the band.
  EXPECT_TRUE(video_persisted_window_size_is_sane(1600, 900));
}

// The ratio band is [0.9, 2.2] inclusive.  900/1000 and 2200/1000 are
// correctly rounded IEEE divisions, so they compare equal to the 0.9 / 2.2
// literals: the boundary itself is accepted unconditionally; one pixel past
// the wide end is accepted only because 1000 is taller than the wide-window
// height floor, and one pixel past the narrow end never is.
TEST_F(WindowGeometryTest, PersistedSizeBandIsInclusiveAtBothEnds) {
  EXPECT_TRUE(video_persisted_window_size_is_sane(900, 1000));   // == 0.9
  EXPECT_FALSE(video_persisted_window_size_is_sane(899, 1000));  // < 0.9
  EXPECT_TRUE(video_persisted_window_size_is_sane(2200, 1000));  // == 2.2
  EXPECT_TRUE(video_persisted_window_size_is_sane(2201, 1000));  // > 2.2, tall
  EXPECT_FALSE(video_persisted_window_size_is_sane(1321, 600));  // > 2.2, short
  // Portrait windows (taller than wide) are never a CPC window.
  EXPECT_FALSE(video_persisted_window_size_is_sane(576, 768));
}

// ─── Window size chosen on a windowed (re)init ───────────────────────────
//
// video_init() sizes every freshly created window through
// video_reinit_window_size(); these pin the three-way decision it makes.

TEST_F(WindowGeometryTest, ReinitInFitModeRestoresASanePersistedSize) {
  CPC.scr_scale = 0;
  CPC.scr_crt_aspect = 0;
  CPC.win_w = 1024;
  CPC.win_h = 768;
  int w = 0;
  int h = 0;
  video_reinit_window_size(w, h);
  EXPECT_EQ(w, 1024);
  EXPECT_EQ(h, 768);
}

TEST_F(WindowGeometryTest, ReinitInFitModeRejectsADegeneratePersistedSize) {
  CPC.scr_scale = 0;
  CPC.scr_crt_aspect = 0;
  CPC.win_w = 1536;  // the stale 2.84:1 value from the user's real config
  CPC.win_h = 540;
  int expected_w = 0;
  int expected_h = 0;
  video_default_window_size(expected_w, expected_h);

  int w = 0;
  int h = 0;
  video_reinit_window_size(w, h);
  EXPECT_EQ(w, expected_w);
  EXPECT_EQ(h, expected_h);
  // Belt and braces: whatever the default is, it is not the bad size.
  EXPECT_FALSE(w == 1536 && h == 540);
}

TEST_F(WindowGeometryTest, ReinitInFitModeFallsBackToDefaultWhenNothingSaved) {
  CPC.scr_scale = 0;
  CPC.scr_crt_aspect = 0;
  CPC.win_w = 0;
  CPC.win_h = 0;
  int expected_w = 0;
  int expected_h = 0;
  video_default_window_size(expected_w, expected_h);

  int w = 0;
  int h = 0;
  video_reinit_window_size(w, h);
  EXPECT_EQ(w, expected_w);
  EXPECT_EQ(h, expected_h);
  EXPECT_GT(w, 0);
  EXPECT_GT(h, 0);
}

TEST_F(WindowGeometryTest, ReinitAtAFixedScaleIgnoresThePersistedSize) {
  CPC.scr_scale = 1;
  CPC.scr_crt_aspect = 0;
  CPC.win_w = 1536;  // must be irrelevant: the scale decides
  CPC.win_h = 540;
  int expected_w = 0;
  int expected_h = 0;
  ASSERT_TRUE(video_derived_window_size(expected_w, expected_h));

  int w = 0;
  int h = 0;
  video_reinit_window_size(w, h);
  EXPECT_EQ(w, expected_w);
  EXPECT_EQ(h, expected_h);
  EXPECT_EQ(w, CPC_RENDER_WIDTH);
  EXPECT_EQ(h, CPC_VISIBLE_SCR_HEIGHT * 2);
}

class ComputeRectsTest : public testing::Test {
 public:
  // Verifies that src corresponds to the whole screen
  void ExpectFullSrc(bool half_pixels) {
    EXPECT_EQ(src.x, 0);
    EXPECT_EQ(src.y, 0);
    // Width is always CPC_RENDER_WIDTH (768) — native Mode 2 resolution
    EXPECT_EQ(src.w, CPC_RENDER_WIDTH);
    // The -4 corresponds to the 'src->h-=2*2' in video.cpp when dh <= 0
    if (half_pixels) {
      EXPECT_EQ(src.h, CPC_VISIBLE_SCR_HEIGHT - 4);
    } else {
      EXPECT_EQ(src.h, CPC_VISIBLE_SCR_HEIGHT * 2 - 4);
    }
  }

  // Verifies that a rectangle corresponds to the whole surface
  void ExpectRectMatchesSurface(SDL_Rect* r, SDL_Surface* s) {
    EXPECT_EQ(r->x, 0);
    EXPECT_EQ(r->y, 0);
    EXPECT_EQ(r->w, s->w);
    EXPECT_EQ(r->h, s->h);
  }

  // Verifies that a rectangle fits in the corresponding surface
  void ExpectRectInSurface(SDL_Rect* r, SDL_Surface* s) {
    EXPECT_LE(r->x + r->w, s->w);
    EXPECT_LE(r->y + r->h, s->h);
  }

  // Verifies that src scaled by a factor 2 fits in dst
  void ExpectSrcFitsInDst() {
    EXPECT_LE(src.w * 2, dst.w);
    EXPECT_LE(src.h * 2, dst.h);
  }

  // Some checks that should be valid for any call.
  void ExpectValid() {
    ExpectRectInSurface(&src, pub);
    ExpectRectInSurface(&dst, scaled);
    ExpectSrcFitsInDst();
  }

  SDL_Surface* CreateSurface(int width, int height) {
    // BPP shouldn't influence this method. Anyway, it is currently called only
    // by filters that support only 16bpp.
    return SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGB565);
  }

  SDL_Rect src, dst;
};

TEST_F(ComputeRectsTest, DefaultSizeHalfPixels) {
  pub = CreateSurface(CPC_RENDER_WIDTH, CPC_VISIBLE_SCR_HEIGHT);
  scaled = CreateSurface(2 * CPC_RENDER_WIDTH, 2 * CPC_VISIBLE_SCR_HEIGHT);
  Uint8 half_pixels = 1;

  compute_rects_for_tests(&src, &dst, half_pixels);

  ExpectFullSrc(half_pixels);
  ExpectRectMatchesSurface(&dst, scaled);
  ExpectValid();
}

TEST_F(ComputeRectsTest, BiggerVidHalfPixels) {
  for (auto offset : {1, 2, 3, 10, 17, 50, 100, 101}) {
    pub = CreateSurface(CPC_RENDER_WIDTH, CPC_VISIBLE_SCR_HEIGHT);
    scaled = CreateSurface(2 * CPC_RENDER_WIDTH + offset,
                           2 * CPC_VISIBLE_SCR_HEIGHT + offset);
    Uint8 half_pixels = 1;

    compute_rects_for_tests(&src, &dst, half_pixels);

    ExpectFullSrc(half_pixels);
    EXPECT_EQ(dst.x, offset / 2);
    EXPECT_EQ(dst.y, offset / 2);
    EXPECT_EQ(dst.w, 2 * CPC_RENDER_WIDTH);
    EXPECT_EQ(dst.h, 2 * CPC_VISIBLE_SCR_HEIGHT);
    ExpectValid();
  }
}

TEST_F(ComputeRectsTest, BiggerPubHalfPixels) {
  // Offsets must be small enough that scaled surface dimensions stay positive
  for (auto offset : {1, 2, 3, 10, 17, 50, 100, 101, 200, 300}) {
    pub = CreateSurface(CPC_RENDER_WIDTH, CPC_VISIBLE_SCR_HEIGHT);
    scaled = CreateSurface(2 * CPC_RENDER_WIDTH - offset,
                           2 * CPC_VISIBLE_SCR_HEIGHT - offset);
    Uint8 half_pixels = 1;

    compute_rects_for_tests(&src, &dst, half_pixels);

    EXPECT_EQ(src.x, (offset + 1) / 4);
    EXPECT_EQ(src.y, (offset + 1) / 4);
    EXPECT_EQ(src.w, CPC_RENDER_WIDTH - (offset + 1) / 2);
    // TODO: There is obviously a problem if offset/2 < 4 compared to when
    // offset = 0 (where we have -4 here)
    EXPECT_EQ(src.h, CPC_VISIBLE_SCR_HEIGHT - (offset + 1) / 2);
    ExpectRectMatchesSurface(&dst, scaled);
    ExpectValid();
  }
}

TEST_F(ComputeRectsTest, DefaultSize) {
  pub = CreateSurface(CPC_RENDER_WIDTH, 2 * CPC_VISIBLE_SCR_HEIGHT);
  scaled = CreateSurface(2 * CPC_RENDER_WIDTH, 4 * CPC_VISIBLE_SCR_HEIGHT);
  Uint8 half_pixels = 0;

  compute_rects_for_tests(&src, &dst, half_pixels);

  ExpectFullSrc(half_pixels);
  ExpectRectMatchesSurface(&dst, scaled);
  ExpectValid();
}

TEST_F(ComputeRectsTest, BiggerVid) {
  for (auto offset : {1, 2, 3, 10, 17, 50, 100, 101}) {
    pub = CreateSurface(CPC_RENDER_WIDTH, 2 * CPC_VISIBLE_SCR_HEIGHT);
    scaled = CreateSurface(2 * CPC_RENDER_WIDTH + offset,
                           4 * CPC_VISIBLE_SCR_HEIGHT + offset);
    Uint8 half_pixels = 0;

    compute_rects_for_tests(&src, &dst, half_pixels);

    ExpectFullSrc(half_pixels);
    EXPECT_EQ(dst.x, offset / 2);
    EXPECT_EQ(dst.y, offset / 2);
    EXPECT_EQ(dst.w, 2 * CPC_RENDER_WIDTH);
    EXPECT_EQ(dst.h, 4 * CPC_VISIBLE_SCR_HEIGHT);
    ExpectValid();
  }
}

TEST_F(ComputeRectsTest, BiggerPub) {
  for (auto offset : {1, 2, 3, 10, 17, 50, 100, 101, 200, 300}) {
    pub = CreateSurface(CPC_RENDER_WIDTH, 2 * CPC_VISIBLE_SCR_HEIGHT);
    scaled = CreateSurface(2 * CPC_RENDER_WIDTH - offset,
                           4 * CPC_VISIBLE_SCR_HEIGHT - offset);
    Uint8 half_pixels = 0;

    compute_rects_for_tests(&src, &dst, half_pixels);

    EXPECT_EQ(src.x, (offset + 1) / 4);
    EXPECT_EQ(src.y, (offset + 1) / 4);
    EXPECT_EQ(src.w, CPC_RENDER_WIDTH - (offset + 1) / 2);
    // TODO: There is obviously a problem if offset/2 < 4 compared to when
    // offset = 0 (where we have -4 here)
    EXPECT_EQ(src.h, 2 * CPC_VISIBLE_SCR_HEIGHT - (offset + 1) / 2);
    ExpectRectMatchesSurface(&dst, scaled);
    ExpectValid();
  }
}
}  // namespace

// ── Hidden-window fixture ───────────────────────────────────────────────────
// Stands up a hidden SDL window as mainSDLWindow for the geometry code that
// can only be reached through a live window.
extern SDL_Window* mainSDLWindow;

class HiddenWindowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Subsystem-refcounted, so a suite that holds SDL video open across its
    // tests (VideoGpuTest) is untouched by this fixture's teardown.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
      GTEST_SKIP() << "no SDL video: " << SDL_GetError();
    }
    window_ =
        SDL_CreateWindow("compute-scale-test", 800, 600, SDL_WINDOW_HIDDEN);
    if (!window_) {
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      GTEST_SKIP() << "no window (headless): " << SDL_GetError();
    }
    saved_window_ = mainSDLWindow;
    saved_cpc_ = CPC;
    mainSDLWindow = window_;
  }

  void TearDown() override {
    if (!window_) return;
    video_clear_topbar();
    video_set_bottombar(0);
    mainSDLWindow = saved_window_;
    CPC = saved_cpc_;
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }

  SDL_Window* window_ = nullptr;
  SDL_Window* saved_window_ = nullptr;
  t_CPC saved_cpc_{};
};

// ── compute_scale() placement ───────────────────────────────────────────────
// The stretch branch (scr_preserve_aspect_ratio=0) must push the image down
// past the topbar: drawn from y=0 it hid the CPC's top border under the topbar
// and left a black band of topbar_height between the image and the bottombar.
using ComputeScaleTest = HiddenWindowTest;

TEST_F(ComputeScaleTest, StretchBranchOffsetsPastTheTopbar) {
  constexpr int kTopbar = 24;
  constexpr int kBottombar = 16;
  CPC.scr_preserve_aspect_ratio = 0;
  video_set_topbar(nullptr, kTopbar);
  video_set_bottombar(kBottombar);
  int win_w = 0;
  int win_h = 0;
  SDL_GetWindowSize(mainSDLWindow, &win_w, &win_h);

  video_plugin t{};
  compute_scale_for_tests(&t, CPC_RENDER_WIDTH, CPC_VISIBLE_SCR_HEIGHT * 2);

  EXPECT_FLOAT_EQ(static_cast<float>(kTopbar), t.y_offset)
      << "the image must start below the topbar, not at y=0";
  EXPECT_FLOAT_EQ(0.f, t.x_offset);
  EXPECT_EQ(win_w, t.width);
  EXPECT_EQ(win_h - kTopbar - kBottombar, t.height)
      << "the image fills exactly the area left between the bars";
}

TEST_F(ComputeScaleTest, StretchBranchWithNoChromeStartsAtTheTop) {
  CPC.scr_preserve_aspect_ratio = 0;
  video_clear_topbar();
  video_set_bottombar(0);
  int win_w = 0;
  int win_h = 0;
  SDL_GetWindowSize(mainSDLWindow, &win_w, &win_h);

  video_plugin t{};
  compute_scale_for_tests(&t, CPC_RENDER_WIDTH, CPC_VISIBLE_SCR_HEIGHT * 2);

  EXPECT_FLOAT_EQ(0.f, t.y_offset);
  EXPECT_EQ(win_w, t.width);
  EXPECT_EQ(win_h, t.height);
}

// ── The scale picker ────────────────────────────────────────────────────────
// The Settings Video tab and the View > Scale menu resize through
// koncpc_set_scale().  It kept its own copy of the sizing formula — from the
// undoubled 270px height — after the reinit path was fixed to use the doubled
// surface, so the picker gave a half-height window while a reinit gave the
// full one.  It must produce exactly video_derived_window_size().
using ScalePickerTest = HiddenWindowTest;

TEST_F(ScalePickerTest, FixedScaleResizesToTheDerivedGeometry) {
  constexpr int kTopbar = 24;
  constexpr int kBottombar = 16;
  CPC.scr_crt_aspect = 0;
  video_set_topbar(nullptr, kTopbar);
  video_set_bottombar(kBottombar);

  koncpc_set_scale(1);
  SDL_SyncWindow(mainSDLWindow);

  EXPECT_EQ(1u, CPC.scr_scale);
  int w = 0;
  int h = 0;
  SDL_GetWindowSize(mainSDLWindow, &w, &h);
  int expected_w = 0;
  int expected_h = 0;
  ASSERT_TRUE(video_derived_window_size(expected_w, expected_h));
  EXPECT_EQ(expected_w, w);
  EXPECT_EQ(expected_h, h);
  EXPECT_EQ(CPC_RENDER_WIDTH, w);
  EXPECT_EQ((CPC_VISIBLE_SCR_HEIGHT * 2) + kTopbar + kBottombar, h)
      << "the doubled-scanline surface plus the chrome, not 270 + chrome";
}

TEST_F(ScalePickerTest, FitModeLeavesTheWindowAlone) {
  SDL_SetWindowSize(mainSDLWindow, 1000, 700);
  SDL_SyncWindow(mainSDLWindow);

  koncpc_set_scale(0);
  SDL_SyncWindow(mainSDLWindow);

  EXPECT_EQ(0u, CPC.scr_scale);
  int w = 0;
  int h = 0;
  SDL_GetWindowSize(mainSDLWindow, &w, &h);
  EXPECT_EQ(1000, w);
  EXPECT_EQ(700, h);
}

// ── Recording the windowed geometry ─────────────────────────────────────────
// video_shutdown() and saveConfiguration() record the live window into
// CPC.win_w/win_h — the only record of a Fit-mode size across a reinit — but
// never a fullscreen window's, whose size belongs to the display.  The pure
// half pins the fullscreen branch without putting a window into fullscreen.

TEST(WindowedGeometryTest, RecordsAWindowedSize) {
  unsigned int w = 1;
  unsigned int h = 2;
  EXPECT_TRUE(video_windowed_geometry(SDL_WINDOW_RESIZABLE, 1024, 768, w, h));
  EXPECT_EQ(1024u, w);
  EXPECT_EQ(768u, h);
}

TEST(WindowedGeometryTest, LeavesTheRecordAloneForAFullscreenWindow) {
  unsigned int w = 1024;
  unsigned int h = 768;
  EXPECT_FALSE(video_windowed_geometry(
      SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE, 2560, 1440, w, h));
  EXPECT_EQ(1024u, w) << "a display's size must never become the user's";
  EXPECT_EQ(768u, h);
}

TEST(WindowedGeometryTest, LeavesTheRecordAloneForADegenerateSize) {
  unsigned int w = 1024;
  unsigned int h = 768;
  EXPECT_FALSE(video_windowed_geometry(0, 0, 768, w, h));
  EXPECT_FALSE(video_windowed_geometry(0, 1024, 0, w, h));
  EXPECT_FALSE(video_windowed_geometry(0, -1, -1, w, h));
  EXPECT_EQ(1024u, w);
  EXPECT_EQ(768u, h);
}

TEST(WindowedGeometryTest, NoWindowRecordsNothing) {
  unsigned int w = 1024;
  unsigned int h = 768;
  EXPECT_FALSE(video_capture_windowed_geometry(nullptr, w, h));
  EXPECT_EQ(1024u, w);
  EXPECT_EQ(768u, h);
}

using WindowedGeometryCaptureTest = HiddenWindowTest;

TEST_F(WindowedGeometryCaptureTest, CapturesTheLiveWindowedSize) {
  SDL_SetWindowSize(mainSDLWindow, 1024, 768);
  SDL_SyncWindow(mainSDLWindow);

  unsigned int w = 0;
  unsigned int h = 0;
  EXPECT_TRUE(video_capture_windowed_geometry(mainSDLWindow, w, h));
  EXPECT_EQ(1024u, w);
  EXPECT_EQ(768u, h);
}
