/* light_gun.cpp — the CPC light gun Device. See
 * docs/hardware/light-gun-device.md. */

#include "light_gun.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace {

struct light_gun_state {
  uint8_t type = 0;  // 0=off, 1=Magnum, 2=Trojan; also the plugged flag
  uint8_t pressed = 0;
  uint16_t aim_line = 0;
  uint16_t aim_col = 0;

  // Beam-column tracking: the active char column advances while dispen is high
  // and resets at the start of each line (dispen falling, or a scanline move).
  uint16_t active_col = 0;
  bool dispen_prev = false;
  uint16_t line_prev = 0;
};

light_gun_state* self_of(void* self) {
  return static_cast<light_gun_state*>(self);
}

void light_gun_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  light_gun_state* g = self_of(self);
  if (g->type == 0) return;  // unplugged: never drives the strobe

  // The host feeds aim_col in character columns
  // (docs/hardware/light-gun-device.md §4), so advance the beam-column tracker
  // once per CRTC character.  In the per-cycle paths this means gating on
  // clk.crtc; in the batch Fast path the caller ticks us once per
  // crtc_advance_view char.
  if (!in->clk.crtc) return;

  const uint16_t line = in->vid.frame_line;
  const bool dispen = in->vid.dispen;

  // New line (scanline moved) or the active window ended: restart the column.
  if (line != g->line_prev || (!dispen && g->dispen_prev)) g->active_col = 0;
  // Advance one column per active-display cycle.
  else if (dispen && g->dispen_prev)
    g->active_col++;
  g->dispen_prev = dispen;
  g->line_prev = line;

  // Fire the LPEN strobe when the trigger is held and the beam is within the
  // aim tolerance window (±2 line / ±2 column — spec §3).
  if (g->pressed) {
    const int dl = static_cast<int>(line) - static_cast<int>(g->aim_line);
    const int dc =
        static_cast<int>(g->active_col) - static_cast<int>(g->aim_col);
    if (std::abs(dl) < 2 && std::abs(dc) < 2) out->pen.strobe = true;
  }
}

void light_gun_dev_reset(void* self) {
  light_gun_state* g = self_of(self);
  // type persists (a reset is not an unplug); clear the transient trigger/beam.
  g->pressed = 0;
  g->active_col = 0;
  g->dispen_prev = false;
  g->line_prev = 0;
}

size_t light_gun_dev_state_size(const void* /*unused*/) {
  return sizeof(light_gun_state) + 1;
}
void light_gun_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(light_gun_state));
}
void light_gun_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, sizeof(light_gun_state));
}

}  // namespace

extern "C" {

size_t light_gun_state_size(void) { return sizeof(light_gun_state); }

Device light_gun_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  light_gun_state* g = new (storage) light_gun_state();
  return Device{g,
                "light-gun",
                light_gun_tick,
                light_gun_dev_reset,
                light_gun_dev_state_size,
                light_gun_save,
                light_gun_load};
}

void light_gun_peek(const Device* dev, LightGunRegs* out) {
  const light_gun_state* g = static_cast<const light_gun_state*>(dev->self);
  out->type = g->type;
  out->pressed = g->pressed;
  out->aim_line = g->aim_line;
  out->aim_col = g->aim_col;
  out->plugged = g->type != 0 ? 1 : 0;
}

void light_gun_set_type(const Device* dev, int type) {
  static_cast<light_gun_state*>(dev->self)->type =
      static_cast<uint8_t>(type & 0xFF);
}

void light_gun_set_aim(const Device* dev, uint16_t line, uint16_t col) {
  light_gun_state* g = static_cast<light_gun_state*>(dev->self);
  g->aim_line = line;
  g->aim_col = col;
}

void light_gun_set_trigger(const Device* dev, int pressed) {
  static_cast<light_gun_state*>(dev->self)->pressed = pressed ? 1 : 0;
}

}  // extern "C"
