/* printer.cpp — the printer port latch Device. See
 * docs/hardware/printer-device.md. */

#include "printer.h"

#include <cstring>
#include <new>

namespace {

struct printer_state {
  uint8_t latch = 0xFF;      // data ^ 0x80 (reset value per the golden master)
  bool access_prev = false;  // previous cycle was an owned write access
  uint64_t now = 0;          // master-cycle clock for event timestamps

  // Strobe-edge byte ring (live telemetry, never serialized): sits AFTER the
  // serialized prefix, see kSaveBytes below.
  PrinterEvent ev[64];
  uint8_t ev_head = 0, ev_count = 0;
};

printer_state* self_of(void* self) { return static_cast<printer_state*>(self); }

void push_event(printer_state* p, uint8_t byte) {
  constexpr uint8_t kCap = 64;
  if (p->ev_count == kCap) {  // full: sacrifice the oldest
    p->ev_head = static_cast<uint8_t>((p->ev_head + 1) % kCap);
    p->ev_count--;
  }
  p->ev[(p->ev_head + p->ev_count) % kCap] = PrinterEvent{p->now, byte};
  p->ev_count++;
}

void printer_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  (void)out;  // write-only latch: the Device never drives the bus
  printer_state* p = self_of(self);
  p->now++;
  // A12 = 0 selects the latch on an I/O write (partial decode, spec §1). An
  // interrupt acknowledge also drives iorq (with m1) and must not be decoded.
  const bool sel =
      in->cpu.iorq && !in->cpu.m1 && in->cpu.wr && (in->cpu.addr & 0x1000) == 0;
  const bool edge = sel && !p->access_prev;  // one latch update per access
  p->access_prev = sel;
  if (!edge) return;
  const uint8_t prev = p->latch;
  p->latch = in->cpu.data ^ 0x80;  // the base machine inverts /STROBE
  // A printer clocks the byte on the falling edge of /STROBE: latched bit 7
  // 1 -> 0 (spec §2).
  if ((prev & 0x80) != 0 && (p->latch & 0x80) == 0)
    push_event(p, p->latch & 0x7F);
}

void printer_dev_reset(void* self) {
  printer_state* p = self_of(self);
  p->latch = 0xFF;
  p->access_prev = false;
  p->ev_head = p->ev_count = 0;  // the ring restarts with the machine
}

// Serialize only the latch and clock — everything BEFORE `ev` (the ring is
// live telemetry).
constexpr size_t kSaveBytes = offsetof(printer_state, ev);

size_t printer_dev_state_size(const void* /*unused*/) { return kSaveBytes + 1; }
void printer_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, kSaveBytes);
}
void printer_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, kSaveBytes);
}

}  // namespace

extern "C" {

size_t printer_state_size(void) { return sizeof(printer_state); }

Device printer_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  printer_state* p = new (storage) printer_state();
  return Device{p,
                "printer",
                printer_tick,
                printer_dev_reset,
                printer_dev_state_size,
                printer_save,
                printer_load};
}

void printer_peek(const Device* dev, PrinterRegs* out) {
  const printer_state* p = static_cast<const printer_state*>(dev->self);
  out->latch = p->latch;
}

void printer_poke_latch(const Device* dev, uint8_t latch) {
  static_cast<printer_state*>(dev->self)->latch = latch;
}

int printer_drain_events(const Device* dev, PrinterEvent* out, int max) {
  printer_state* p = static_cast<printer_state*>(dev->self);
  int n = 0;
  while (n < max && p->ev_count > 0) {
    out[n++] = p->ev[p->ev_head];
    p->ev_head = static_cast<uint8_t>((p->ev_head + 1) % 64);
    p->ev_count--;
  }
  return n;
}

void printer_advance(const Device* dev, uint64_t skipped_cycles) {
  // Wake-scheduler catch-up (printer.h): the skipped ticks would each have done
  // exactly `now++` (no strobe was on the bus, so no decode, no latch, no
  // event). Re-applying the count before the next real tick keeps PrinterEvent
  // timestamps identical to a per-cycle run.
  static_cast<printer_state*>(dev->self)->now += skipped_cycles;
}

}  // extern "C"
