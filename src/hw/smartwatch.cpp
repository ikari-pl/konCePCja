/* smartwatch.cpp — the DS1216 phantom RTC Device. See
 * docs/hardware/smartwatch-device.md. */

#include "smartwatch.h"

#include <cstring>
#include <new>

namespace {

// DS1216 recognition pattern: C5 3A A3 5C C5 3A A3 5C, LSB first per byte.
constexpr uint64_t kPattern = 0x5CA33AC55CA33AC5ULL;

enum : uint8_t { SW_IDLE = 0, SW_MATCHING = 1, SW_READING = 2 };

struct sw_state {
  uint8_t state = SW_IDLE;
  uint8_t bit_index = 0;
  uint64_t shift = 0;
  uint8_t time_bcd[8] = {0};  // host-fed wall clock (spec §4)
  uint8_t snap[8] = {0};      // latched on a pattern match
  uint8_t plugged = 0;
  uint8_t upper_rom_off = 0;  // snooped GA config bit 3 (the socket's /CE)
  // The in-flight overridden access (spec §3).
  uint8_t drive = 0;      // overriding the current access' D0
  uint8_t drive_bit = 0;  // the time bit for this access
  uint8_t have_byte = 0;  // the ROM chip's byte has been latched
  uint8_t rom_byte = 0;
  bool acc_prev = false;  // memory-read access seen last tick
  bool io_prev = false;   // I/O write access seen last tick
};

sw_state* self_of(void* self) { return static_cast<sw_state*>(self); }

// One protocol step — exactly the golden master's FSM (spec §2). Returns
// whether THIS access' D0 must carry a time bit.
bool fsm_step(sw_state* w, bool a0, bool a2) {
  switch (w->state) {
    case SW_IDLE:
      if (!a2) {
        w->state = SW_MATCHING;
        w->shift = a0 ? 1u : 0u;
        w->bit_index = 1;
      }
      return false;
    case SW_MATCHING:
      if (a2) {  // a read-mode access resets the match
        w->state = SW_IDLE;
        w->bit_index = 0;
        return false;
      }
      w->shift |= static_cast<uint64_t>(a0 ? 1 : 0) << w->bit_index;
      if (++w->bit_index == 64) {
        if (w->shift == kPattern) {
          std::memcpy(w->snap, w->time_bcd, 8);  // burst atomicity (spec §4)
          w->state = SW_READING;
        } else {
          w->state = SW_IDLE;
        }
        w->bit_index = 0;
      }
      return false;
    case SW_READING:
    default:
      if (!a2) {  // abort: this bit starts a fresh match
        w->state = SW_MATCHING;
        w->shift = a0 ? 1u : 0u;
        w->bit_index = 1;
        return false;
      }
      w->drive_bit = (w->snap[w->bit_index / 8] >> (w->bit_index % 8)) & 1;
      if (++w->bit_index >= 64) {
        w->state = SW_IDLE;
        w->bit_index = 0;
      }
      return true;
  }
}

void sw_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  sw_state* w = self_of(self);
  if (!w->plugged) return;  // empty socket adapter

  // Reconstruct the socket's /CE: track the GA's upper-ROM disable bit from
  // the same I/O writes the GA decodes (spec §1).
  const bool io_wr = in->cpu.iorq && !in->cpu.m1 && in->cpu.wr;
  if (io_wr && !w->io_prev && (in->cpu.addr & 0xC000) == 0x4000 &&
      (in->cpu.data >> 6) == 2)
    w->upper_rom_off = (in->cpu.data >> 3) & 1;
  w->io_prev = io_wr;

  const bool access = in->cpu.mreq && in->cpu.rd && !in->cpu.rfsh &&
                      in->cpu.addr >= 0xC000 && !w->upper_rom_off;
  const bool edge = access && !w->acc_prev;
  w->acc_prev = access;
  if (!access) {
    w->drive = 0;  // the access ended: stop overriding
    w->have_byte = 0;
    return;
  }
  if (edge)
    w->drive =
        fsm_step(w, (in->cpu.addr & 0x01) != 0, (in->cpu.addr & 0x04) != 0) ? 1
                                                                            : 0;
  if (!w->drive) return;

  // Let the ROM chip answer first, then override D0 (spec §3): the memory
  // Device's byte is on the committed bus one tick after the strobes.
  out->cpu.romdis = true;
  if (!edge && !w->have_byte) {
    w->rom_byte = in->cpu.data;  // the chip's answer, committed last tick
    w->have_byte = 1;
  }
  if (w->have_byte)
    out->cpu.data = static_cast<uint8_t>((w->rom_byte & 0xFE) | w->drive_bit);
}

void sw_dev_reset(void* self) {
  sw_state* w = self_of(self);
  // The golden master's smartwatch_reset: FSM to idle; plugged and the
  // host-fed clock persist.
  w->state = SW_IDLE;
  w->bit_index = 0;
  w->shift = 0;
  w->drive = 0;
  w->drive_bit = 0;
  w->have_byte = 0;
  w->acc_prev = false;
  w->io_prev = false;
}

size_t sw_dev_state_size(const void* /*unused*/) {
  return sizeof(sw_state) + 1;
}
void sw_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(sw_state));
}
void sw_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, sizeof(sw_state));
}

}  // namespace

extern "C" {

size_t smartwatch_state_size(void) { return sizeof(sw_state); }

Device smartwatch_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self (void*), cannot be const
  sw_state *w = new (storage) sw_state();
  return Device{w,       "smartwatch", sw_tick, sw_dev_reset, sw_dev_state_size,
                sw_save, sw_load};
}

void smartwatch_peek(const Device* dev, SmartwatchRegs* out) {
  const sw_state* w = static_cast<const sw_state*>(dev->self);
  out->plugged = w->plugged;
  out->state = w->state;
  out->bit_index = w->bit_index;
}

void smartwatch_set_time(const Device* dev, const uint8_t bcd[8]) {
  std::memcpy(static_cast<sw_state*>(dev->self)->time_bcd, bcd, 8);
}

void smartwatch_set_plugged(const Device* dev, int on) {
  static_cast<sw_state*>(dev->self)->plugged = on ? 1 : 0;
}

}  // extern "C"
