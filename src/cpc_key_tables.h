#pragma once

#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "keyboard.h"

// Shared key-name and char-to-key lookup tables.
// Used by both the IPC server and the autotype subsystem.

inline const std::map<std::string, CPC_KEYS>& cpc_key_names() {
  static const std::map<std::string, CPC_KEYS> tbl = {
      {"ESC", CPC_ESC},
      {"RETURN", CPC_RETURN},
      {"ENTER", CPC_RETURN},
      {"SPACE", CPC_SPACE},
      {"TAB", CPC_TAB},
      {"DEL", CPC_DEL},
      {"COPY", CPC_COPY},
      {"CONTROL", CPC_CONTROL},
      {"CTRL", CPC_CONTROL},
      {"SHIFT", CPC_LSHIFT},
      {"LSHIFT", CPC_LSHIFT},
      {"RSHIFT", CPC_RSHIFT},
      {"UP", CPC_CUR_UP},
      {"DOWN", CPC_CUR_DOWN},
      {"LEFT", CPC_CUR_LEFT},
      {"RIGHT", CPC_CUR_RIGHT},
      {"CLR", CPC_CLR},
      {"F0", CPC_F0},
      {"F1", CPC_F1},
      {"F2", CPC_F2},
      {"F3", CPC_F3},
      {"F4", CPC_F4},
      {"F5", CPC_F5},
      {"F6", CPC_F6},
      {"F7", CPC_F7},
      {"F8", CPC_F8},
      {"F9", CPC_F9},
      // Joystick
      {"J0_UP", CPC_J0_UP},
      {"J0_DOWN", CPC_J0_DOWN},
      {"J0_LEFT", CPC_J0_LEFT},
      {"J0_RIGHT", CPC_J0_RIGHT},
      {"J0_FIRE1", CPC_J0_FIRE1},
      {"J0_FIRE2", CPC_J0_FIRE2},
      {"J1_UP", CPC_J1_UP},
      {"J1_DOWN", CPC_J1_DOWN},
      {"J1_LEFT", CPC_J1_LEFT},
      {"J1_RIGHT", CPC_J1_RIGHT},
      {"J1_FIRE1", CPC_J1_FIRE1},
      {"J1_FIRE2", CPC_J1_FIRE2},
  };
  return tbl;
}

inline const std::map<char, CPC_KEYS>& cpc_char_to_key() {
  static const std::map<char, CPC_KEYS> tbl = {
      {'a', CPC_a},          {'b', CPC_b},          {'c', CPC_c},
      {'d', CPC_d},          {'e', CPC_e},          {'f', CPC_f},
      {'g', CPC_g},          {'h', CPC_h},          {'i', CPC_i},
      {'j', CPC_j},          {'k', CPC_k},          {'l', CPC_l},
      {'m', CPC_m},          {'n', CPC_n},          {'o', CPC_o},
      {'p', CPC_p},          {'q', CPC_q},          {'r', CPC_r},
      {'s', CPC_s},          {'t', CPC_t},          {'u', CPC_u},
      {'v', CPC_v},          {'w', CPC_w},          {'x', CPC_x},
      {'y', CPC_y},          {'z', CPC_z},          {'A', CPC_A},
      {'B', CPC_B},          {'C', CPC_C},          {'D', CPC_D},
      {'E', CPC_E},          {'F', CPC_F},          {'G', CPC_G},
      {'H', CPC_H},          {'I', CPC_I},          {'J', CPC_J},
      {'K', CPC_K},          {'L', CPC_L},          {'M', CPC_M},
      {'N', CPC_N},          {'O', CPC_O},          {'P', CPC_P},
      {'Q', CPC_Q},          {'R', CPC_R},          {'S', CPC_S},
      {'T', CPC_T},          {'U', CPC_U},          {'V', CPC_V},
      {'W', CPC_W},          {'X', CPC_X},          {'Y', CPC_Y},
      {'Z', CPC_Z},          {'0', CPC_0},          {'1', CPC_1},
      {'2', CPC_2},          {'3', CPC_3},          {'4', CPC_4},
      {'5', CPC_5},          {'6', CPC_6},          {'7', CPC_7},
      {'8', CPC_8},          {'9', CPC_9},          {' ', CPC_SPACE},
      {'\n', CPC_RETURN},    {'\r', CPC_RETURN},    {'.', CPC_PERIOD},
      {',', CPC_COMMA},      {';', CPC_SEMICOLON},  {':', CPC_COLON},
      {'-', CPC_MINUS},      {'+', CPC_PLUS},       {'/', CPC_SLASH},
      {'*', CPC_ASTERISK},   {'=', CPC_EQUAL},      {'(', CPC_LEFTPAREN},
      {')', CPC_RIGHTPAREN}, {'[', CPC_LBRACKET},   {']', CPC_RBRACKET},
      {'{', CPC_LCBRACE},    {'}', CPC_RCBRACE},    {'<', CPC_LESS},
      {'>', CPC_GREATER},    {'?', CPC_QUESTION},   {'!', CPC_EXCLAMATN},
      {'@', CPC_AT},         {'#', CPC_HASH},       {'$', CPC_DOLLAR},
      {'%', CPC_PERCENT},    {'^', CPC_POWER},      {'&', CPC_AMPERSAND},
      {'|', CPC_PIPE},       {'\\', CPC_BACKSLASH}, {'"', CPC_DBLQUOTE},
      {'\'', CPC_QUOTE},     {'`', CPC_BACKQUOTE},  {'_', CPC_UNDERSCORE},
  };
  return tbl;
}

// Modifier flag for one chord token, or 0 if it is not a recognized modifier.
// CTRL/CONTROL -> MOD_CPC_CTRL; SHIFT/LSHIFT/RSHIFT -> MOD_CPC_SHIFT. Case-
// insensitive. The flags ride in the CPCScancode high byte, so OR-ing them
// into a base key's scancode makes the matrix write set the modifier rows in
// the same atomic ipc_apply_keypress call.
inline CPCScancode cpc_modifier_flag(const std::string& name) {
  std::string upper = name;
  for (auto& c : upper)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (upper == "CTRL" || upper == "CONTROL") return MOD_CPC_CTRL;
  if (upper == "SHIFT" || upper == "LSHIFT" || upper == "RSHIFT")
    return MOD_CPC_SHIFT;
  return 0;
}

// Split a chord string ("CTRL+SHIFT+ESC") on '+' into its tokens. A trailing
// '+' yields an empty final token, which the caller treats as malformed.
inline std::vector<std::string> cpc_chord_tokens(const std::string& chord) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= chord.size()) {
    size_t const plus = chord.find('+', start);
    if (plus == std::string::npos) {
      out.push_back(chord.substr(start));
      break;
    }
    out.push_back(chord.substr(start, plus - start));
    start = plus + 1;
  }
  return out;
}
