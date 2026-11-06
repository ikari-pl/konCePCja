// joy_key_capture.h - The Settings "Stick presses these keys" capture: which
// host key, and which modifiers were held when it went down, a stick slot is
// bound to. Fed from the SDL event pump's KEY_DOWN/KEY_UP events (not a
// per-frame keyboard-state diff), so auto-repeat is visible and ignored, and
// the modifiers are the ones of the press itself. Pure (SDL types only, no
// SDL calls), so test/joy_key_capture_test.cpp drives it without a window.
#pragma once

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

#include <bitset>
#include <optional>

// A finished capture: bind `scancode` (with `mods` held) to stick slot `slot`
// (0 up, 1 down, 2 left, 3 right, 4 fire1, 5 fire2).
struct JoyKeyBinding {
  int slot;
  SDL_Scancode scancode;
  SDL_Keymod mods;
};

class JoyKeyCapture {
 public:
  bool armed() const { return slot_ >= 0; }
  int slot() const { return slot_; }

  // Wait for a key for `slot`, dropping any capture already under way.
  void arm(int slot) {
    disarm();
    slot_ = slot;
    taken_.reset();
  }

  // Stop waiting and forget a binding nobody took.
  void disarm() {
    finish();
    binding_.reset();
  }

  // Key down from the event pump. A non-modifier key binds at once, with the
  // modifiers of that press; a modifier only becomes the binding if it is
  // released with nothing pressed after it (see on_key_up). Esc cancels.
  // Returns true when the capture consumed the event (it is armed), so
  // neither the UI nor the CPC sees it.
  bool on_key_down(SDL_Scancode scancode, SDL_Keymod mods, bool repeat) {
    if (!armed()) return false;
    if (repeat) return true;
    mark_taken(scancode);
    if (scancode == SDL_SCANCODE_ESCAPE) {
      finish();
    } else if (is_modifier(scancode)) {
      pending_modifier_ = scancode;
    } else {
      binding_ = JoyKeyBinding{slot_, scancode, mods};
      finish();
    }
    return true;
  }

  // Key up from the event pump: releasing the last modifier pressed, with no
  // other key in between, binds that modifier on its own. Only the key-up of
  // a press the capture took is consumed: a key held before arming (the
  // Space or Return that activated the slot button) must reach the UI, or
  // the UI thinks it stays down.
  bool on_key_up(SDL_Scancode scancode) {
    if (!armed() || !was_taken(scancode)) return false;
    taken_.reset(static_cast<size_t>(scancode));
    if (pending_modifier_ != SDL_SCANCODE_UNKNOWN &&
        scancode == pending_modifier_) {
      binding_ = JoyKeyBinding{slot_, scancode, SDL_KMOD_NONE};
      finish();
    }
    return true;
  }

  // The binding the last capture produced, handed out once.
  std::optional<JoyKeyBinding> take_binding() {
    std::optional<JoyKeyBinding> out = binding_;
    binding_.reset();
    return out;
  }

  // The UI calls mark_shown() on every frame it draws the slot buttons, and
  // end_frame() once per frame after it. A frame without the buttons (tree
  // node collapsed, another mode or tab, Settings closed) disarms, so a
  // capture never outlives the widget that armed it.
  void mark_shown() { shown_ = true; }
  void end_frame() {
    if (!shown_) disarm();
    shown_ = false;
  }

  static bool is_modifier(SDL_Scancode scancode) {
    switch (scancode) {
      case SDL_SCANCODE_LSHIFT:
      case SDL_SCANCODE_RSHIFT:
      case SDL_SCANCODE_LCTRL:
      case SDL_SCANCODE_RCTRL:
      case SDL_SCANCODE_LALT:
      case SDL_SCANCODE_RALT:
      case SDL_SCANCODE_LGUI:
      case SDL_SCANCODE_RGUI:
        return true;
      default:
        return false;
    }
  }

 private:
  void finish() {
    slot_ = -1;
    pending_modifier_ = SDL_SCANCODE_UNKNOWN;
  }

  void mark_taken(SDL_Scancode scancode) {
    if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT)
      taken_.set(static_cast<size_t>(scancode));
  }
  bool was_taken(SDL_Scancode scancode) const {
    return scancode >= 0 && scancode < SDL_SCANCODE_COUNT &&
           taken_.test(static_cast<size_t>(scancode));
  }

  int slot_ = -1;
  SDL_Scancode pending_modifier_ = SDL_SCANCODE_UNKNOWN;
  // Keys whose key-down this arming consumed, so their key-up is ours too.
  std::bitset<SDL_SCANCODE_COUNT> taken_;
  std::optional<JoyKeyBinding> binding_;
  bool shown_ = false;
};

// The one capture: the event pump (kon_cpc_ja.cpp) feeds it, the Settings
// window (imgui_ui.cpp) arms it and takes the binding. Both run on the main
// thread.
extern JoyKeyCapture g_joy_key_capture;
