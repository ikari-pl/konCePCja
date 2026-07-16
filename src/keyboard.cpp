// konCePCja — host input mapping (see keyboard.h for the model).

#include "keyboard.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "fileutils.h"
#include "koncepcja.h"
#include "log.h"
#include "stringutils.h"

extern byte bit_values[8];
extern t_CPC CPC;

namespace {

// Scancode a CPC key does not exist on / cannot be produced.
constexpr CPCScancode kNoScancode = 0xff;

// ------------------------------------------------------------------------
// CPC-side scancode tables.
//
// The English layout is the base; the national keyboards are the same
// matrix with a handful of keys moved or dropped, so they are expressed as
// override lists — every difference between layouts is auditable at a
// glance instead of being buried in three near-identical 209-entry tables.
// Scancode format: (matrix row << 4) | bit, plus MOD_CPC_* lines.
// https://www.cpcwiki.eu/index.php/Programming:Keyboard_scanning
// ------------------------------------------------------------------------

constexpr std::array<CPCScancode, CPC_KEY_NUM> kEnglishLayout = {{
    0x40,  // CPC_0
    0x80,  // CPC_1
    0x81,  // CPC_2
    0x71,  // CPC_3
    0x70,  // CPC_4
    0x61,  // CPC_5
    0x60,  // CPC_6
    0x51,  // CPC_7
    0x50,  // CPC_8
    0x41,  // CPC_9
    0x85 | MOD_CPC_SHIFT,  // CPC_A
    0x66 | MOD_CPC_SHIFT,  // CPC_B
    0x76 | MOD_CPC_SHIFT,  // CPC_C
    0x75 | MOD_CPC_SHIFT,  // CPC_D
    0x72 | MOD_CPC_SHIFT,  // CPC_E
    0x65 | MOD_CPC_SHIFT,  // CPC_F
    0x64 | MOD_CPC_SHIFT,  // CPC_G
    0x54 | MOD_CPC_SHIFT,  // CPC_H
    0x43 | MOD_CPC_SHIFT,  // CPC_I
    0x55 | MOD_CPC_SHIFT,  // CPC_J
    0x45 | MOD_CPC_SHIFT,  // CPC_K
    0x44 | MOD_CPC_SHIFT,  // CPC_L
    0x46 | MOD_CPC_SHIFT,  // CPC_M
    0x56 | MOD_CPC_SHIFT,  // CPC_N
    0x42 | MOD_CPC_SHIFT,  // CPC_O
    0x33 | MOD_CPC_SHIFT,  // CPC_P
    0x83 | MOD_CPC_SHIFT,  // CPC_Q
    0x62 | MOD_CPC_SHIFT,  // CPC_R
    0x74 | MOD_CPC_SHIFT,  // CPC_S
    0x63 | MOD_CPC_SHIFT,  // CPC_T
    0x52 | MOD_CPC_SHIFT,  // CPC_U
    0x67 | MOD_CPC_SHIFT,  // CPC_V
    0x73 | MOD_CPC_SHIFT,  // CPC_W
    0x77 | MOD_CPC_SHIFT,  // CPC_X
    0x53 | MOD_CPC_SHIFT,  // CPC_Y
    0x87 | MOD_CPC_SHIFT,  // CPC_Z
    0x85,  // CPC_a
    0x66,  // CPC_b
    0x76,  // CPC_c
    0x75,  // CPC_d
    0x72,  // CPC_e
    0x65,  // CPC_f
    0x64,  // CPC_g
    0x54,  // CPC_h
    0x43,  // CPC_i
    0x55,  // CPC_j
    0x45,  // CPC_k
    0x44,  // CPC_l
    0x46,  // CPC_m
    0x56,  // CPC_n
    0x42,  // CPC_o
    0x33,  // CPC_p
    0x83,  // CPC_q
    0x62,  // CPC_r
    0x74,  // CPC_s
    0x63,  // CPC_t
    0x52,  // CPC_u
    0x67,  // CPC_v
    0x73,  // CPC_w
    0x77,  // CPC_x
    0x53,  // CPC_y
    0x87,  // CPC_z
    0x85 | MOD_CPC_CTRL,  // CPC_CTRL_a
    0x66 | MOD_CPC_CTRL,  // CPC_CTRL_b
    0x76 | MOD_CPC_CTRL,  // CPC_CTRL_c
    0x75 | MOD_CPC_CTRL,  // CPC_CTRL_d
    0x72 | MOD_CPC_CTRL,  // CPC_CTRL_e
    0x65 | MOD_CPC_CTRL,  // CPC_CTRL_f
    0x64 | MOD_CPC_CTRL,  // CPC_CTRL_g
    0x54 | MOD_CPC_CTRL,  // CPC_CTRL_h
    0x43 | MOD_CPC_CTRL,  // CPC_CTRL_i
    0x55 | MOD_CPC_CTRL,  // CPC_CTRL_j
    0x45 | MOD_CPC_CTRL,  // CPC_CTRL_k
    0x44 | MOD_CPC_CTRL,  // CPC_CTRL_l
    0x46 | MOD_CPC_CTRL,  // CPC_CTRL_m
    0x56 | MOD_CPC_CTRL,  // CPC_CTRL_n
    0x42 | MOD_CPC_CTRL,  // CPC_CTRL_o
    0x33 | MOD_CPC_CTRL,  // CPC_CTRL_p
    0x83 | MOD_CPC_CTRL,  // CPC_CTRL_q
    0x62 | MOD_CPC_CTRL,  // CPC_CTRL_r
    0x74 | MOD_CPC_CTRL,  // CPC_CTRL_s
    0x63 | MOD_CPC_CTRL,  // CPC_CTRL_t
    0x52 | MOD_CPC_CTRL,  // CPC_CTRL_u
    0x67 | MOD_CPC_CTRL,  // CPC_CTRL_v
    0x73 | MOD_CPC_CTRL,  // CPC_CTRL_w
    0x77 | MOD_CPC_CTRL,  // CPC_CTRL_x
    0x53 | MOD_CPC_CTRL,  // CPC_CTRL_y
    0x87 | MOD_CPC_CTRL,  // CPC_CTRL_z
    0x40 | MOD_CPC_CTRL,  // CPC_CTRL_0
    0x80 | MOD_CPC_CTRL,  // CPC_CTRL_1
    0x81 | MOD_CPC_CTRL,  // CPC_CTRL_2
    0x71 | MOD_CPC_CTRL,  // CPC_CTRL_3
    0x70 | MOD_CPC_CTRL,  // CPC_CTRL_4
    0x61 | MOD_CPC_CTRL,  // CPC_CTRL_5
    0x60 | MOD_CPC_CTRL,  // CPC_CTRL_6
    0x51 | MOD_CPC_CTRL,  // CPC_CTRL_7
    0x50 | MOD_CPC_CTRL,  // CPC_CTRL_8
    0x41 | MOD_CPC_CTRL,  // CPC_CTRL_9
    0x00 | MOD_CPC_CTRL,  // CPC_CTRL_UP
    0x02 | MOD_CPC_CTRL,  // CPC_CTRL_DOWN
    0x10 | MOD_CPC_CTRL,  // CPC_CTRL_LEFT
    0x01 | MOD_CPC_CTRL,  // CPC_CTRL_RIGHT
    0x60 | MOD_CPC_SHIFT,  // CPC_AMPERSAND
    0x35 | MOD_CPC_SHIFT,  // CPC_ASTERISK
    0x32,  // CPC_AT
    0x26 | MOD_CPC_SHIFT,  // CPC_BACKQUOTE
    0x26,  // CPC_BACKSLASH
    0x86,  // CPC_CAPSLOCK
    0x20,  // CPC_CLR
    0x35,  // CPC_COLON
    0x47,  // CPC_COMMA
    0x27,  // CPC_CONTROL
    0x11,  // CPC_COPY
    0x02 | MOD_CPC_SHIFT,  // CPC_CPY_DOWN
    0x10 | MOD_CPC_SHIFT,  // CPC_CPY_LEFT
    0x01 | MOD_CPC_SHIFT,  // CPC_CPY_RIGHT
    0x00 | MOD_CPC_SHIFT,  // CPC_CPY_UP
    0x02,  // CPC_CUR_DOWN
    0x10,  // CPC_CUR_LEFT
    0x01,  // CPC_CUR_RIGHT
    0x00,  // CPC_CUR_UP
    0x02 | MOD_CPC_CTRL,  // CPC_CUR_ENDBL
    0x10 | MOD_CPC_CTRL,  // CPC_CUR_HOMELN
    0x01 | MOD_CPC_CTRL,  // CPC_CUR_ENDLN
    0x00 | MOD_CPC_CTRL,  // CPC_CUR_HOMEBL
    0x81 | MOD_CPC_SHIFT,  // CPC_DBLQUOTE
    0x97,  // CPC_DEL
    0x70 | MOD_CPC_SHIFT,  // CPC_DOLLAR
    0x06,  // CPC_ENTER
    0x31 | MOD_CPC_SHIFT,  // CPC_EQUAL
    0x82,  // CPC_ESC
    0x80 | MOD_CPC_SHIFT,  // CPC_EXCLAMATN
    0x17,  // CPC_F0
    0x15,  // CPC_F1
    0x16,  // CPC_F2
    0x05,  // CPC_F3
    0x24,  // CPC_F4
    0x14,  // CPC_F5
    0x04,  // CPC_F6
    0x12,  // CPC_F7
    0x13,  // CPC_F8
    0x03,  // CPC_F9
    0x17 | MOD_CPC_CTRL,  // CPC_CTRL_F0
    0x15 | MOD_CPC_CTRL,  // CPC_CTRL_F1
    0x16 | MOD_CPC_CTRL,  // CPC_CTRL_F2
    0x05 | MOD_CPC_CTRL,  // CPC_CTRL_F3
    0x24 | MOD_CPC_CTRL,  // CPC_CTRL_F4
    0x14 | MOD_CPC_CTRL,  // CPC_CTRL_F5
    0x04 | MOD_CPC_CTRL,  // CPC_CTRL_F6
    0x12 | MOD_CPC_CTRL,  // CPC_CTRL_F7
    0x13 | MOD_CPC_CTRL,  // CPC_CTRL_F8
    0x03 | MOD_CPC_CTRL,  // CPC_CTRL_F9
    0x17 | MOD_CPC_SHIFT,  // CPC_SHIFT_F0
    0x15 | MOD_CPC_SHIFT,  // CPC_SHIFT_F1
    0x16 | MOD_CPC_SHIFT,  // CPC_SHIFT_F2
    0x05 | MOD_CPC_SHIFT,  // CPC_SHIFT_F3
    0x24 | MOD_CPC_SHIFT,  // CPC_SHIFT_F4
    0x14 | MOD_CPC_SHIFT,  // CPC_SHIFT_F5
    0x04 | MOD_CPC_SHIFT,  // CPC_SHIFT_F6
    0x12 | MOD_CPC_SHIFT,  // CPC_SHIFT_F7
    0x13 | MOD_CPC_SHIFT,  // CPC_SHIFT_F8
    0x03 | MOD_CPC_SHIFT,  // CPC_SHIFT_F9
    0x07,  // CPC_FPERIOD
    0x37 | MOD_CPC_SHIFT,  // CPC_GREATER
    0x71 | MOD_CPC_SHIFT,  // CPC_HASH
    0x21,  // CPC_LBRACKET
    0x21 | MOD_CPC_SHIFT,  // CPC_LCBRACE
    0x50 | MOD_CPC_SHIFT,  // CPC_LEFTPAREN
    0x47 | MOD_CPC_SHIFT,  // CPC_LESS
    0x25,  // CPC_LSHIFT
    0x31,  // CPC_MINUS
    0x61 | MOD_CPC_SHIFT,  // CPC_PERCENT
    0x37,  // CPC_PERIOD
    0x32 | MOD_CPC_SHIFT,  // CPC_PIPE
    0x34 | MOD_CPC_SHIFT,  // CPC_PLUS
    0x30 | MOD_CPC_SHIFT,  // CPC_POUND
    0x30,  // CPC_POWER
    0x36 | MOD_CPC_SHIFT,  // CPC_QUESTION
    0x51 | MOD_CPC_SHIFT,  // CPC_QUOTE
    0x23,  // CPC_RBRACKET
    0x23 | MOD_CPC_SHIFT,  // CPC_RCBRACE
    0x22,  // CPC_RETURN
    0x41 | MOD_CPC_SHIFT,  // CPC_RIGHTPAREN
    0x25,  // CPC_RSHIFT
    0x34,  // CPC_SEMICOLON
    0x36,  // CPC_SLASH
    0x57,  // CPC_SPACE
    0x84,  // CPC_TAB
    0x40 | MOD_CPC_SHIFT,  // CPC_UNDERSCORE
    0x90,  // CPC_J0_UP
    0x91,  // CPC_J0_DOWN
    0x92,  // CPC_J0_LEFT
    0x93,  // CPC_J0_RIGHT
    0x94,  // CPC_J0_FIRE1
    0x95,  // CPC_J0_FIRE2
    0x60,  // CPC_J1_UP
    0x61,  // CPC_J1_DOWN
    0x62,  // CPC_J1_LEFT
    0x63,  // CPC_J1_RIGHT
    0x64,  // CPC_J1_FIRE1
    0x65,  // CPC_J1_FIRE2
    0xff,  // CPC_ES_NTILDE
    0xff,  // CPC_ES_nTILDE
    0xff,  // CPC_ES_PESETA
    0xff,  // CPC_FR_eACUTE
    0xff,  // CPC_FR_eGRAVE
    0xff,  // CPC_FR_cCEDIL
    0xff,  // CPC_FR_aGRAVE
    0xff,  // CPC_FR_uGRAVE
}};

struct LayoutOverride {
  CPC_KEYS key;
  CPCScancode scancode;
};

// French (AZERTY) differences from the English layout.
constexpr LayoutOverride kFrenchOverrides[] = {
    {CPC_0, 0x40 | MOD_CPC_SHIFT},
    {CPC_1, 0x80 | MOD_CPC_SHIFT},
    {CPC_2, 0x81 | MOD_CPC_SHIFT},
    {CPC_3, 0x71 | MOD_CPC_SHIFT},
    {CPC_4, 0x70 | MOD_CPC_SHIFT},
    {CPC_5, 0x61 | MOD_CPC_SHIFT},
    {CPC_6, 0x60 | MOD_CPC_SHIFT},
    {CPC_7, 0x51 | MOD_CPC_SHIFT},
    {CPC_8, 0x50 | MOD_CPC_SHIFT},
    {CPC_9, 0x41 | MOD_CPC_SHIFT},
    {CPC_A, 0x83 | MOD_CPC_SHIFT},
    {CPC_M, 0x35 | MOD_CPC_SHIFT},
    {CPC_Q, 0x85 | MOD_CPC_SHIFT},
    {CPC_W, 0x87 | MOD_CPC_SHIFT},
    {CPC_Z, 0x73 | MOD_CPC_SHIFT},
    {CPC_a, 0x83},
    {CPC_m, 0x35},
    {CPC_q, 0x85},
    {CPC_w, 0x87},
    {CPC_z, 0x73},
    {CPC_CTRL_a, 0x83 | MOD_CPC_CTRL},
    {CPC_CTRL_m, 0x35 | MOD_CPC_CTRL},
    {CPC_CTRL_q, 0x85 | MOD_CPC_CTRL},
    {CPC_CTRL_w, 0x87 | MOD_CPC_CTRL},
    {CPC_CTRL_z, 0x73 | MOD_CPC_CTRL},
    {CPC_AMPERSAND, 0x80},
    {CPC_ASTERISK, 0x21},
    {CPC_AT, 0x26 | MOD_CPC_SHIFT},
    {CPC_BACKQUOTE, 0xff},
    {CPC_BACKSLASH, 0x26 | MOD_CPC_CTRL},
    {CPC_COLON, 0x37},
    {CPC_COMMA, 0x46},
    {CPC_DBLQUOTE, 0x71},
    {CPC_DOLLAR, 0x26},
    {CPC_EQUAL, 0x36},
    {CPC_EXCLAMATN, 0x50},
    {CPC_GREATER, 0x23 | MOD_CPC_SHIFT},
    {CPC_HASH, 0x23},
    {CPC_LBRACKET, 0x31 | MOD_CPC_SHIFT},
    {CPC_LCBRACE, 0xff},
    {CPC_LEFTPAREN, 0x61},
    {CPC_LESS, 0x21 | MOD_CPC_SHIFT},
    {CPC_MINUS, 0x30},
    {CPC_PERCENT, 0x34 | MOD_CPC_SHIFT},
    {CPC_PERIOD, 0x47 | MOD_CPC_SHIFT},
    {CPC_PLUS, 0x36 | MOD_CPC_SHIFT},
    {CPC_POUND, 0xff},
    {CPC_POWER, 0x32},
    {CPC_QUESTION, 0x46 | MOD_CPC_SHIFT},
    {CPC_QUOTE, 0x70},
    {CPC_RBRACKET, 0x60},
    {CPC_RCBRACE, 0xff},
    {CPC_RIGHTPAREN, 0x31},
    {CPC_SEMICOLON, 0x47},
    {CPC_SLASH, 0x37 | MOD_CPC_SHIFT},
    {CPC_UNDERSCORE, 0x30 | MOD_CPC_SHIFT},
    {CPC_FR_eACUTE, 0x81},
    {CPC_FR_eGRAVE, 0x51},
    {CPC_FR_cCEDIL, 0x41},
    {CPC_FR_aGRAVE, 0x40},
    {CPC_FR_uGRAVE, 0x34},
};

// Spanish differences from the English layout.
constexpr LayoutOverride kSpanishOverrides[] = {
    {CPC_ASTERISK, 0x21 | MOD_CPC_SHIFT},
    {CPC_COLON, 0x34 | MOD_CPC_SHIFT},
    {CPC_LCBRACE, 0xff},
    {CPC_PLUS, 0x23 | MOD_CPC_SHIFT},
    {CPC_POUND, 0xff},
    {CPC_RCBRACE, 0xff},
    {CPC_ES_NTILDE, 0x35 | MOD_CPC_SHIFT},
    {CPC_ES_nTILDE, 0x35},
    {CPC_ES_PESETA, 0x30 | MOD_CPC_SHIFT},
};

template <size_t N>
constexpr std::array<CPCScancode, CPC_KEY_NUM> patched_layout(
    const std::array<CPCScancode, CPC_KEY_NUM>& base,
    const LayoutOverride (&overrides)[N]) {
  std::array<CPCScancode, CPC_KEY_NUM> out = base;
  for (size_t i = 0; i < N; ++i) {
    out[overrides[i].key] = overrides[i].scancode;
  }
  return out;
}

}  // namespace

const std::array<std::array<CPCScancode, CPC_KEY_NUM>, CPC_KEYBOARD_NUM>
    InputMapper::cpc_kbd = {{
        kEnglishLayout,
        patched_layout(kEnglishLayout, kFrenchOverrides),
        patched_layout(kEnglishLayout, kSpanishOverrides),
    }};

// Char → CPC key, for the virtual keyboard and autotype-style text entry.
// Letters and digits are mechanical (the enums mirror ASCII order); only
// punctuation and control characters need explicit entries.
const std::map<char, CPC_KEYS> InputMapper::CPCkeysFromChars = [] {
  std::map<char, CPC_KEYS> m;
  for (int i = 0; i < 26; ++i) {
    m.emplace(static_cast<char>('a' + i), static_cast<CPC_KEYS>(CPC_a + i));
    m.emplace(static_cast<char>('A' + i), static_cast<CPC_KEYS>(CPC_A + i));
  }
  for (int i = 0; i < 10; ++i) {
    m.emplace(static_cast<char>('0' + i), static_cast<CPC_KEYS>(CPC_0 + i));
  }
  constexpr std::pair<char, CPC_KEYS> punctuation[] = {
      {'&', CPC_AMPERSAND},
      {'#', CPC_HASH},
      {'"', CPC_DBLQUOTE},
      {'\'', CPC_QUOTE},
      {'(', CPC_LEFTPAREN},
      {'-', CPC_MINUS},
      {'_', CPC_UNDERSCORE},
      {')', CPC_RIGHTPAREN},
      {'=', CPC_EQUAL},
      {'*', CPC_ASTERISK},
      {',', CPC_COMMA},
      {';', CPC_SEMICOLON},
      {':', CPC_COLON},
      {'!', CPC_EXCLAMATN},
      {'$', CPC_DOLLAR},
      {'|', CPC_PIPE},
      {'?', CPC_QUESTION},
      {'.', CPC_PERIOD},
      {'/', CPC_SLASH},
      {' ', CPC_SPACE},
      {'\n', CPC_RETURN},
      {'+', CPC_PLUS},
      {'%', CPC_PERCENT},
      {'<', CPC_LESS},
      {'>', CPC_GREATER},
      {'[', CPC_LBRACKET},
      {']', CPC_RBRACKET},
      {'{', CPC_LCBRACE},
      {'}', CPC_RCBRACE},
      {'\\', CPC_BACKSLASH},
      {'\b', CPC_DEL},
      {'`', CPC_BACKQUOTE},
      {'@', CPC_AT},
      {'^', CPC_POWER},
  };
  for (const auto& [c, key] : punctuation) m.emplace(c, key);
  return m;
}();

// Default (US host keyboard) CapriceKey → host key bindings, used when no
// .map layout file overrides them. Letter, digit, and keypad-function rows
// are mechanical; the rest is explicit. Insertion keeps the FIRST binding
// for a key (map semantics the layout loader relies on).
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
std::map<CapriceKey, PCKey> InputMapper::SDLkeysymFromCPCkeys_us = [] {
  std::map<CapriceKey, PCKey> m;
  for (int i = 0; i < 26; ++i) {
    PCKey letter = SDLK_A + i;
    m.emplace(CPC_A + i, letter | MOD_PC_SHIFT);
    m.emplace(CPC_a + i, letter);
    m.emplace(CPC_CTRL_a + i, letter | MOD_PC_CTRL);
  }
  for (int i = 0; i < 10; ++i) {
    m.emplace(CPC_0 + i, PCKey{SDLK_0} + i);
    m.emplace(CPC_CTRL_0 + i, (PCKey{SDLK_0} + i) | MOD_PC_CTRL);
    // CPC function keys live on the host numeric keypad.
    m.emplace(CPC_F0 + i, PCKey{SDLK_KP_0} + i);
    m.emplace(CPC_CTRL_F0 + i, (PCKey{SDLK_KP_0} + i) | MOD_PC_CTRL);
    m.emplace(CPC_SHIFT_F0 + i, (PCKey{SDLK_KP_0} + i) | MOD_PC_SHIFT);
  }
    m.emplace(CPC_CTRL_UP, SDLK_UP | MOD_PC_CTRL);
    m.emplace(CPC_CTRL_DOWN, SDLK_DOWN | MOD_PC_CTRL);
    m.emplace(CPC_CTRL_LEFT, SDLK_LEFT | MOD_PC_CTRL);
    m.emplace(CPC_CTRL_RIGHT, SDLK_RIGHT | MOD_PC_CTRL);
    m.emplace(CPC_AMPERSAND, SDLK_7 | MOD_PC_SHIFT);
    m.emplace(CPC_ASTERISK, SDLK_8 | MOD_PC_SHIFT);
    m.emplace(CPC_AT, SDLK_2 | MOD_PC_SHIFT);
    m.emplace(CPC_BACKQUOTE, PCKey{SDLK_GRAVE});
    m.emplace(CPC_BACKSLASH, PCKey{SDLK_BACKSLASH});
    m.emplace(CPC_CAPSLOCK, PCKey{SDLK_CAPSLOCK});
    m.emplace(CPC_CLR, PCKey{SDLK_DELETE});
    m.emplace(CPC_COLON, SDLK_SEMICOLON | MOD_PC_SHIFT);
    m.emplace(CPC_COMMA, PCKey{SDLK_COMMA});
    m.emplace(CPC_CONTROL, PCKey{SDLK_LCTRL});
    m.emplace(CPC_COPY, PCKey{SDLK_LALT});
    m.emplace(CPC_CPY_DOWN, SDLK_DOWN | MOD_PC_SHIFT);
    m.emplace(CPC_CPY_LEFT, SDLK_LEFT | MOD_PC_SHIFT);
    m.emplace(CPC_CPY_RIGHT, SDLK_RIGHT | MOD_PC_SHIFT);
    m.emplace(CPC_CPY_UP, SDLK_UP | MOD_PC_SHIFT);
    m.emplace(CPC_CUR_DOWN, PCKey{SDLK_DOWN});
    m.emplace(CPC_CUR_LEFT, PCKey{SDLK_LEFT});
    m.emplace(CPC_CUR_RIGHT, PCKey{SDLK_RIGHT});
    m.emplace(CPC_CUR_UP, PCKey{SDLK_UP});
    m.emplace(CPC_CUR_HOMELN, PCKey{SDLK_HOME});
    m.emplace(CPC_CUR_ENDLN, PCKey{SDLK_END});
    m.emplace(CPC_CUR_HOMEBL, SDLK_HOME | MOD_PC_CTRL);
    m.emplace(CPC_CUR_ENDBL, SDLK_END | MOD_PC_CTRL);
    m.emplace(CPC_DBLQUOTE, SDLK_APOSTROPHE | MOD_PC_SHIFT);
    m.emplace(CPC_DEL, PCKey{SDLK_BACKSPACE});
    m.emplace(CPC_DOLLAR, SDLK_4 | MOD_PC_SHIFT);
    m.emplace(CPC_ENTER, PCKey{SDLK_KP_ENTER});
    m.emplace(CPC_EQUAL, PCKey{SDLK_EQUALS});
    m.emplace(CPC_ESC, PCKey{SDLK_ESCAPE});
    m.emplace(CPC_EXCLAMATN, SDLK_1 | MOD_PC_SHIFT);
    m.emplace(CPC_FPERIOD, PCKey{SDLK_KP_PERIOD});
    m.emplace(CPC_GREATER, SDLK_PERIOD | MOD_PC_SHIFT);
    m.emplace(CPC_HASH, SDLK_3 | MOD_PC_SHIFT);
    m.emplace(CPC_LBRACKET, PCKey{SDLK_LEFTBRACKET});
    m.emplace(CPC_LCBRACE, SDLK_LEFTBRACKET | MOD_PC_SHIFT);
    m.emplace(CPC_LEFTPAREN, SDLK_9 | MOD_PC_SHIFT);
    m.emplace(CPC_LESS, SDLK_COMMA | MOD_PC_SHIFT);
    m.emplace(CPC_LSHIFT, PCKey{SDLK_LSHIFT});
    m.emplace(CPC_MINUS, PCKey{SDLK_MINUS});
    m.emplace(CPC_PERCENT, SDLK_5 | MOD_PC_SHIFT);
    m.emplace(CPC_PERIOD, PCKey{SDLK_PERIOD});
    m.emplace(CPC_PIPE, SDLK_BACKSLASH | MOD_PC_SHIFT);
    m.emplace(CPC_PLUS, SDLK_EQUALS | MOD_PC_SHIFT);
    m.emplace(CPC_POUND, PCKey{0});
    m.emplace(CPC_POWER, SDLK_6 | MOD_PC_SHIFT);
    m.emplace(CPC_QUESTION, SDLK_SLASH | MOD_PC_SHIFT);
    m.emplace(CPC_QUOTE, PCKey{SDLK_APOSTROPHE});
    m.emplace(CPC_RBRACKET, PCKey{SDLK_RIGHTBRACKET});
    m.emplace(CPC_RCBRACE, SDLK_RIGHTBRACKET | MOD_PC_SHIFT);
    m.emplace(CPC_RETURN, PCKey{SDLK_RETURN});
    m.emplace(CPC_RIGHTPAREN, SDLK_0 | MOD_PC_SHIFT);
    m.emplace(CPC_RSHIFT, PCKey{SDLK_RSHIFT});
    m.emplace(CPC_SEMICOLON, PCKey{SDLK_SEMICOLON});
    m.emplace(CPC_SLASH, PCKey{SDLK_SLASH});
    m.emplace(CPC_SPACE, PCKey{SDLK_SPACE});
    m.emplace(CPC_TAB, PCKey{SDLK_TAB});
    m.emplace(CPC_UNDERSCORE, SDLK_MINUS | MOD_PC_SHIFT);
    m.emplace(KONCPC_GUI, PCKey{SDLK_F1});
    m.emplace(KONCPC_VKBD, SDLK_F1 | MOD_PC_SHIFT);
    m.emplace(KONCPC_FULLSCRN, PCKey{SDLK_F2});
    m.emplace(KONCPC_DEVTOOLS, SDLK_F2 | MOD_PC_SHIFT);
    m.emplace(KONCPC_SCRNSHOT, PCKey{SDLK_F3});
    m.emplace(KONCPC_SNAPSHOT, SDLK_F3 | MOD_PC_SHIFT);
    m.emplace(KONCPC_LD_SNAP, SDLK_F4 | MOD_PC_SHIFT);
    m.emplace(KONCPC_RESET, PCKey{SDLK_F5});
    m.emplace(KONCPC_NEXTDISKA, SDLK_F5 | MOD_PC_SHIFT);
    m.emplace(KONCPC_MF2STOP, PCKey{SDLK_F6});
    m.emplace(KONCPC_VJOY, SDLK_F6 | MOD_PC_SHIFT);
    m.emplace(KONCPC_JOY, PCKey{SDLK_F7});
    m.emplace(KONCPC_PHAZER, SDLK_F7 | MOD_PC_SHIFT);
    m.emplace(KONCPC_FPS, PCKey{SDLK_F8});
    m.emplace(KONCPC_SPEED, PCKey{SDLK_F9});
    m.emplace(KONCPC_TIER, SDLK_F9 | MOD_PC_SHIFT);
    m.emplace(KONCPC_EXIT, PCKey{SDLK_F10});
    m.emplace(KONCPC_PASTE, PCKey{SDLK_F11});
    m.emplace(KONCPC_DEVTOOLS, PCKey{SDLK_F12});
    m.emplace(KONCPC_TAPEPLAY, PCKey{SDLK_F4});
    m.emplace(KONCPC_DELAY, PCKey{SDLK_PAUSE});
  return m;
}();

// Name → key tables for the .map layout files, generated from the same
// X-macro lists that define the enums (the strings can never go stale).
const std::map<const std::string, const CapriceKey>
    InputMapper::CPCkeysFromStrings = {
#define KEY_NAMED(k) {#k, k},
        CPC_KEYS_LIST(KEY_NAMED) KONCPC_KEYS_LIST(KEY_NAMED)
#undef KEY_NAMED
};

#define SDL_NAMED_KEY(k) {#k, k},
const std::map<const std::string, const PCKey> InputMapper::SDLkeysFromStrings =
    {
        SDL_NAMED_KEY(SDLK_BACKSPACE)
        SDL_NAMED_KEY(SDLK_TAB)
        SDL_NAMED_KEY(SDLK_CLEAR)
        SDL_NAMED_KEY(SDLK_RETURN)
        SDL_NAMED_KEY(SDLK_PAUSE)
        SDL_NAMED_KEY(SDLK_ESCAPE)
        SDL_NAMED_KEY(SDLK_SPACE)
        SDL_NAMED_KEY(SDLK_EXCLAIM)
        SDL_NAMED_KEY(SDLK_DBLAPOSTROPHE)
        SDL_NAMED_KEY(SDLK_HASH)
        SDL_NAMED_KEY(SDLK_DOLLAR)
        SDL_NAMED_KEY(SDLK_AMPERSAND)
        SDL_NAMED_KEY(SDLK_APOSTROPHE)
        SDL_NAMED_KEY(SDLK_LEFTPAREN)
        SDL_NAMED_KEY(SDLK_RIGHTPAREN)
        SDL_NAMED_KEY(SDLK_ASTERISK)
        SDL_NAMED_KEY(SDLK_PLUS)
        SDL_NAMED_KEY(SDLK_COMMA)
        SDL_NAMED_KEY(SDLK_MINUS)
        SDL_NAMED_KEY(SDLK_PERIOD)
        SDL_NAMED_KEY(SDLK_SLASH)
        SDL_NAMED_KEY(SDLK_0)
        SDL_NAMED_KEY(SDLK_1)
        SDL_NAMED_KEY(SDLK_2)
        SDL_NAMED_KEY(SDLK_3)
        SDL_NAMED_KEY(SDLK_4)
        SDL_NAMED_KEY(SDLK_5)
        SDL_NAMED_KEY(SDLK_6)
        SDL_NAMED_KEY(SDLK_7)
        SDL_NAMED_KEY(SDLK_8)
        SDL_NAMED_KEY(SDLK_9)
        SDL_NAMED_KEY(SDLK_COLON)
        SDL_NAMED_KEY(SDLK_SEMICOLON)
        SDL_NAMED_KEY(SDLK_LESS)
        SDL_NAMED_KEY(SDLK_EQUALS)
        SDL_NAMED_KEY(SDLK_GREATER)
        SDL_NAMED_KEY(SDLK_QUESTION)
        SDL_NAMED_KEY(SDLK_AT)
        SDL_NAMED_KEY(SDLK_LEFTBRACKET)
        SDL_NAMED_KEY(SDLK_BACKSLASH)
        SDL_NAMED_KEY(SDLK_RIGHTBRACKET)
        SDL_NAMED_KEY(SDLK_CARET)
        SDL_NAMED_KEY(SDLK_UNDERSCORE)
        SDL_NAMED_KEY(SDLK_GRAVE)
        SDL_NAMED_KEY(SDLK_A)
        SDL_NAMED_KEY(SDLK_B)
        SDL_NAMED_KEY(SDLK_C)
        SDL_NAMED_KEY(SDLK_D)
        SDL_NAMED_KEY(SDLK_E)
        SDL_NAMED_KEY(SDLK_F)
        SDL_NAMED_KEY(SDLK_G)
        SDL_NAMED_KEY(SDLK_H)
        SDL_NAMED_KEY(SDLK_I)
        SDL_NAMED_KEY(SDLK_J)
        SDL_NAMED_KEY(SDLK_K)
        SDL_NAMED_KEY(SDLK_L)
        SDL_NAMED_KEY(SDLK_M)
        SDL_NAMED_KEY(SDLK_N)
        SDL_NAMED_KEY(SDLK_O)
        SDL_NAMED_KEY(SDLK_P)
        SDL_NAMED_KEY(SDLK_Q)
        SDL_NAMED_KEY(SDLK_R)
        SDL_NAMED_KEY(SDLK_S)
        SDL_NAMED_KEY(SDLK_T)
        SDL_NAMED_KEY(SDLK_U)
        SDL_NAMED_KEY(SDLK_V)
        SDL_NAMED_KEY(SDLK_W)
        SDL_NAMED_KEY(SDLK_X)
        SDL_NAMED_KEY(SDLK_Y)
        SDL_NAMED_KEY(SDLK_Z)
        SDL_NAMED_KEY(SDLK_DELETE)
        SDL_NAMED_KEY(SDLK_PERCENT)
        SDL_NAMED_KEY(SDLK_KP_0)
        SDL_NAMED_KEY(SDLK_KP_1)
        SDL_NAMED_KEY(SDLK_KP_2)
        SDL_NAMED_KEY(SDLK_KP_3)
        SDL_NAMED_KEY(SDLK_KP_4)
        SDL_NAMED_KEY(SDLK_KP_5)
        SDL_NAMED_KEY(SDLK_KP_6)
        SDL_NAMED_KEY(SDLK_KP_7)
        SDL_NAMED_KEY(SDLK_KP_8)
        SDL_NAMED_KEY(SDLK_KP_9)
        SDL_NAMED_KEY(SDLK_KP_PERIOD)
        SDL_NAMED_KEY(SDLK_KP_DIVIDE)
        SDL_NAMED_KEY(SDLK_KP_MULTIPLY)
        SDL_NAMED_KEY(SDLK_KP_MINUS)
        SDL_NAMED_KEY(SDLK_KP_PLUS)
        SDL_NAMED_KEY(SDLK_KP_ENTER)
        SDL_NAMED_KEY(SDLK_KP_EQUALS)
        SDL_NAMED_KEY(SDLK_UP)
        SDL_NAMED_KEY(SDLK_DOWN)
        SDL_NAMED_KEY(SDLK_RIGHT)
        SDL_NAMED_KEY(SDLK_LEFT)
        SDL_NAMED_KEY(SDLK_INSERT)
        SDL_NAMED_KEY(SDLK_HOME)
        SDL_NAMED_KEY(SDLK_END)
        SDL_NAMED_KEY(SDLK_PAGEUP)
        SDL_NAMED_KEY(SDLK_PAGEDOWN)
        SDL_NAMED_KEY(SDLK_F1)
        SDL_NAMED_KEY(SDLK_F2)
        SDL_NAMED_KEY(SDLK_F3)
        SDL_NAMED_KEY(SDLK_F4)
        SDL_NAMED_KEY(SDLK_F5)
        SDL_NAMED_KEY(SDLK_F6)
        SDL_NAMED_KEY(SDLK_F7)
        SDL_NAMED_KEY(SDLK_F8)
        SDL_NAMED_KEY(SDLK_F9)
        SDL_NAMED_KEY(SDLK_F10)
        SDL_NAMED_KEY(SDLK_F11)
        SDL_NAMED_KEY(SDLK_F12)
        SDL_NAMED_KEY(SDLK_F13)
        SDL_NAMED_KEY(SDLK_F14)
        SDL_NAMED_KEY(SDLK_F15)
        SDL_NAMED_KEY(SDLK_NUMLOCKCLEAR)
        SDL_NAMED_KEY(SDLK_CAPSLOCK)
        SDL_NAMED_KEY(SDLK_SCROLLLOCK)
        SDL_NAMED_KEY(SDLK_RSHIFT)
        SDL_NAMED_KEY(SDLK_LSHIFT)
        SDL_NAMED_KEY(SDLK_RCTRL)
        SDL_NAMED_KEY(SDLK_LCTRL)
        SDL_NAMED_KEY(SDLK_RALT)
        SDL_NAMED_KEY(SDLK_LALT)
        SDL_NAMED_KEY(SDLK_LGUI)
        SDL_NAMED_KEY(SDLK_RGUI)
        SDL_NAMED_KEY(SDLK_MODE)
        SDL_NAMED_KEY(SDLK_APPLICATION)
        SDL_NAMED_KEY(SDLK_HELP)
        SDL_NAMED_KEY(SDLK_PRINTSCREEN)
        SDL_NAMED_KEY(SDLK_SYSREQ)
        SDL_NAMED_KEY(SDLK_PAUSE)
        SDL_NAMED_KEY(SDLK_MENU)
        SDL_NAMED_KEY(SDLK_POWER)
        SDL_NAMED_KEY(SDLK_UNDO)
        // Keysyms SDL has no name for (national characters entered by
        // keycode value).
        {"SDLK_NTILDE", 241},
        {"SDLK_UGRAVE", 249},
        {"SDLK_CCEDIL", 231},
        {"SDLK_CIRC", 0x40000000},
        {"SDLK_ESZETT", 223},
        {"SDLK_DEGREE", 186},
        {"SDLK_INV_QUESTION", 161},
        // Modifier names (MODE is AltGr / right Alt; left Alt is mapped to
        // CPC_COPY, so it is not accepted as a modifier).
        SDL_NAMED_KEY(MOD_PC_SHIFT)
        SDL_NAMED_KEY(MOD_PC_CTRL)
        SDL_NAMED_KEY(MOD_PC_MODE)
};
#undef SDL_NAMED_KEY

// Format of a line: CPC_xxx\tSDLK_Xxx\tMODIFIER
LineParsingResult InputMapper::process_cfg_line(const std::string& line) {
  LineParsingResult result;

  auto fields = stringutils::split(line, '\t', /*ignore_empty=*/true);
  if (fields.empty() || fields[0][0] == '#') return result;

  const std::string& cpc_name = fields[0];
  auto cpc_entry = CPCkeysFromStrings.find(cpc_name);
  if (cpc_entry == CPCkeysFromStrings.end()) {
    LOG_ERROR("Unknown CPC key " << cpc_name
                                 << " found in mapping file. Ignoring it.");
    result.valid = false;
    return result;
  }
  result.cpc_key = cpc_entry->second;
  result.cpc_key_name = cpc_name;

  // One host key name, optionally followed by one modifier name.
  for (size_t field = 1; field < fields.size() && field <= 2; ++field) {
    auto sdl_entry = SDLkeysFromStrings.find(fields[field]);
    if (sdl_entry == SDLkeysFromStrings.end()) {
      LOG_ERROR("Unknown SDL key or modifier "
                << fields[field] << " found in mapping file. Ignoring it.");
      result.valid = false;
      return result;
    }
    result.sdl_key |= sdl_entry->second;
    if (field > 1) result.sdl_key_name += " ";
    result.sdl_key_name += fields[field];
  }
  result.contains_mapping = true;
  SDLkeysymFromCPCkeys[result.cpc_key] = result.sdl_key;
  return result;
}

bool InputMapper::load_layout(const std::string& filename) {
  std::ifstream file(filename);
  if (is_directory(filename) || !file) {
    SDLkeysymFromCPCkeys = SDLkeysymFromCPCkeys_us;
    return true;
  }

  bool valid = true;
  std::set<CapriceKey> mapped_cpc_keys;
  std::set<PCKey> mapped_sdl_keys;
  std::string line;
  while (std::getline(file, line)) {
    auto parsed_line = process_cfg_line(line);
    valid &= parsed_line.valid;
    if (!parsed_line.contains_mapping) continue;
    // Verify that each CPC key is mapped only once
    if (!mapped_cpc_keys.insert(parsed_line.cpc_key).second) {
      LOG_ERROR("Mapping '" << filename
                            << "' contains a CPC key multiple times: "
                            << parsed_line.cpc_key_name);
      valid = false;
    }
    // And that no SDL key combination is mapped to 2 different CPC keys
    if (!mapped_sdl_keys.insert(parsed_line.sdl_key).second) {
      LOG_ERROR("Mapping '" << filename
                            << "' contains a SDL key multiple times: "
                            << parsed_line.sdl_key_name);
      valid = false;
    }
  }
  return valid;
}

void InputMapper::init() {
  // Ensure we're starting from a fresh state
  SDLkeysymFromCPCkeys.clear();
  CPCkeysFromSDLkeysym.clear();
  SDLkeysFromChars.clear();

  std::string const layout_file = CPC->resources_path + "/" + CPC->kbd_layout;
  load_layout(layout_file);

  for (const auto& [caprice_key, pc_key] : SDLkeysymFromCPCkeys) {
    CPCkeysFromSDLkeysym[pc_key] = caprice_key;
  }

  for (const auto& [c, caprice_key] : CPCkeysFromChars) {
    auto binding = SDLkeysymFromCPCkeys.find(caprice_key);
    if (binding != SDLkeysymFromCPCkeys.end()) {
      PCKey const sdl_moddedkey = binding->second;
      SDLkeysFromChars[c] = std::make_pair(
          static_cast<SDL_Keycode>(sdl_moddedkey & BITMASK_NOMOD),
          static_cast<SDL_Keymod>(sdl_moddedkey >> BITSHIFT_MOD));
    }
  }
}

CPCScancode InputMapper::CPCscancodeFromCPCkey(CPC_KEYS cpc_key) {
  return cpc_kbd[CPC->keyboard][cpc_key];
}

namespace {

// Folds an SDL keysym + live modifier state into the PCKey lookup form.
PCKey pckey_from_keysym(SDL_Keycode key, SDL_Keymod mod) {
  PCKey sdl_key = key;
  if (mod & SDL_KMOD_SHIFT) sdl_key |= MOD_PC_SHIFT;
  if (mod & SDL_KMOD_CTRL) sdl_key |= MOD_PC_CTRL;
  // Map right alt to Mode (AltGr). Not clear what determines whether SDL uses
  // one or the other and if both can happen together.
  if (mod & (SDL_KMOD_MODE | SDL_KMOD_RALT)) sdl_key |= MOD_PC_MODE;
  // Left alt is not folded: the key itself is mapped to CPC_COPY.
  // Sticky modifiers (num lock, caps lock) are ignored.
  return sdl_key;
}

}  // namespace

CPCScancode InputMapper::CPCscancodeFromKeysym(SDL_Keycode key,
                                               SDL_Keymod mod) {
  PCKey const sdl_key = pckey_from_keysym(key, mod);

  auto cpc_key = CPCkeysFromSDLkeysym.find(sdl_key);
  // Ctrl+key fallback: if no explicit Ctrl+key mapping, look up the base key
  // and add MOD_CPC_CTRL to the scancode.  applyKeypressDirect will then keep
  // the CPC Control key pressed in the matrix (without MOD_CPC_CTRL it would
  // explicitly release Control, defeating the combination).
  bool ctrl_fallback = false;
  if (cpc_key == CPCkeysFromSDLkeysym.end() && (sdl_key & MOD_PC_CTRL)) {
    cpc_key = CPCkeysFromSDLkeysym.find(sdl_key & ~MOD_PC_CTRL);
    ctrl_fallback = true;
  }
  if (cpc_key == CPCkeysFromSDLkeysym.end()) return kNoScancode;

  if (cpc_key->second & MOD_EMU_KEY) return cpc_key->second;
  CPCScancode sc = cpc_kbd[CPC->keyboard][cpc_key->second];
  if (ctrl_fallback) sc |= MOD_CPC_CTRL;
  return sc;
}

CapriceKey InputMapper::CPCkeyFromKeysym(SDL_Keycode key, SDL_Keymod mod) {
  auto cpc_key = CPCkeysFromSDLkeysym.find(pckey_from_keysym(key, mod));
  if (cpc_key == CPCkeysFromSDLkeysym.end()) return kNoScancode;
  return cpc_key->second;
}

std::string InputMapper::CPCkeyToString(const CapriceKey cpc_key) {
  if (cpc_key == kNoScancode) return "UNKNOWN";
  for (const auto& [str, keycode] : CPCkeysFromStrings) {
    if (cpc_key == keycode) {
      return str;
    }
  }
  return "UNMAPPED(" + std::to_string(cpc_key) + ")";
}

namespace {
// Format one host PCKey (modifier in the high dword, SDL keysym in the low) as
// a readable accelerator string, e.g. "Shift+F2", "F5", "Pause".
std::string format_pckey(PCKey pckey) {
  SDL_Keycode const key = static_cast<SDL_Keycode>(pckey & BITMASK_NOMOD);
  PCKey const mod = pckey >> BITSHIFT_MOD;
  std::string s;
  if (mod & SDL_KMOD_CTRL) s += "Ctrl+";
  if (mod & SDL_KMOD_SHIFT) s += "Shift+";
  const char* name = SDL_GetKeyName(key);
  s += (name != nullptr && *name != '\0') ? name : "?";
  return s;
}
}  // namespace

std::string InputMapper::shortcutForAction(CapriceKey action) const {
  std::vector<std::string> parts;
  for (const auto& [pckey, capkey] : CPCkeysFromSDLkeysym) {
    if (capkey == action) parts.push_back(format_pckey(pckey));
  }
  std::sort(parts.begin(), parts.end());
  parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out += " / ";
    out += parts[i];
  }
  return out;
}

// Keystone helper: derive an action's shortcut DISPLAY string from the live
// binding map, so every surface's hint stays truthful automatically.
std::string koncpc_action_shortcut(KONCPC_KEYS action) {
  if (CPC.InputMapper == nullptr) return "";
  return CPC.InputMapper->shortcutForAction(static_cast<CapriceKey>(action));
}

void InputMapper::set_joystick_emulation() {
  // Joystick-emulation remaps: host keys bound to these CPC keys drive the
  // digital joystick instead while the mode is active.
  static constexpr std::pair<CPC_KEYS, CPC_KEYS> joy_layout[] = {
      {CPC_J0_UP, CPC_CUR_UP},      {CPC_J0_DOWN, CPC_CUR_DOWN},
      {CPC_J0_LEFT, CPC_CUR_LEFT},  {CPC_J0_RIGHT, CPC_CUR_RIGHT},
      {CPC_J0_FIRE1, CPC_z},        {CPC_J0_FIRE2, CPC_x},
  };

  for (const auto& [joy_key, cpc_key] : joy_layout) {
    PCKey const pc_idx = SDLkeysymFromCPCkeys[cpc_key];  // host key to remap
    if (CPC->joystick_emulation == JoystickEmulation::Keyboard) {
      CPCkeysFromSDLkeysym[pc_idx] = joy_key;
    } else {
      CPCkeysFromSDLkeysym[pc_idx] = cpc_key;
    }
  }
}

CPCScancode InputMapper::CPCscancodeFromJoystickButton(
    SDL_JoyButtonEvent jbutton) {
  static constexpr CPC_KEYS fire[2][2] = {
      {CPC_J0_FIRE1, CPC_J0_FIRE2},
      {CPC_J1_FIRE1, CPC_J1_FIRE2},
  };
  if (jbutton.which > 1 || jbutton.button > 1) return kNoScancode;
  return cpc_kbd[CPC->keyboard][fire[jbutton.which][jbutton.button]];
}

void InputMapper::CPCscancodeFromJoystickAxis(SDL_JoyAxisEvent jaxis,
                                              CPCScancode* cpc_key,
                                              bool& release) {
  // Axes 0/2 steer left-right, axes 1/3 up-down; two joysticks supported.
  static constexpr CPC_KEYS dir_neg[2][2] = {
      {CPC_J0_LEFT, CPC_J0_UP},
      {CPC_J1_LEFT, CPC_J1_UP},
  };
  static constexpr CPC_KEYS dir_pos[2][2] = {
      {CPC_J0_RIGHT, CPC_J0_DOWN},
      {CPC_J1_RIGHT, CPC_J1_DOWN},
  };
  if (jaxis.which > 1 || jaxis.axis > 3) return;
  const int joy = jaxis.which;
  const int dir = jaxis.axis & 1;  // 0 = horizontal, 1 = vertical

  if (jaxis.value < -JOYSTICK_AXIS_THRESHOLD) {
    cpc_key[0] = cpc_kbd[CPC->keyboard][dir_neg[joy][dir]];
  } else if (jaxis.value > JOYSTICK_AXIS_THRESHOLD) {
    cpc_key[0] = cpc_kbd[CPC->keyboard][dir_pos[joy][dir]];
  } else {
    // Centered: release both directions of this axis.
    cpc_key[0] = cpc_kbd[CPC->keyboard][dir_neg[joy][dir]];
    cpc_key[1] = cpc_kbd[CPC->keyboard][dir_pos[joy][dir]];
    release = true;
  }
}

InputMapper::InputMapper(t_CPC* CPC) : CPC(CPC) {}

#include "keyboard_manager.h"
extern dword dwFrameCountOverall;

namespace {

// Presses (clears) or releases (sets) one matrix line.
inline void set_matrix_line(std::atomic<byte> keyboard_matrix[],
                            byte scancode, bool pressed) {
  if (pressed) {
    keyboard_matrix[scancode >> 4].fetch_and(~bit_values[scancode & 7],
                                             std::memory_order_relaxed);
  } else {
    keyboard_matrix[scancode >> 4].fetch_or(bit_values[scancode & 7],
                                            std::memory_order_relaxed);
  }
}

constexpr byte kShiftScancode = 0x25;
constexpr byte kControlScancode = 0x27;

}  // namespace

void applyKeypressDirect(CPCScancode cpc_key,
                         std::atomic<byte> keyboard_matrix[], bool pressed,
                         bool release_modifiers) {
  // Hold the matrix mutex across the whole key+shift+ctrl write sequence so the
  // per-frame snapshot copy (Z80 thread) cannot observe a half-applied keypress
  // — otherwise a shifted key's digit line could be scanned before its SHIFT
  // line, decoding the unshifted glyph ('1'->'&').  See beads-2qg / beads-d1n.
  std::scoped_lock const matrix_lock(g_kbd_matrix_mutex);
  if (pressed) {
    set_matrix_line(keyboard_matrix, static_cast<byte>(cpc_key), true);
    // Drive the SHIFT and CONTROL lines to exactly what the combination
    // needs — a required modifier is held, an unneeded one is released.
    set_matrix_line(keyboard_matrix, kShiftScancode,
                    (cpc_key & MOD_CPC_SHIFT) != 0);
    set_matrix_line(keyboard_matrix, kControlScancode,
                    (cpc_key & MOD_CPC_CTRL) != 0);
  } else {
    set_matrix_line(keyboard_matrix, static_cast<byte>(cpc_key), false);
    if (release_modifiers) {
      set_matrix_line(keyboard_matrix, kShiftScancode, false);
      set_matrix_line(keyboard_matrix, kControlScancode, false);
    }
  }
}

void applyKeypress(CPCScancode cpc_key, std::atomic<byte> keyboard_matrix[],
                   bool pressed, bool release_modifiers) {
  if ((!CPC.paused) && (static_cast<byte>(cpc_key) != 0xff)) {
    if (pressed) {
      g_keyboard_manager.handle_keydown(cpc_key, keyboard_matrix);
    } else {
      g_keyboard_manager.handle_keyup(cpc_key, keyboard_matrix,
                                      release_modifiers, dwFrameCountOverall);
    }
  }
}
