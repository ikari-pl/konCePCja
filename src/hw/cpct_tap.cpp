/* cpct_tap.cpp — CPCT bus tap. The rule is in cpct_tap.h. */

#include "cpct_tap.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "buses.h"

namespace {

struct tap_state {
  uint64_t now = 0;      // master cycles since reset
  uint8_t active = 0;    // flags of the access in progress, 0 = none
  uint64_t start = 0;    // master cycle the access began
  uint16_t addr = 0;
  uint8_t data = 0;
  size_t capacity = 1u << 22;
  size_t dropped = 0;
  std::vector<CpctRecord> rec;
};

tap_state* self_of(void* self) { return static_cast<tap_state*>(self); }

void emit(tap_state* t) {
  if (t->rec.size() < t->capacity)
    t->rec.push_back(CpctRecord{static_cast<uint32_t>(t->start / 4), t->active,
                                t->data, t->addr});
  else
    t->dropped++;
}

void tap_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  (void)out;  // drives nothing
  tap_state* t = self_of(self);
  const CpuBus& c = in->cpu;
  uint8_t f = 0;
  if (c.mreq && c.rd) f = c.m1 ? (CPCT_M1 | CPCT_MEM_RD) : CPCT_MEM_RD;
  else if (c.mreq && c.wr) f = CPCT_MEM_WR;
  else if (c.iorq && c.rd && !c.m1) f = CPCT_IO_RD;
  else if (c.iorq && c.wr && !c.m1) f = CPCT_IO_WR;

  if (f != t->active) {
    if (t->active) emit(t);
    t->active = f;
    t->start = t->now;
  }
  if (f) {
    t->addr = c.addr;
    t->data = c.data;
  }
  t->now++;
}

void tap_reset(void* self) {
  tap_state* t = self_of(self);
  t->now = 0;
  t->active = 0;
  t->rec.clear();
  t->dropped = 0;
}

// A tap's records are not machine state: nothing is saved.
size_t tap_save_size(const void*) { return 1; }
void tap_save(const void*, void* buf) { static_cast<uint8_t*>(buf)[0] = 1; }
void tap_load(void*, const void*) {}

void put_u16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put_u32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF; }
void put_u64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (v >> (8 * i)) & 0xFF; }

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

const CpctRecord* cpct_tap_records(const Device* dev, size_t* count, size_t* dropped) {
  const tap_state* t = self_of(dev->self);
  if (count) *count = t->rec.size();
  if (dropped) *dropped = t->dropped;
  return t->rec.data();
}

void cpct_tap_set_capacity(const Device* dev, size_t capacity) {
  self_of(dev->self)->capacity = capacity;
}

int cpct_tap_write(const Device* dev, const char* path, uint8_t machine) {
  const tap_state* t = self_of(dev->self);
  FILE* f = std::fopen(path, "wb");
  if (!f) return -1;
  uint8_t h[32] = {};
  std::memcpy(h, "CPCT", 4);
  put_u16(h + 4, 0);
  h[6] = 0;        // crtc_type: HD6845S
  h[7] = machine;  // 2 = 6128
  put_u32(h + 8, 4000000);
  put_u64(h + 12, 0);
  std::fwrite(h, 1, 32, f);
  uint8_t r[8];
  for (const CpctRecord& x : t->rec) {
    put_u32(r, x.cycle);
    r[4] = x.flags;
    r[5] = x.data;
    put_u16(r + 6, x.addr);
    std::fwrite(r, 1, 8, f);
  }
  std::fclose(f);
  return 0;
}

}  // extern "C"
