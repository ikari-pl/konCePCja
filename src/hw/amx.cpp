/* amx.cpp — the AMX mouse Device. See docs/hardware/amx-mouse-device.md. */

#include "amx.h"

#include <cstring>
#include <new>

namespace {

struct amx_state {
  int16_t mickeys_x = 0;  // pending counts, signed (opposite signs cancel)
  int16_t mickeys_y = 0;
  uint8_t buttons = 0;  // host mask: bit0 left, bit1 middle, bit2 right
  uint8_t plugged = 0;
  uint8_t row9_now = 0;    // row 9 currently selected (monostable input)
  uint8_t row9_armed = 0;  // deselect seen: the next select consumes counts
};

amx_state* self_of(void* self) { return static_cast<amx_state*>(self); }

void amx_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  amx_state* a = self_of(self);
  if (!a->plugged) return;  // nothing on the connector

  // The interface's monostable (spec §2): a row-9 deselect arms it; the
  // next select consumes one mickey per axis.
  const uint8_t sel = (in->ay.kbd_row & 0x0F) == 9 ? 1 : 0;
  if (!sel && a->row9_now) a->row9_armed = 1;
  if (sel && !a->row9_now && a->row9_armed) {
    if (a->mickeys_x > 0)
      a->mickeys_x--;
    else if (a->mickeys_x < 0)
      a->mickeys_x++;
    if (a->mickeys_y > 0)
      a->mickeys_y--;
    else if (a->mickeys_y < 0)
      a->mickeys_y++;
    a->row9_armed = 0;
  }
  a->row9_now = sel;

  if (!sel) return;      // our columns only drive while our row is selected
  uint8_t lines = 0xFF;  // active-low
  if (a->mickeys_y < 0) lines &= ~(1u << 0);  // up
  if (a->mickeys_y > 0) lines &= ~(1u << 1);  // down
  if (a->mickeys_x < 0) lines &= ~(1u << 2);  // left
  if (a->mickeys_x > 0) lines &= ~(1u << 3);  // right
  if (a->buttons & 1) lines &= ~(1u << 4);    // left button  → Fire2
  if (a->buttons & 4) lines &= ~(1u << 5);    // right button → Fire1
  if (a->buttons & 2) lines &= ~(1u << 6);    // middle       → Fire3
  out->ay.row_ext = static_cast<uint8_t>(out->ay.row_ext & lines);  // wired-AND
}

void amx_dev_reset(void* self) {
  amx_state* a = self_of(self);
  // Counters and the monostable clear; plugged persists (a reset does not
  // unplug the connector).
  a->mickeys_x = a->mickeys_y = 0;
  a->buttons = 0;
  a->row9_now = a->row9_armed = 0;
}

size_t amx_dev_state_size(const void* /*unused*/) {
  return sizeof(amx_state) + 1;
}
void amx_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(amx_state));
}
void amx_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, sizeof(amx_state));
}

}  // namespace

extern "C" {

size_t amx_state_size(void) { return sizeof(amx_state); }

Device amx_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  amx_state* a = new (storage) amx_state();
  return Device{a,        "amx",   amx_tick, amx_dev_reset, amx_dev_state_size,
                amx_save, amx_load};
}

void amx_peek(const Device* dev, AmxRegs* out) {
  const amx_state* a = static_cast<const amx_state*>(dev->self);
  out->plugged = a->plugged;
  out->mickeys_x = a->mickeys_x;
  out->mickeys_y = a->mickeys_y;
  out->buttons = a->buttons;
}

void amx_feed(const Device* dev, int dx, int dy, uint8_t buttons) {
  amx_state* a = static_cast<amx_state*>(dev->self);
  long const x = a->mickeys_x + dx;
  long const y = a->mickeys_y + dy;
  a->mickeys_x =
      // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): nested
      // conditional kept intentionally; no clang-tidy auto-fix
      static_cast<int16_t>(x < -32768 ? -32768 : (x > 32767 ? 32767 : x));
  a->mickeys_y =
      // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): nested
      // conditional kept intentionally; no clang-tidy auto-fix
      static_cast<int16_t>(y < -32768 ? -32768 : (y > 32767 ? 32767 : y));
  a->buttons = buttons;
}

void amx_set_plugged(const Device* dev, int on) {
  static_cast<amx_state*>(dev->self)->plugged = on ? 1 : 0;
}

}  // extern "C"
