/* amdrum.cpp — the AmDrum DAC Device. See docs/hardware/amdrum-device.md. */

#include "amdrum.h"

#include <cstring>
#include <new>

namespace {

struct amdrum_state {
  uint8_t dac = 128;         // mid-scale = silence (the golden master's reset)
  uint8_t plugged = 0;       // presence on the expansion port
  bool access_prev = false;  // previous cycle was an owned write access
};

amdrum_state* self_of(void* self) { return static_cast<amdrum_state*>(self); }

void amdrum_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  (void)out;  // write-only latch: the Device never drives the bus
  amdrum_state* a = self_of(self);
  if (!a->plugged) return;  // nothing on the port: the space stays empty
  // All upper address bits high on an I/O write — the uncontested space
  // (spec §1). An IACK also drives iorq (with m1) and must not be decoded.
  const bool sel = in->cpu.iorq && !in->cpu.m1 && in->cpu.wr &&
                   (in->cpu.addr & 0xFF00) == 0xFF00;
  const bool edge = sel && !a->access_prev;
  a->access_prev = sel;
  if (edge) a->dac = in->cpu.data;
}

void amdrum_dev_reset(void* self) {
  amdrum_state* a = self_of(self);
  a->dac = 128;  // the plugged state persists: a reset is not an unplug
  a->access_prev = false;
}

size_t amdrum_dev_state_size(const void* /*unused*/) {
  return sizeof(amdrum_state) + 1;
}
void amdrum_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(amdrum_state));
}
void amdrum_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, sizeof(amdrum_state));
}

}  // namespace

extern "C" {

size_t amdrum_state_size(void) { return sizeof(amdrum_state); }

Device amdrum_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  amdrum_state* a = new (storage) amdrum_state();
  return Device{a,
                "amdrum",
                amdrum_tick,
                amdrum_dev_reset,
                amdrum_dev_state_size,
                amdrum_save,
                amdrum_load};
}

void amdrum_peek(const Device* dev, AmdrumRegs* out) {
  const amdrum_state* a = static_cast<const amdrum_state*>(dev->self);
  out->dac = a->dac;
  out->plugged = a->plugged;
}

void amdrum_set_plugged(const Device* dev, int on) {
  static_cast<amdrum_state*>(dev->self)->plugged = on ? 1 : 0;
}

}  // extern "C"
