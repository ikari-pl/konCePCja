// konCePCja — quit policy: when may an exit raise the "unsaved disk" dialog?
//
// Pure decisions, no SDL, so the rules are unit-testable. The host shell
// (kon_cpc_ja.cpp) feeds in the live state.
#pragma once

#include <atomic>
#include <cstring>
#include <optional>

// A quit reaches cleanExit() either from a user gesture (window close, F10,
// the native Quit item) or programmatically (IPC `quit`, SIGTERM/SIGINT,
// -E/--exit-after, -B/--exit-on-break). Only a gesture may block on a
// native modal: nobody is there to answer one raised by a script, and a
// test harness that terminates the emulator with a dirty disk would hang
// on it — or, worse, pop it onto the maintainer's desktop.
//
// KONCPC_NO_DIALOGS=1 in the environment suppresses the dialog for every
// path. Harnesses set it so no test can ever raise a modal.
inline bool koncpc_quit_should_prompt(bool headless, bool ask_if_unsaved,
                                      bool drive_altered,
                                      bool dialogs_suppressed) {
  return !headless && !dialogs_suppressed && ask_if_unsaved && drive_altered;
}

// KONCPC_NO_DIALOGS: unset or "0" keeps dialogs; anything else suppresses.
inline bool koncpc_dialogs_suppressed_by_env(const char* value) {
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

// KONCPC_EXIT is both a gesture (F10, the ImGui menu, the native Quit item —
// all dispatched on the main thread) and a script step (`-a KONCPC_EXIT`,
// replayed by the autotype queue on the Z80 thread). Only the gesture asks.
inline bool koncpc_exit_action_asks(bool on_main_thread) {
  return on_main_thread;
}

// A signalled exit reports the shell convention 128+signo (130 for SIGINT,
// 143 for SIGTERM) rather than a success code — and, being non-zero, it also
// skips the settings save that only a clean exit performs.
inline int koncpc_signal_exit_code(int sig) { return 128 + sig; }

// Carries a quit requested off the main thread (IPC, Z80, HTTP, telnet) to the
// main thread, which alone may prompt and tear down. The main thread takes it
// from SDL_EVENT_QUIT in the GUI and from the loop top in headless mode, where
// no SDL events are polled. Taking empties the box, so a later QUIT that SDL
// raises on its own reads "nothing posted" instead of a stale request.
class QuitMailbox {
 public:
  struct Request {
    int code;
    bool ask_if_unsaved;
  };

  void post(int code, bool ask_if_unsaved) {
    code_.store(code, std::memory_order_relaxed);
    ask_.store(ask_if_unsaved, std::memory_order_relaxed);
    pending_.store(true, std::memory_order_release);
  }

  std::optional<Request> take() {
    if (!pending_.exchange(false, std::memory_order_acquire)) {
      return std::nullopt;
    }
    return Request{code_.load(std::memory_order_relaxed),
                   ask_.load(std::memory_order_relaxed)};
  }

 private:
  std::atomic<int> code_{0};
  std::atomic<bool> ask_{true};
  std::atomic<bool> pending_{false};
};
