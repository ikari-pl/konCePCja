// konCePCja — quit policy: when may an exit raise the "unsaved disk" dialog?
//
// Pure decisions, no SDL, so the rules are unit-testable. The host shell
// (kon_cpc_ja.cpp) feeds in the live state.
#pragma once

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

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

// KONCPC_NO_DIALOGS: unset, empty, or one of the usual off spellings ("0",
// "false", "no", "off", any case) keeps dialogs; anything else suppresses.
// Spelling `KONCPC_NO_DIALOGS=false` and getting dialogs suppressed is the
// kind of surprise that only ever shows up as a modal on someone's desktop.
inline bool koncpc_dialogs_suppressed_by_env(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  std::string lowered;
  for (const char* p = value; *p != '\0'; ++p) {
    lowered.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
  }
  return !(lowered == "0" || lowered == "false" || lowered == "no" ||
           lowered == "off");
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
//
// The whole request lives in ONE atomic word, so a post() is observed as a
// unit. Written as three separate atomics, two threads quitting at the same
// instant (IPC `quit 5` while an --exit-after deadline fires on the Z80
// thread) could have the main thread read one poster's code next to the
// other's ask flag — and an aux-thread caller that ever forgets the
// ask_if_unsaved=false argument (the parameter defaults to true) would then
// reintroduce the very dialog this policy exists to suppress.
//
// Layout of the word:
//   bit 31     pending
//   bit 30     ask_if_unsaved
//   bits 0-7   exit code
// Eight bits is the whole exit code a process can report: _exit() hands the
// low byte to the parent's wait status. So the box stores `code & 0xFF` and
// take() reports 0..255 — `quit -1` becomes 255 and `quit 256` becomes 0,
// which is exactly what the shell would have seen anyway.
class QuitMailbox {
 public:
  struct Request {
    int code;
    bool ask_if_unsaved;
  };

  void post(int code, bool ask_if_unsaved) {
    word_.store(pack(code, ask_if_unsaved), std::memory_order_release);
  }

  std::optional<Request> take() {
    const uint32_t word = word_.exchange(0, std::memory_order_acquire);
    if ((word & kPending) == 0) {
      return std::nullopt;
    }
    return Request{static_cast<int>(word & kCodeMask), (word & kAsk) != 0};
  }

 private:
  static constexpr uint32_t kPending = 1U << 31;
  static constexpr uint32_t kAsk = 1U << 30;
  static constexpr uint32_t kCodeMask = 0xFFU;

  static uint32_t pack(int code, bool ask_if_unsaved) {
    return kPending | (ask_if_unsaved ? kAsk : 0U) |
           (static_cast<uint32_t>(code) & kCodeMask);
  }

  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "the quit mailbox is posted from aux threads and must not "
                "take a lock");
  std::atomic<uint32_t> word_{0};
};
