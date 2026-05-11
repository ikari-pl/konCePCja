// Tests for IUiHost interface — default NullUiHost behavior and the
// UiHostOverride scope-guard mechanism that the modern-UI build (and
// future tests) will use to swap implementations.
//
// Phase: P1.5.1 (beads-1az).

#include "iui_host.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>

namespace {

// Test double that records every interface call so we can verify
// the override mechanism actually routes through.
class RecordingHost final : public IUiHost {
  public:
    int events_processed = 0;
    int toasts_emitted   = 0;
    bool want_kbd     = false;
    bool want_mouse   = false;
    bool kbd_ui_open  = false;

    std::vector<std::pair<UiToastLevel, std::string>> last_toasts;

    void process_event(const SDL_Event& /*ev*/) override { ++events_processed; }
    bool wants_capture_keyboard() const override { return want_kbd; }
    bool wants_capture_mouse()    const override { return want_mouse; }
    bool any_keyboard_ui_active() const override { return kbd_ui_open; }
    void toast(UiToastLevel level, const std::string& message) override {
        last_toasts.emplace_back(level, message);
        ++toasts_emitted;
    }
};

} // namespace

// -- NullUiHost: every method is safely a no-op or returns a sane default.

TEST(IUiHostTest, NullHostIsDefault) {
    // Without any override installed, ui_host() returns the null host.
    EXPECT_FALSE(ui_host().wants_capture_keyboard());
    EXPECT_FALSE(ui_host().wants_capture_mouse());
    EXPECT_FALSE(ui_host().any_keyboard_ui_active());
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
        ui_host().toast(UiToastLevel::Warning, "from inside scope");
        EXPECT_EQ(test_host.toasts_emitted, 1);
        ASSERT_EQ(test_host.last_toasts.size(), 1u);
        EXPECT_EQ(test_host.last_toasts[0].first, UiToastLevel::Warning);
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
        EXPECT_EQ(outer.events_processed, 0); // null host swallows it
    }

    // Back to outer.
    SDL_Event ev{};
    ui_host().process_event(ev);
    EXPECT_EQ(outer.events_processed, 1);
}
