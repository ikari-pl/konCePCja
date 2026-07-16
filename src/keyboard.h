#pragma once

// konCePCja — host input mapping: SDL keyboard/joystick events to CPC
// keyboard-matrix scancodes.
//
// Layered model:
//   host key event (SDL keysym + modifiers, packed into a PCKey)
//     → CapriceKey  (host-agnostic: a CPC_KEYS combination or an emulator
//                    command from KONCPC_KEYS)
//     → CPCScancode (hardware matrix position (row << 4 | bit) plus the
//                    MOD_CPC_* lines that must be held with it)
// The per-user .map layout files rebind PCKey→CapriceKey; the CPC-side
// tables live in keyboard.cpp and depend only on the emulated keyboard
// variant (English/French/Spanish).

#include <array>
#include <cstdint>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "SDL3/SDL.h"
#include "koncepcja.h"
#include "types.h"

// Double-buffered CPC keyboard matrix (see kon_cpc_ja.cpp for rationale).
//   keyboard_matrix      - pending state written by all key sources
//   keyboard_matrix_live - per-frame snapshot the firmware scans (Z80 thread)
//   g_kbd_matrix_mutex    - makes a writer's multi-line keypress atomic vs the
//                           snapshot copy
extern std::atomic<byte> keyboard_matrix[16];
extern std::atomic<byte> keyboard_matrix_live[16];
extern std::mutex g_kbd_matrix_mutex;

// PCKey packs one host key combination: SDL modifier mask in the high dword,
// SDL keysym in the low dword.
using PCKey = qword;

// CapriceKey is the host-agnostic keyboard event: either a CPC key
// combination (CPC_KEYS) or an emulator command (KONCPC_KEYS).
using CapriceKey = unsigned int;

// CPCScancode is a hardware scancode — matrix row in the high nibble, bit in
// the low nibble — plus the MOD_CPC_* flag lines. cf.
// https://www.cpcwiki.eu/index.php/Programming:Keyboard_scanning#Hardware_scancode_table
using CPCScancode = dword;

// CPC-side modifier lines encoded alongside a scancode.
inline constexpr CPCScancode MOD_CPC_SHIFT = 0x01 << 8;
inline constexpr CPCScancode MOD_CPC_CTRL = 0x02 << 8;
inline constexpr CPCScancode MOD_EMU_KEY = 0x10 << 8;

// Host-side modifier bits of a PCKey.
inline constexpr int BITSHIFT_MOD = 32;
inline constexpr PCKey BITMASK_NOMOD = (PCKey{1} << BITSHIFT_MOD) - 1;
inline constexpr PCKey MOD_PC_SHIFT = PCKey{SDL_KMOD_SHIFT} << BITSHIFT_MOD;
inline constexpr PCKey MOD_PC_CTRL = PCKey{SDL_KMOD_CTRL} << BITSHIFT_MOD;
inline constexpr PCKey MOD_PC_MODE = PCKey{SDL_KMOD_MODE} << BITSHIFT_MOD;

// Master lists of key identifiers. Each X-macro is the single source of
// truth for both the enum below and the name→key tables in keyboard.cpp,
// so the two can never drift apart.
#define KONCPC_KEYS_LIST(X) \
  X(KONCPC_EXIT) \
  X(KONCPC_FPS) \
  X(KONCPC_FULLSCRN) \
  X(KONCPC_GUI) \
  X(KONCPC_VKBD) \
  X(KONCPC_JOY) \
  X(KONCPC_PHAZER) \
  X(KONCPC_MF2STOP) \
  X(KONCPC_RESET) \
  X(KONCPC_SCRNSHOT) \
  X(KONCPC_SPEED) \
  X(KONCPC_TAPEPLAY) \
  X(KONCPC_DEBUG) \
  X(KONCPC_SNAPSHOT) \
  X(KONCPC_LD_SNAP) \
  X(KONCPC_WAITBREAK) \
  X(KONCPC_DELAY) \
  X(KONCPC_PASTE) \
  X(KONCPC_DEVTOOLS) \
  X(KONCPC_NEXTDISKA) \
  X(KONCPC_VJOY) \
  X(KONCPC_TIER)

#define CPC_KEYS_LIST(X) \
  X(CPC_0) \
  X(CPC_1) \
  X(CPC_2) \
  X(CPC_3) \
  X(CPC_4) \
  X(CPC_5) \
  X(CPC_6) \
  X(CPC_7) \
  X(CPC_8) \
  X(CPC_9) \
  X(CPC_A) \
  X(CPC_B) \
  X(CPC_C) \
  X(CPC_D) \
  X(CPC_E) \
  X(CPC_F) \
  X(CPC_G) \
  X(CPC_H) \
  X(CPC_I) \
  X(CPC_J) \
  X(CPC_K) \
  X(CPC_L) \
  X(CPC_M) \
  X(CPC_N) \
  X(CPC_O) \
  X(CPC_P) \
  X(CPC_Q) \
  X(CPC_R) \
  X(CPC_S) \
  X(CPC_T) \
  X(CPC_U) \
  X(CPC_V) \
  X(CPC_W) \
  X(CPC_X) \
  X(CPC_Y) \
  X(CPC_Z) \
  X(CPC_a) \
  X(CPC_b) \
  X(CPC_c) \
  X(CPC_d) \
  X(CPC_e) \
  X(CPC_f) \
  X(CPC_g) \
  X(CPC_h) \
  X(CPC_i) \
  X(CPC_j) \
  X(CPC_k) \
  X(CPC_l) \
  X(CPC_m) \
  X(CPC_n) \
  X(CPC_o) \
  X(CPC_p) \
  X(CPC_q) \
  X(CPC_r) \
  X(CPC_s) \
  X(CPC_t) \
  X(CPC_u) \
  X(CPC_v) \
  X(CPC_w) \
  X(CPC_x) \
  X(CPC_y) \
  X(CPC_z) \
  X(CPC_CTRL_a) \
  X(CPC_CTRL_b) \
  X(CPC_CTRL_c) \
  X(CPC_CTRL_d) \
  X(CPC_CTRL_e) \
  X(CPC_CTRL_f) \
  X(CPC_CTRL_g) \
  X(CPC_CTRL_h) \
  X(CPC_CTRL_i) \
  X(CPC_CTRL_j) \
  X(CPC_CTRL_k) \
  X(CPC_CTRL_l) \
  X(CPC_CTRL_m) \
  X(CPC_CTRL_n) \
  X(CPC_CTRL_o) \
  X(CPC_CTRL_p) \
  X(CPC_CTRL_q) \
  X(CPC_CTRL_r) \
  X(CPC_CTRL_s) \
  X(CPC_CTRL_t) \
  X(CPC_CTRL_u) \
  X(CPC_CTRL_v) \
  X(CPC_CTRL_w) \
  X(CPC_CTRL_x) \
  X(CPC_CTRL_y) \
  X(CPC_CTRL_z) \
  X(CPC_CTRL_0) \
  X(CPC_CTRL_1) \
  X(CPC_CTRL_2) \
  X(CPC_CTRL_3) \
  X(CPC_CTRL_4) \
  X(CPC_CTRL_5) \
  X(CPC_CTRL_6) \
  X(CPC_CTRL_7) \
  X(CPC_CTRL_8) \
  X(CPC_CTRL_9) \
  X(CPC_CTRL_UP) \
  X(CPC_CTRL_DOWN) \
  X(CPC_CTRL_LEFT) \
  X(CPC_CTRL_RIGHT) \
  X(CPC_AMPERSAND) \
  X(CPC_ASTERISK) \
  X(CPC_AT) \
  X(CPC_BACKQUOTE) \
  X(CPC_BACKSLASH) \
  X(CPC_CAPSLOCK) \
  X(CPC_CLR) \
  X(CPC_COLON) \
  X(CPC_COMMA) \
  X(CPC_CONTROL) \
  X(CPC_COPY) \
  X(CPC_CPY_DOWN) \
  X(CPC_CPY_LEFT) \
  X(CPC_CPY_RIGHT) \
  X(CPC_CPY_UP) \
  X(CPC_CUR_DOWN) \
  X(CPC_CUR_LEFT) \
  X(CPC_CUR_RIGHT) \
  X(CPC_CUR_UP) \
  X(CPC_CUR_ENDBL) \
  X(CPC_CUR_HOMELN) \
  X(CPC_CUR_ENDLN) \
  X(CPC_CUR_HOMEBL) \
  X(CPC_DBLQUOTE) \
  X(CPC_DEL) \
  X(CPC_DOLLAR) \
  X(CPC_ENTER) \
  X(CPC_EQUAL) \
  X(CPC_ESC) \
  X(CPC_EXCLAMATN) \
  X(CPC_F0) \
  X(CPC_F1) \
  X(CPC_F2) \
  X(CPC_F3) \
  X(CPC_F4) \
  X(CPC_F5) \
  X(CPC_F6) \
  X(CPC_F7) \
  X(CPC_F8) \
  X(CPC_F9) \
  X(CPC_CTRL_F0) \
  X(CPC_CTRL_F1) \
  X(CPC_CTRL_F2) \
  X(CPC_CTRL_F3) \
  X(CPC_CTRL_F4) \
  X(CPC_CTRL_F5) \
  X(CPC_CTRL_F6) \
  X(CPC_CTRL_F7) \
  X(CPC_CTRL_F8) \
  X(CPC_CTRL_F9) \
  X(CPC_SHIFT_F0) \
  X(CPC_SHIFT_F1) \
  X(CPC_SHIFT_F2) \
  X(CPC_SHIFT_F3) \
  X(CPC_SHIFT_F4) \
  X(CPC_SHIFT_F5) \
  X(CPC_SHIFT_F6) \
  X(CPC_SHIFT_F7) \
  X(CPC_SHIFT_F8) \
  X(CPC_SHIFT_F9) \
  X(CPC_FPERIOD) \
  X(CPC_GREATER) \
  X(CPC_HASH) \
  X(CPC_LBRACKET) \
  X(CPC_LCBRACE) \
  X(CPC_LEFTPAREN) \
  X(CPC_LESS) \
  X(CPC_LSHIFT) \
  X(CPC_MINUS) \
  X(CPC_PERCENT) \
  X(CPC_PERIOD) \
  X(CPC_PIPE) \
  X(CPC_PLUS) \
  X(CPC_POUND) \
  X(CPC_POWER) \
  X(CPC_QUESTION) \
  X(CPC_QUOTE) \
  X(CPC_RBRACKET) \
  X(CPC_RCBRACE) \
  X(CPC_RETURN) \
  X(CPC_RIGHTPAREN) \
  X(CPC_RSHIFT) \
  X(CPC_SEMICOLON) \
  X(CPC_SLASH) \
  X(CPC_SPACE) \
  X(CPC_TAB) \
  X(CPC_UNDERSCORE) \
  X(CPC_J0_UP) \
  X(CPC_J0_DOWN) \
  X(CPC_J0_LEFT) \
  X(CPC_J0_RIGHT) \
  X(CPC_J0_FIRE1) \
  X(CPC_J0_FIRE2) \
  X(CPC_J1_UP) \
  X(CPC_J1_DOWN) \
  X(CPC_J1_LEFT) \
  X(CPC_J1_RIGHT) \
  X(CPC_J1_FIRE1) \
  X(CPC_J1_FIRE2) \
  X(CPC_ES_NTILDE) \
  X(CPC_ES_nTILDE) \
  X(CPC_ES_PESETA) \
  X(CPC_FR_eACUTE) \
  X(CPC_FR_eGRAVE) \
  X(CPC_FR_cCEDIL) \
  X(CPC_FR_aGRAVE) \
  X(CPC_FR_uGRAVE)

// Emulator commands (fullscreen, reset, screenshot, ...). Values sit above
// MOD_EMU_KEY so a CapriceKey can be told apart from a CPC key.
// NOLINTNEXTLINE(performance-enum-size): enumerators combine with MOD_* scancode
// bits in CPCScancode arithmetic; narrowing the base type breaks key decoding.
enum KONCPC_KEYS : std::uint16_t {
  KONCPC_KEYS_BASE = MOD_EMU_KEY - 1,  // sentinel: first real key follows
#define KONCPC_AS_ENUM(k) k,
  KONCPC_KEYS_LIST(KONCPC_AS_ENUM)
#undef KONCPC_AS_ENUM
};

#ifdef __cplusplus
extern "C" {
#endif
void koncpc_menu_action(int action);
#ifdef __cplusplus
}
#endif

// Every key (and shifted/control combination) of the CPC keyboard.
// NOLINTNEXTLINE(performance-enum-size): CPC_KEYS values are OR'd with MOD_CPC_*
// bits to form CPCScancodes; narrowing the base type corrupts that arithmetic.
enum CPC_KEYS : std::uint8_t {
#define CPC_AS_ENUM(k) k,
  // NOLINTNEXTLINE(misc-confusable-identifiers): distinct identifiers; unambiguous in context
  CPC_KEYS_LIST(CPC_AS_ENUM)
#undef CPC_AS_ENUM
};

// Number of different keys on a CPC keyboard.
inline constexpr int CPC_KEY_NUM = CPC_FR_uGRAVE + 1;
static_assert(CPC_KEY_NUM == 209, "CPC_KEYS_LIST changed size");
// Number of different keyboards supported (English, French, Spanish).
inline constexpr int CPC_KEYBOARD_NUM = 3;

void applyKeypressDirect(CPCScancode cpc_key,
                         std::atomic<byte> keyboard_matrix[], bool pressed,
                         bool release_modifiers = true);
void applyKeypress(CPCScancode cpc_key, std::atomic<byte> keyboard_matrix[],
                   bool pressed, bool release_modifiers = true);

class LineParsingResult {
 public:
  bool valid = true;
  bool contains_mapping = false;
  CapriceKey cpc_key = 0;
  PCKey sdl_key = 0;
  std::string cpc_key_name;
  std::string sdl_key_name;
};

class InputMapper {
 private:
  static const std::array<std::array<CPCScancode, CPC_KEY_NUM>,
                          CPC_KEYBOARD_NUM>
      cpc_kbd;
  static const std::map<const std::string, const CapriceKey> CPCkeysFromStrings;
  static const std::map<const std::string, const PCKey> SDLkeysFromStrings;
  static const std::map<char, CPC_KEYS> CPCkeysFromChars;
  std::map<char, std::pair<SDL_Keycode, SDL_Keymod>> SDLkeysFromChars;
  static std::map<CapriceKey, PCKey> SDLkeysymFromCPCkeys_us;
  std::map<PCKey, CapriceKey> CPCkeysFromSDLkeysym;
  std::map<CapriceKey, PCKey> SDLkeysymFromCPCkeys;
  t_CPC* CPC;

  LineParsingResult process_cfg_line(const std::string& line);

 public:
  InputMapper(t_CPC* CPC);
  bool load_layout(const std::string& filename);
  void init();
  CPCScancode CPCscancodeFromCPCkey(CPC_KEYS cpc_key);
  CPCScancode CPCscancodeFromKeysym(SDL_Keycode key, SDL_Keymod mod);
  CapriceKey CPCkeyFromKeysym(SDL_Keycode key, SDL_Keymod mod);
  std::string CPCkeyToString(const CapriceKey cpc_key);
  CPCScancode CPCscancodeFromJoystickButton(SDL_JoyButtonEvent jbutton);
  void CPCscancodeFromJoystickAxis(SDL_JoyAxisEvent jaxis, CPCScancode* cpc_key,
                                   bool& release);
  void set_joystick_emulation();
  // Human-readable host shortcut(s) currently bound to an emulator command,
  // derived from the live binding map so menu/UI hints can never drift from the
  // real keys.  Returns e.g. "F5", "Shift+F2 / F12", or "" if unbound.
  std::string shortcutForAction(CapriceKey action) const;
};

// Keystone helper: the single source of truth for an action's shortcut DISPLAY
// string, derived from the real binding (not hand-typed per surface).  Every
// menu/topbar/popup/command-palette surface renders hints via this.
std::string koncpc_action_shortcut(KONCPC_KEYS action);
