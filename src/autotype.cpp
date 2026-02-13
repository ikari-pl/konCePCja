#include "autotype.h"
#include "cpc_key_tables.h"
#include <cctype>
#include <cstdlib>

AutoTypeQueue g_autotype_queue;

// Resolve a key name (case-insensitive) to CPC_KEYS enum value.
// Returns -1 if not found.
static int resolve_key_name(const std::string& name) {
  std::string upper = name;
  for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  auto it = cpc_key_names().find(upper);
  if (it != cpc_key_names().end()) return static_cast<int>(it->second);
  // Single char: try char map
  if (name.size() == 1) {
    auto cit = cpc_char_to_key().find(name[0]);
    if (cit != cpc_char_to_key().end()) return static_cast<int>(cit->second);
  }
  return -1;
}

std::string AutoTypeQueue::enqueue(const std::string& text) {
  std::deque<AutoTypeAction> parsed;

  for (size_t i = 0; i < text.size(); ) {
    if (text[i] == '~') {
      // Check for literal tilde: ~~
      if (i + 1 < text.size() && text[i + 1] == '~') {
        // Literal tilde character - but tilde is not in the CPC char map,
        // so we skip it (same as unmappable chars in input type)
        i += 2;
        continue;
      }

      // Find closing ~
      size_t close = text.find('~', i + 1);
      if (close == std::string::npos) {
        return "unclosed ~ at position " + std::to_string(i);
      }

      std::string tag = text.substr(i + 1, close - i - 1);
      if (tag.empty()) {
        return "empty ~~ tag at position " + std::to_string(i);
      }

      // Check for PAUSE
      if (tag.size() > 6 && tag.substr(0, 6) == "PAUSE ") {
        std::string num_str = tag.substr(6);
        char* endptr = nullptr;
        long frames = std::strtol(num_str.c_str(), &endptr, 10);
        if (endptr == num_str.c_str() || *endptr != '\0' || frames < 1) {
          return "bad PAUSE value: " + tag;
        }
        AutoTypeAction a;
        a.type = AutoTypeAction::PAUSE;
        a.cpc_key = 0;
        a.pause_frames = static_cast<int>(frames);
        parsed.push_back(a);
        i = close + 1;
        continue;
      }

      // Check for key hold: ~+KEY~ or ~-KEY~
      if (tag.size() >= 2 && (tag[0] == '+' || tag[0] == '-')) {
        bool press = (tag[0] == '+');
        std::string key_name = tag.substr(1);
        int key = resolve_key_name(key_name);
        if (key < 0) {
          return "unknown key: " + key_name;
        }
        AutoTypeAction a;
        a.type = press ? AutoTypeAction::KEY_PRESS : AutoTypeAction::KEY_RELEASE;
        a.cpc_key = static_cast<uint16_t>(key);
        a.pause_frames = 0;
        parsed.push_back(a);
        i = close + 1;
        continue;
      }

      // Regular special key: ~RETURN~, ~SPACE~, etc.
      int key = resolve_key_name(tag);
      if (key < 0) {
        return "unknown key: " + tag;
      }
      AutoTypeAction a;
      a.type = AutoTypeAction::CHAR_PRESS_RELEASE;
      a.cpc_key = static_cast<uint16_t>(key);
      a.pause_frames = 0;
      parsed.push_back(a);
      i = close + 1;
      continue;
    }

    // Regular character
    char ch = text[i];
    auto cit = cpc_char_to_key().find(ch);
    if (cit == cpc_char_to_key().end()) {
      // Skip unmappable characters (consistent with input type behavior)
      i++;
      continue;
    }
    AutoTypeAction a;
    a.type = AutoTypeAction::CHAR_PRESS_RELEASE;
    a.cpc_key = static_cast<uint16_t>(cit->second);
    a.pause_frames = 0;
    parsed.push_back(a);
    i++;
  }

  // Append parsed actions to existing queue
  for (auto& a : parsed) {
    queue_.push_back(a);
  }
  return "";
}

bool AutoTypeQueue::tick(const AutoTypeKeyFunc& apply_key) {
  // Handle pending release from previous CHAR_PRESS_RELEASE
  if (awaiting_release_) {
    apply_key(pending_release_key_, false);
    awaiting_release_ = false;
    pending_release_key_ = 0;
    return !queue_.empty();
  }

  // Handle active pause
  if (pause_counter_ > 0) {
    pause_counter_--;
    return true;
  }

  if (queue_.empty()) return false;

  AutoTypeAction action = queue_.front();
  queue_.pop_front();

  switch (action.type) {
    case AutoTypeAction::CHAR_PRESS_RELEASE:
      // Press key this frame, release next frame
      apply_key(action.cpc_key, true);
      awaiting_release_ = true;
      pending_release_key_ = action.cpc_key;
      return true;

    case AutoTypeAction::KEY_PRESS:
      apply_key(action.cpc_key, true);
      return !queue_.empty() || awaiting_release_;

    case AutoTypeAction::KEY_RELEASE:
      apply_key(action.cpc_key, false);
      return !queue_.empty() || awaiting_release_;

    case AutoTypeAction::PAUSE:
      pause_counter_ = action.pause_frames - 1;  // -1 because this frame counts
      return true;
  }

  return false;
}

bool AutoTypeQueue::is_active() const {
  return !queue_.empty() || awaiting_release_ || pause_counter_ > 0;
}

size_t AutoTypeQueue::remaining() const {
  return queue_.size();
}

void AutoTypeQueue::clear() {
  queue_.clear();
  pause_counter_ = 0;
  awaiting_release_ = false;
  pending_release_key_ = 0;
}
