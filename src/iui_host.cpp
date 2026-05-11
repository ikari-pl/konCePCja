// IUiHost implementation: NullUiHost (default) + the override mechanism.
//
// See iui_host.h for the architectural rationale.  This file is part of
// every build — modern and headless alike.  The modern-UI host is in a
// separate translation unit (imgui_ui_host.cpp) that only links into the
// MODERN_UI build.
//
// Phase: P1.5.1 (beads-1az).  First sub-PR — additive only, no existing
// callers rewired yet.

#include "iui_host.h"

#include <cstdio>

namespace {

// Headless / pre-init / test-default UI host.  Every method is a no-op
// except toast(), which logs to stderr so headless diagnostics still
// reach the user.
class NullUiHost final : public IUiHost {
  public:
    void process_event(const SDL_Event& /*ev*/) override {}

    bool wants_capture_keyboard() const override { return false; }
    bool wants_capture_mouse()    const override { return false; }
    bool any_keyboard_ui_active() const override { return false; }

    void toast(UiToastLevel level, const std::string& message) override {
        const char* tag = "info";
        switch (level) {
            case UiToastLevel::Info:    tag = "info";    break;
            case UiToastLevel::Success: tag = "success"; break;
            case UiToastLevel::Warning: tag = "warning"; break;
            case UiToastLevel::Error:   tag = "error";   break;
        }
        // stderr is intentional — toasts in headless mode are diagnostics,
        // not user-visible UI.  Process supervisors / CI logs capture them.
        std::fprintf(stderr, "[toast/%s] %s\n", tag, message.c_str());
    }
};

// Process-wide default.  Constructed on first use (avoids static-init
// ordering pitfalls — `ui_host()` may be called from any TU's init).
NullUiHost& null_host_singleton() {
    static NullUiHost instance;
    return instance;
}

// Currently installed host.  Starts as the null host; the modern-UI
// startup path will swap in the real one via UiHostOverride (or a
// follow-up install_ui_host() helper).  Never null after first call
// to ui_host().
IUiHost* g_current_host = nullptr;

} // namespace

IUiHost& ui_host() {
    if (!g_current_host) {
        g_current_host = &null_host_singleton();
    }
    return *g_current_host;
}

UiHostOverride::UiHostOverride(IUiHost* test_host)
    : previous_(g_current_host ? g_current_host : &null_host_singleton()) {
    g_current_host = test_host ? test_host : &null_host_singleton();
}

UiHostOverride::~UiHostOverride() {
    g_current_host = previous_;
}
