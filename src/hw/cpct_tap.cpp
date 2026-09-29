/* cpct_tap.cpp — CPCT bus tap. The rule is in cpct_tap.h. */

#include "cpct_tap.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "buses.h"

namespace {

// Line events that fell inside an open access wait here until its record is
// out; more than this many edges inside one access merge into the last slot.
constexpr int kMaxPendingLines = 4;

struct tap_state {
  uint64_t now = 0;        // master cycles since reset
  uint8_t active = 0;      // access flags of the access in progress, 0 = none
  bool halt_seen = false;  // /HALT low at some point during the open access
  uint64_t start = 0;      // master cycle the access began
  uint16_t addr = 0;
  uint8_t data = 0;
  bool prev_irq = false;
  bool prev_reset = false;
  CpctRecord pending[kMaxPendingLines] = {};
  int npending = 0;
  size_t capacity = 1u << 22;
  size_t dropped = 0;
  uint32_t gap_cycle = 0;  // cycle of the first dropped record
  std::vector<CpctRecord> rec;
};

tap_state* self_of(void* self) { return static_cast<tap_state*>(self); }

uint32_t tstate(uint64_t master) { return static_cast<uint32_t>(master / 4); }

void push(tap_state* t, const CpctRecord& r) {
  if (t->rec.size() < t->capacity) {
    t->rec.push_back(r);
  } else {
    // The GAP record is stamped with the cycle the loss began (the spec's
    // "first cycle after the gap"), not the cycle the file is written on.
    if (t->dropped == 0) t->gap_cycle = r.cycle;
    t->dropped++;
  }
}

void emit_access(tap_state* t) {
  const uint8_t flags = t->active | (t->halt_seen ? CPCT_HALT : 0);
  push(t, CpctRecord{tstate(t->start), flags, t->data, t->addr});
}

void flush_pending(tap_state* t) {
  for (int i = 0; i < t->npending; i++) push(t, t->pending[i]);
  t->npending = 0;
}

void line_event(tap_state* t, uint8_t flags) {
  const CpctRecord r{tstate(t->now), flags, 0, 0};
  // An open access (even one that opened on this very cycle) is not out yet:
  // hold the marker behind its record, as the rule in cpct_tap.h says.
  if (t->active) {
    if (t->npending < kMaxPendingLines)
      t->pending[t->npending++] = r;
    else
      t->pending[kMaxPendingLines - 1].flags |= flags;
    return;
  }
  push(t, r);
}

void tap_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  (void)out;  // drives nothing
  tap_state* t = self_of(self);
  const CpuBus& c = in->cpu;
  uint8_t f = 0;
  if (c.mreq && c.rd)
    f = c.m1 ? (CPCT_M1 | CPCT_MEM_RD) : CPCT_MEM_RD;
  else if (c.mreq && c.wr)
    f = CPCT_MEM_WR;
  else if (c.iorq && c.rd && !c.m1)
    f = CPCT_IO_RD;
  else if (c.iorq && c.wr && !c.m1)
    f = CPCT_IO_WR;

  if (f != t->active) {
    if (t->active) emit_access(t);
    flush_pending(t);
    t->active = f;
    t->start = t->now;
    t->halt_seen = false;
  }
  if (f) {
    t->addr = c.addr;
    t->data = c.data;
    if (c.halt) t->halt_seen = true;
  }

  uint8_t line = 0;
  if (c.irq && !t->prev_irq) line |= CPCT_INT;
  if (c.reset && !t->prev_reset) line |= CPCT_RESET;
  t->prev_irq = c.irq;
  t->prev_reset = c.reset;
  if (line) line_event(t, line | (c.halt ? CPCT_HALT : 0));

  t->now++;
}

void tap_reset(void* self) {
  tap_state* t = self_of(self);
  t->now = 0;
  t->active = 0;
  t->halt_seen = false;
  t->prev_irq = false;
  t->prev_reset = false;
  t->npending = 0;
  t->rec.clear();
  t->dropped = 0;
  t->gap_cycle = 0;
  push(t, CpctRecord{0, CPCT_RESET, 0, 0});  // the capture starts at power-on
}

// A tap's records are not machine state: nothing is saved.
size_t tap_save_size(const void* /*self*/) { return 1; }
void tap_save(const void* /*self*/, void* buf) {
  static_cast<uint8_t*>(buf)[0] = 1;
}
void tap_load(void* /*self*/, const void* /*buf*/) {}

void put_u16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}
void put_u32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF;
}
void put_u64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

}  // namespace

extern "C" {

size_t cpct_tap_state_size(void) { return sizeof(tap_state); }

Device cpct_tap_init(void* storage) {
  tap_state* t = new (storage) tap_state();
  Device dev = {};
  dev.self = t;
  dev.name = "cpct-tap";
  dev.tick = tap_tick;
  dev.reset = tap_reset;
  dev.state_size = tap_save_size;
  dev.save = tap_save;
  dev.load = tap_load;
  return dev;
}

const CpctRecord* cpct_tap_records(const Device* dev, size_t* count,
                                   size_t* dropped) {
  const tap_state* t = self_of(dev->self);
  if (count) *count = t->rec.size();
  if (dropped) *dropped = t->dropped;
  return t->rec.data();
}

void cpct_tap_set_capacity(const Device* dev, size_t capacity) {
  self_of(dev->self)->capacity = capacity;
}

void cpct_tap_destroy(const Device* dev) {
  if (dev && dev->self) self_of(dev->self)->~tap_state();
}

void cpct_encode_header(uint8_t out[CPCT_HEADER_SIZE], uint8_t crtc_type,
                        uint8_t machine, uint64_t start_cycle) {
  std::memset(out, 0, CPCT_HEADER_SIZE);
  std::memcpy(out, "CPCT", 4);
  put_u16(out + 4, 0);  // version
  out[6] = crtc_type;
  out[7] = machine;
  put_u32(out + 8, 4000000);  // crystal_hz: the T-state clock
  put_u64(out + 12, start_cycle);
}

void cpct_encode_record(uint8_t out[CPCT_RECORD_SIZE], const CpctRecord* r) {
  put_u32(out, r->cycle);
  out[4] = r->flags;
  out[5] = r->data;
  put_u16(out + 6, r->addr);
}

int cpct_tap_write(const Device* dev, const char* path, uint8_t crtc_type,
                   uint8_t machine) {
  const tap_state* t = self_of(dev->self);
  FILE* f = std::fopen(path, "wb");
  if (!f) return -1;
  bool ok = true;
  uint8_t h[CPCT_HEADER_SIZE];
  cpct_encode_header(h, crtc_type, machine, 0);
  ok = ok && std::fwrite(h, 1, sizeof h, f) == sizeof h;
  uint8_t r[CPCT_RECORD_SIZE];
  for (size_t i = 0; ok && i < t->rec.size(); i++) {
    cpct_encode_record(r, &t->rec[i]);
    ok = std::fwrite(r, 1, sizeof r, f) == sizeof r;
  }
  // Line events still queued behind the open access are complete events:
  // write them (the open access itself has not ended, so it is not a record
  // yet). Past the capacity they count as lost like any other record.
  size_t lost_total = t->dropped;
  uint32_t gap_cycle = t->gap_cycle;
  size_t room =
      t->capacity > t->rec.size() ? t->capacity - t->rec.size() : size_t{0};
  for (int i = 0; ok && i < t->npending; i++) {
    if (room == 0) {
      if (lost_total == 0) gap_cycle = t->pending[i].cycle;
      lost_total++;
      continue;
    }
    room--;
    cpct_encode_record(r, &t->pending[i]);
    ok = std::fwrite(r, 1, sizeof r, f) == sizeof r;
  }
  if (ok && lost_total) {
    const uint16_t lost =
        lost_total >= 0xFFFF ? 0xFFFF : static_cast<uint16_t>(lost_total);
    const CpctRecord gap{gap_cycle, CPCT_GAP, 0, lost};
    cpct_encode_record(r, &gap);
    ok = std::fwrite(r, 1, sizeof r, f) == sizeof r;
  }
  if (std::fclose(f) != 0) ok = false;
  // A trace cut short by a failed write parses as a shorter but valid capture
  // (v0 has no record count or trailer): remove it rather than leave a
  // plausible partial oracle on disk.
  if (!ok) std::remove(path);
  return ok ? 0 : -1;
}

}  // extern "C"
