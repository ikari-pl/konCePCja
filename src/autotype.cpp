#include "autotype.h"
#include "keyboard.h"
#include <map>
#include <cctype>
#include <cstdlib>

AutoTypeQueue g_autotype_queue;

// Key name map: matches ipc_key_names from koncepcja_ipc_server.cpp
static const std::map<std::string, CPC_KEYS> autotype_key_names = {
  {"ESC", CPC_ESC}, {"RETURN", CPC_RETURN}, {"ENTER", CPC_RETURN},
  {"SPACE", CPC_SPACE}, {"TAB", CPC_TAB}, {"DEL", CPC_DEL},
  {"COPY", CPC_COPY}, {"CONTROL", CPC_CONTROL}, {"CTRL", CPC_CONTROL},
  {"SHIFT", CPC_LSHIFT}, {"LSHIFT", CPC_LSHIFT}, {"RSHIFT", CPC_RSHIFT},
  {"UP", CPC_CUR_UP}, {"DOWN", CPC_CUR_DOWN},
  {"LEFT", CPC_CUR_LEFT}, {"RIGHT", CPC_CUR_RIGHT},
  {"CLR", CPC_CLR},
  {"F0", CPC_F0}, {"F1", CPC_F1}, {"F2", CPC_F2}, {"F3", CPC_F3},
  {"F4", CPC_F4}, {"F5", CPC_F5}, {"F6", CPC_F6}, {"F7", CPC_F7},
  {"F8", CPC_F8}, {"F9", CPC_F9},
  // Joystick
  {"J0_UP", CPC_J0_UP}, {"J0_DOWN", CPC_J0_DOWN},
  {"J0_LEFT", CPC_J0_LEFT}, {"J0_RIGHT", CPC_J0_RIGHT},
  {"J0_FIRE1", CPC_J0_FIRE1}, {"J0_FIRE2", CPC_J0_FIRE2},
  {"J1_UP", CPC_J1_UP}, {"J1_DOWN", CPC_J1_DOWN},
  {"J1_LEFT", CPC_J1_LEFT}, {"J1_RIGHT", CPC_J1_RIGHT},
  {"J1_FIRE1", CPC_J1_FIRE1}, {"J1_FIRE2", CPC_J1_FIRE2},
};

// Char-to-key map: matches ipc_char_to_key from koncepcja_ipc_server.cpp
static const std::map<char, CPC_KEYS> autotype_char_to_key = {
  {'a', CPC_a}, {'b', CPC_b}, {'c', CPC_c}, {'d', CPC_d}, {'e', CPC_e},
  {'f', CPC_f}, {'g', CPC_g}, {'h', CPC_h}, {'i', CPC_i}, {'j', CPC_j},
  {'k', CPC_k}, {'l', CPC_l}, {'m', CPC_m}, {'n', CPC_n}, {'o', CPC_o},
  {'p', CPC_p}, {'q', CPC_q}, {'r', CPC_r}, {'s', CPC_s}, {'t', CPC_t},
  {'u', CPC_u}, {'v', CPC_v}, {'w', CPC_w}, {'x', CPC_x}, {'y', CPC_y},
  {'z', CPC_z},
  {'A', CPC_A}, {'B', CPC_B}, {'C', CPC_C}, {'D', CPC_D}, {'E', CPC_E},
  {'F', CPC_F}, {'G', CPC_G}, {'H', CPC_H}, {'I', CPC_I}, {'J', CPC_J},
  {'K', CPC_K}, {'L', CPC_L}, {'M', CPC_M}, {'N', CPC_N}, {'O', CPC_O},
  {'P', CPC_P}, {'Q', CPC_Q}, {'R', CPC_R}, {'S', CPC_S}, {'T', CPC_T},
  {'U', CPC_U}, {'V', CPC_V}, {'W', CPC_W}, {'X', CPC_X}, {'Y', CPC_Y},
  {'Z', CPC_Z},
  {'0', CPC_0}, {'1', CPC_1}, {'2', CPC_2}, {'3', CPC_3}, {'4', CPC_4},
  {'5', CPC_5}, {'6', CPC_6}, {'7', CPC_7}, {'8', CPC_8}, {'9', CPC_9},
  {' ', CPC_SPACE}, {'\n', CPC_RETURN}, {'\r', CPC_RETURN},
  {'.', CPC_PERIOD}, {',', CPC_COMMA}, {';', CPC_SEMICOLON},
  {':', CPC_COLON}, {'-', CPC_MINUS}, {'+', CPC_PLUS},
  {'/', CPC_SLASH}, {'*', CPC_ASTERISK}, {'=', CPC_EQUAL},
  {'(', CPC_LEFTPAREN}, {')', CPC_RIGHTPAREN},
  {'[', CPC_LBRACKET}, {']', CPC_RBRACKET},
  {'{', CPC_LCBRACE}, {'}', CPC_RCBRACE},
  {'<', CPC_LESS}, {'>', CPC_GREATER},
  {'?', CPC_QUESTION}, {'!', CPC_EXCLAMATN},
  {'@', CPC_AT}, {'#', CPC_HASH}, {'$', CPC_DOLLAR},
  {'%', CPC_PERCENT}, {'^', CPC_POWER}, {'&', CPC_AMPERSAND},
  {'|', CPC_PIPE}, {'\\', CPC_BACKSLASH},
  {'"', CPC_DBLQUOTE}, {'\'', CPC_QUOTE},
  {'`', CPC_BACKQUOTE}, {'_', CPC_UNDERSCORE},
};

// Resolve a key name (case-insensitive) to CPC_KEYS enum value.
// Returns -1 if not found.
static int resolve_key_name(const std::string& name) {
  std::string upper = name;
  for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  auto it = autotype_key_names.find(upper);
  if (it != autotype_key_names.end()) return static_cast<int>(it->second);
  // Single char: try char map
  if (name.size() == 1) {
    auto cit = autotype_char_to_key.find(name[0]);
    if (cit != autotype_char_to_key.end()) return static_cast<int>(cit->second);
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
    auto cit = autotype_char_to_key.find(ch);
    if (cit == autotype_char_to_key.end()) {
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
