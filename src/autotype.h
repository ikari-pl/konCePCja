#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

struct AutoTypeAction {
  enum Type : std::uint8_t { CHAR_PRESS_RELEASE, KEY_PRESS, KEY_RELEASE, PAUSE, COMMAND };
  Type type;
  uint16_t cpc_key;  // CPC_KEYS enum value; for COMMAND, the KONCPC_* code
  int pause_frames;  // for PAUSE type
};

// Callback type for applying a key press/release.
// Parameters: cpc_key (CPC_KEYS enum value), pressed (true=press,
// false=release)
using AutoTypeKeyFunc = std::function<void(uint16_t cpc_key, bool pressed)>;

// Callback type for executing an emulator command (KONCPC_* code).  Return true
// to PAUSE autotype until resume() is called (used by KONCPC_WAITBREAK, which
// blocks the queue until the next CPU breakpoint fires).
using AutoTypeCmdFunc = std::function<bool(uint16_t koncpc_cmd)>;

class AutoTypeQueue {
 public:
  // Parse WinAPE ~KEY~ syntax into action queue.
  // Returns empty string on success, error message on failure.
  std::string enqueue(const std::string& text);

  // Parse the legacy autocmd encoding into the action queue: plain chars type
  // themselves, '\a'+char is a CPC special key, '\f'+char is an emulator
  // command (KONCPC_*).  This is the encoding produced by the -a autocmd
  // assembler and used by clipboard paste / IPC.  Never fails (unmappable chars
  // are skipped, matching the old StringToEvents behaviour).
  void enqueue_legacy(const std::string& text);

  // Called once per keyboard scan (or timeout). Applies the next action using
  // apply_key for keystrokes and apply_cmd for KONCPC_* commands.  Returns true
  // if there are more actions pending (or the queue is blocked on a command).
  bool tick(const AutoTypeKeyFunc& apply_key,
            const AutoTypeCmdFunc& apply_cmd = {});

  // Release a command-induced block (e.g. KONCPC_WAITBREAK once the breakpoint
  // fires) so tick() resumes draining the queue.
  void resume();

  // Status
  bool is_active() const;
  size_t remaining() const;

  // Clear queue
  void clear();

  // Access internal queue for testing — returns a copy for thread safety
  std::deque<AutoTypeAction> actions() const {
    std::scoped_lock const lock(mutex_);
    return queue_;
  }

  // Thread safety: enqueue() may be called from IPC, HTTP, or telnet threads.
  // tick() runs on the main thread. The mutex serializes all access.
  mutable std::mutex mutex_;

 private:
  std::deque<AutoTypeAction> queue_;
  int pause_counter_ = 0;
  // For CHAR_PRESS_RELEASE: press on one frame, release on next
  bool awaiting_release_ = false;
  uint16_t pending_release_key_ = 0;
  bool inter_char_pause_ = false;
  // Set by a COMMAND whose apply_cmd returned true (e.g. KONCPC_WAITBREAK);
  // tick() stops draining until resume() clears it.
  bool blocked_ = false;
};

extern AutoTypeQueue g_autotype_queue;
