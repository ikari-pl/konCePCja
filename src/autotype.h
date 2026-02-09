#pragma once

#include <string>
#include <deque>
#include <cstdint>
#include <functional>

struct AutoTypeAction {
  enum Type { CHAR_PRESS_RELEASE, KEY_PRESS, KEY_RELEASE, PAUSE };
  Type type;
  uint16_t cpc_key;       // CPC_KEYS enum value
  int pause_frames;       // for PAUSE type
};

// Callback type for applying a key press/release.
// Parameters: cpc_key (CPC_KEYS enum value), pressed (true=press, false=release)
using AutoTypeKeyFunc = std::function<void(uint16_t cpc_key, bool pressed)>;

class AutoTypeQueue {
public:
  // Parse WinAPE ~KEY~ syntax into action queue.
  // Returns empty string on success, error message on failure.
  std::string enqueue(const std::string& text);

  // Called once per frame from main loop. Applies next action(s)
  // using the provided key function for matrix manipulation.
  // Returns true if there are more actions pending.
  bool tick(const AutoTypeKeyFunc& apply_key);

  // Status
  bool is_active() const;
  size_t remaining() const;

  // Clear queue
  void clear();

  // Access internal queue for testing
  const std::deque<AutoTypeAction>& actions() const { return queue_; }

private:
  std::deque<AutoTypeAction> queue_;
  int pause_counter_ = 0;
  // For CHAR_PRESS_RELEASE: press on one frame, release on next
  bool awaiting_release_ = false;
  uint16_t pending_release_key_ = 0;
};

extern AutoTypeQueue g_autotype_queue;
