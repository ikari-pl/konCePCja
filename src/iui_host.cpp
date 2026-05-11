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

// Process-wide null-host default.  Function-local static so that the
// first caller's thread-safe init (guaranteed by C++11 [stmt.dcl]/4)
// is the moment the singleton becomes valid — no dependence on
// translation-unit order.
NullUiHost& null_host_singleton() {
    static NullUiHost instance;
    return instance;
}

// Currently installed host.  Returned by a function-local static
// accessor to dodge the static-initialization-order fiasco — if a TU
// somewhere (e.g. imgui_ui_host.cpp's AutoInstaller) calls
// install_ui_host() from its file-scope constructor, that constructor
// can run before iui_host.cpp's namespace-scope dynamic init would
// have, and a plain `std::atomic<IUiHost*> g_current_host{nullptr};`
// could be re-zeroed afterwards, wiping the install.  Wrapping in a
// function-local static defers initialisation to the first call,
// which is guaranteed thread-safe and ordered.
//
// Still atomic because it's read/written from the main (render)
// thread AND the Z80 thread — the latter can emit toasts from
// emulation side-effects (e.g. tape autoload errors).
std::atomic<IUiHost*>& host_atomic() {
    static std::atomic<IUiHost*> instance{nullptr};
    return instance;
}

} // namespace

IUiHost& ui_host() {
    auto& atom = host_atomic();
    IUiHost* h = atom.load(std::memory_order_acquire);
    if (h) {
        return *h;
    }
    // Lazy init: try to publish the null host as the default.  CAS so
    // concurrent first-callers race-free converge on the same pointer.
    IUiHost* expected = nullptr;
    IUiHost* desired  = &null_host_singleton();
    if (atom.compare_exchange_strong(expected, desired,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
        return *desired;
    }
    // Lost the race — `expected` now holds the winner.
    return *expected;
}

IUiHost* install_ui_host(IUiHost* host) {
    IUiHost* prev = host_atomic().exchange(host ? host : &null_host_singleton(),
                                           std::memory_order_acq_rel);
    return prev ? prev : &null_host_singleton();
}

UiHostOverride::UiHostOverride(IUiHost* test_host) {
    auto& atom = host_atomic();
    // Snapshot the current host before installing the override.  If
    // nobody has called ui_host() yet, fall back to the null host so
    // the destructor restores a sensible value rather than nullptr.
    IUiHost* prev = atom.load(std::memory_order_acquire);
    if (!prev) {
        prev = &null_host_singleton();
    }
    previous_ = prev;
    atom.store(test_host ? test_host : &null_host_singleton(),
               std::memory_order_release);
}

UiHostOverride::~UiHostOverride() {
    host_atomic().store(previous_, std::memory_order_release);
}
