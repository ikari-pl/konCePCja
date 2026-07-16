// Tests for IUiHost interface — default NullUiHost behavior and the
// UiHostOverride scope-guard mechanism that the modern-UI build (and
// future tests) will use to swap implementations.
//
// Phase: P1.5.1 (beads-1az).

#include "iui_host.h"

#include <SDL3/SDL_events.h>
#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

// Test double that records every interface call so we can verify the
// override mechanism actually routes through.  Counters are atomic so
// the recorder can also be used from the concurrent-access test —
// `last_toasts` is NOT thread-safe and only the single-threaded tests
// touch it.
class RecordingHost final : public IUiHost {
 public:
  std::atomic<int> events_processed{0};
  std::atomic<int> toasts_emitted{0};
  bool want_kbd = false;
  bool want_mouse = false;
  bool kbd_ui_open = false;

  // Only safe to read/write from a single thread.  The concurrent
  // test avoids touching this.
  std::vector<std::pair<UiToastLevel, std::string>> last_toasts;
  bool record_toast_payloads = true;

  void process_event(const SDL_Event& /*ev*/) override {
    events_processed.fetch_add(1, std::memory_order_relaxed);
  }
  bool wants_capture_keyboard() const override { return want_kbd; }
  bool wants_capture_mouse() const override { return want_mouse; }
  bool any_keyboard_ui_active() const override { return kbd_ui_open; }
  int topbar_height_returned = 0;
  int topbar_height() const override { return topbar_height_returned; }
  void toast(UiToastLevel level, const std::string& message) override {
    if (record_toast_payloads) {
      last_toasts.emplace_back(level, message);
    }
    toasts_emitted.fetch_add(1, std::memory_order_relaxed);
  }
};

}  // namespace

// -- NullUiHost: every method is safely a no-op or returns a sane default.

TEST(IUiHostTest, NullHostIsDefault) {
  // Without any override installed, ui_host() returns the null host.
  EXPECT_FALSE(ui_host().wants_capture_keyboard());
  EXPECT_FALSE(ui_host().wants_capture_mouse());
  EXPECT_FALSE(ui_host().any_keyboard_ui_active());
  // NOTE: not asserting topbar_height() == 0 here because modern-UI test
  // builds install ImGuiUiHost via static-init, and its topbar_height()
  // returns the live ImGui menubar measurement (≥19px default).  See the
  // separate NullHostTopbarHeightIsZero test which forces NullUiHost.
}

TEST(IUiHostTest, NullHostTopbarHeightIsZero) {
  // Explicit null-host scope: headless mode has no top bar, so the
  // CPC viewport must span the full window.  Tests should not depend
  // on what static-init left as the process-wide default.
  UiHostOverride scope(nullptr);  // nullptr → null host
  EXPECT_EQ(0, ui_host().topbar_height());
}

TEST(IUiHostTest, OverrideReportsTopbarHeight) {
  RecordingHost recorder;
  recorder.topbar_height_returned = 42;
  UiHostOverride scope(&recorder);
  EXPECT_EQ(42, ui_host().topbar_height());
}

TEST(IUiHostTest, NullHostSwallowsEventsAndToasts) {
  // Synthesize a dummy SDL_Event — we only care that process_event
  // doesn't crash on the null host.
  SDL_Event ev{};
  ev.type = SDL_EVENT_QUIT;
  ui_host().process_event(ev);

  // Toast on the null host goes to stderr; we don't capture it here,
  // just verify it returns cleanly.
  ui_host().toast(UiToastLevel::Info, "smoke test");
  ui_host().toast(UiToastLevel::Error, "another smoke test");
  SUCCEED();
}

// -- UiHostOverride: install / restore semantics.

TEST(IUiHostTest, OverrideInstallsAndRestores) {
  RecordingHost test_host;

  // Sanity: before override, the default host doesn't record events.
  SDL_Event ev{};
  ev.type = SDL_EVENT_KEY_DOWN;
  ui_host().process_event(ev);
  EXPECT_EQ(test_host.events_processed, 0);

  {
    UiHostOverride scope(&test_host);

    // Inside scope: events should route to the recording host.
    ui_host().process_event(ev);
    EXPECT_EQ(test_host.events_processed, 1);

    // Query flags route too.
    test_host.want_kbd = true;
    EXPECT_TRUE(ui_host().wants_capture_keyboard());

    // Toast routes.
    ui_host().toast(UiToastLevel::Error, "from inside scope");
    EXPECT_EQ(test_host.toasts_emitted, 1);
    ASSERT_EQ(test_host.last_toasts.size(), 1u);
    EXPECT_EQ(test_host.last_toasts[0].first, UiToastLevel::Error);
    EXPECT_EQ(test_host.last_toasts[0].second, "from inside scope");
  }

  // After scope ends, the host is back to the default (null host).
  EXPECT_FALSE(ui_host().wants_capture_keyboard());
  ui_host().process_event(ev);
  // Counter on test_host should not have advanced now that it's not
  // installed any more.
  EXPECT_EQ(test_host.events_processed, 1);
}

TEST(IUiHostTest, OverrideNestsCorrectly) {
  RecordingHost outer;
  RecordingHost inner;

  UiHostOverride outer_scope(&outer);
  SDL_Event ev{};
  ui_host().process_event(ev);
  EXPECT_EQ(outer.events_processed, 1);
  EXPECT_EQ(inner.events_processed, 0);

  {
    UiHostOverride inner_scope(&inner);
    ui_host().process_event(ev);
    EXPECT_EQ(outer.events_processed, 1);
    EXPECT_EQ(inner.events_processed, 1);
  }

  // Inner scope ended — back to outer.
  ui_host().process_event(ev);
  EXPECT_EQ(outer.events_processed, 2);
  EXPECT_EQ(inner.events_processed, 1);
}

// -- Concurrency: addresses Gemini review on PR #124.  The host pointer
//    is accessed from the main thread (UI, init) AND the Z80 thread
//    (toasts from emulation side-effects), so g_current_host must be
//    atomic.  This test spawns N threads each calling ui_host() in a
//    tight loop while another thread installs/restores overrides; the
//    invariant we check is "no crash, no TSan complaint, no observable
//    null deref".  Each iteration just hits the methods that the real
//    callers will hit (toast + wants_capture_*); the recording host
//    counter under contention isn't deterministic, so we don't assert
//    on it — just that the program completes cleanly.

TEST(IUiHostTest, ConcurrentAccessIsSafe) {
  RecordingHost recorder;
  recorder.record_toast_payloads = false;  // vector is not thread-safe
  UiHostOverride scope(&recorder);

  constexpr int kThreads = 4;
  constexpr int kIters = 500;
  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      SDL_Event ev{};
      for (int i = 0; i < kIters; ++i) {
        ui_host().toast(UiToastLevel::Info, "from-thread");
        (void)ui_host().wants_capture_keyboard();
        ui_host().process_event(ev);
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (auto& th : threads) th.join();

  // The atomic counters must have caught every call exactly.  If
  // g_current_host weren't atomic, TSan would fire here on the
  // concurrent loads inside ui_host().
  EXPECT_EQ(recorder.events_processed.load(), kThreads * kIters);
  EXPECT_EQ(recorder.toasts_emitted.load(), kThreads * kIters);
}

TEST(IUiHostTest, OverrideWithNullptrFallsBackToNullHost) {
  // Passing nullptr explicitly means "use the null host as the active
  // one during this scope" — useful for tests that want to verify
  // headless behavior even in a process where another host was
  // previously installed.
  RecordingHost outer;
  UiHostOverride outer_scope(&outer);

  {
    UiHostOverride null_scope(nullptr);
    SDL_Event ev{};
    ui_host().process_event(ev);
    EXPECT_EQ(outer.events_processed, 0);  // null host swallows it
  }

  // Back to outer.
  SDL_Event ev{};
  ui_host().process_event(ev);
  EXPECT_EQ(outer.events_processed, 1);
}
