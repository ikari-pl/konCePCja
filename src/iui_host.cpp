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

#include <atomic>
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

// Currently installed host.  Atomic because it's read/written from the
// main (render) thread AND the Z80 thread — the latter can emit toasts
// from emulation side-effects (e.g. tape autoload errors).  Starts as
// nullptr and is lazily initialised to the null host on first ui_host()
// call.  After that, swaps go through UiHostOverride which uses atomic
// load/store as well.
std::atomic<IUiHost*> g_current_host{nullptr};

} // namespace

IUiHost& ui_host() {
    IUiHost* h = g_current_host.load(std::memory_order_acquire);
    if (h) {
        return *h;
    }
    // Lazy init: try to publish the null host as the default.  CAS so
    // concurrent first-callers race-free converge on the same pointer.
    IUiHost* expected = nullptr;
    IUiHost* desired  = &null_host_singleton();
    if (g_current_host.compare_exchange_strong(expected, desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        return *desired;
    }
    // Lost the race — `expected` now holds the winner.
    return *expected;
}

IUiHost* install_ui_host(IUiHost* host) {
    IUiHost* prev = g_current_host.exchange(host ? host : &null_host_singleton(),
                                            std::memory_order_acq_rel);
    return prev ? prev : &null_host_singleton();
}

UiHostOverride::UiHostOverride(IUiHost* test_host) {
    // Snapshot the current host before installing the override.  If
    // nobody has called ui_host() yet, fall back to the null host so
    // the destructor restores a sensible value rather than nullptr.
    IUiHost* prev = g_current_host.load(std::memory_order_acquire);
    if (!prev) {
        prev = &null_host_singleton();
    }
    previous_ = prev;
    g_current_host.store(test_host ? test_host : &null_host_singleton(),
                         std::memory_order_release);
}

UiHostOverride::~UiHostOverride() {
    g_current_host.store(previous_, std::memory_order_release);
}
