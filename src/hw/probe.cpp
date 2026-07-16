/* probe.cpp — ICE-style bus probe. THE SPEC: docs/hardware/probe-device.md. */

#include "probe.h"

#include <cstring>
#include <new>

#include "buses.h"

namespace {

constexpr int kMaxExec = 32;
constexpr int kMaxWatch = 16;
constexpr int kMaxIo = 8;

struct WatchEntry {
  uint16_t addr;
  uint16_t len;  // 1 = point; >1 = addr..addr+len-1
  uint8_t on_read, on_write;
};
struct IoEntry {
  uint16_t value, mask;
  uint8_t on_read, on_write;
};

struct probe_state {
  uint64_t now = 0;  // master cycles since init (the latch timestamp base)

  // Comparator tables (bench setup: survives target reset).
  uint16_t exec[kMaxExec] = {};
  uint8_t exec_count = 0;
  WatchEntry watch[kMaxWatch] = {};
  uint8_t watch_count = 0;
  IoEntry io[kMaxIo] = {};
  uint8_t io_count = 0;

  // Previous-cycle strobe states (edge detection across held/stretched cycles).
  uint8_t prev_fetch = 0, prev_mrd = 0, prev_mwr = 0, prev_iord = 0,
          prev_iowr = 0;

  ProbeHit hit = {};  // hit.kind == PROBE_HIT_NONE → not latched

  // Notify taps (trigger-out): one-cycle flag, independent of the halt latch.
  uint16_t tap[4] = {};
  uint8_t tap_count = 0;
  uint16_t tap_hit_addr = 0;
  uint8_t tap_hit = 0;
};

probe_state* self_of(void* self) { return static_cast<probe_state*>(self); }

void latch(probe_state* p, uint8_t kind, uint16_t addr, uint8_t data) {
  p->hit.kind = kind;
  p->hit.addr = addr;
  p->hit.data = data;
  p->hit.cycle = p->now;
}

void probe_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  (void)out;  // infinite input impedance: the probe never drives the bus
  probe_state* p = self_of(self);

  // Fully idle: no breakpoints, watchpoints, IO breaks, taps, or latched hit.
  // There is nothing to detect and the probe drives nothing, so this is a pure
  // no-op — now and the edge latches stay frozen. That makes TICKING an idle
  // probe byte-identical to SKIPPING it, which lets the board drop the probe
  // from the per-cycle loop while unarmed (recompose_active) with the soldered
  // fast path staying identical (it still lists the probe; the tick no-ops).
  // now is read only as a hit's timestamp (latch()), so freezing it while there
  // can be no hit is unobservable. On arm, the first live cycle re-baselines
  // the edge latches (a strobe already high reads as no rising edge — the same
  // instruction boundary the CPU is mid-fetch of, not a spurious hit).
  if (p->exec_count == 0 && p->watch_count == 0 && p->io_count == 0 &&
      p->hit.kind == PROBE_HIT_NONE) {
    return;
  }

  p->now++;

  // Qualified conditions this cycle.
  const uint8_t fetch = (in->cpu.m1 && in->cpu.mreq && in->cpu.rd) ? 1 : 0;
  const uint8_t mrd = (in->cpu.mreq && in->cpu.rd) ? 1 : 0;
  const uint8_t mwr = (in->cpu.mreq && in->cpu.wr) ? 1 : 0;
  const uint8_t iord = (in->cpu.iorq && in->cpu.rd && !in->cpu.m1) ? 1 : 0;
  const uint8_t iowr = (in->cpu.iorq && in->cpu.wr && !in->cpu.m1) ? 1 : 0;

  // Rising edges (a Z80 access holds its strobes across many master cycles).
  const bool fetch_edge = fetch && !p->prev_fetch;
  const bool mrd_edge = mrd && !p->prev_mrd;
  const bool mwr_edge = mwr && !p->prev_mwr;
  const bool iord_edge = iord && !p->prev_iord;
  const bool iowr_edge = iowr && !p->prev_iowr;
  p->prev_fetch = fetch;
  p->prev_mrd = mrd;
  p->prev_mwr = mwr;
  p->prev_iord = iord;
  p->prev_iowr = iowr;

  if (p->hit.kind != PROBE_HIT_NONE) return;  // latched: matching suspended
  if (p->exec_count == 0 && p->watch_count == 0 && p->io_count == 0) return;

  const uint16_t addr = in->cpu.addr;
  const uint8_t data = in->cpu.data;

  // Table order resolves simultaneous matches: exec, then memory, then I/O.
  if (fetch_edge) {
    for (int i = 0; i < p->exec_count; ++i) {
      if (p->exec[i] == addr) {
        latch(p, PROBE_HIT_EXEC, addr, data);
        return;
      }
    }
  }
  if (mrd_edge || mwr_edge) {
    for (int i = 0; i < p->watch_count; ++i) {
      // 32-bit end: a range reaching the top of memory (e.g. 0xFFF0 len
      // 0x20) must not wrap to a tiny end and match nothing.
      const uint32_t end =
          static_cast<uint32_t>(p->watch[i].addr) + p->watch[i].len - 1;
      if (addr < p->watch[i].addr || addr > end) continue;
      if (mrd_edge && p->watch[i].on_read) {
        latch(p, PROBE_HIT_MEM_READ, addr, data);
        return;
      }
      if (mwr_edge && p->watch[i].on_write) {
        latch(p, PROBE_HIT_MEM_WRITE, addr, data);
        return;
      }
    }
  }
  if (iord_edge || iowr_edge) {
    for (int i = 0; i < p->io_count; ++i) {
      if ((addr & p->io[i].mask) != (p->io[i].value & p->io[i].mask)) continue;
      if (iord_edge && p->io[i].on_read) {
        latch(p, PROBE_HIT_IO_READ, addr, data);
        return;
      }
      if (iowr_edge && p->io[i].on_write) {
        latch(p, PROBE_HIT_IO_WRITE, addr, data);
        return;
      }
    }
  }
}

// A CPC reset does not reconfigure the bench: comparators stay, the latch
// clears (the run the hit belonged to is gone).
void probe_reset(void* self) {
  probe_state* p = self_of(self);
  p->hit = ProbeHit{};
  p->prev_fetch = p->prev_mrd = p->prev_mwr = p->prev_iord = p->prev_iowr = 0;
}

// probe_state is pointer-free (the tap tables are uint16_t addresses, not
// callbacks), so the whole struct is logical state. Prefix the mandated 1-byte
// format version (device.h:38) like every other device.
size_t probe_save_size(const void* self) {
  (void)self;
  return sizeof(probe_state) + 1;
}
void probe_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(probe_state));
}
void probe_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] != 1) return;
  std::memcpy(self, b + 1, sizeof(probe_state));
}

}  // namespace

extern "C" {

size_t probe_state_size(void) { return sizeof(probe_state); }

Device probe_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  probe_state* p = new (storage) probe_state();
  Device dev = {};
  dev.self = p;
  dev.name = "probe";
  dev.tick = probe_tick;
  dev.reset = probe_reset;
  dev.state_size = probe_save_size;
  dev.save = probe_save;
  dev.load = probe_load;
  return dev;
}

int probe_add_exec(const Device* dev, uint16_t addr) {
  probe_state* p = self_of(dev->self);
  for (int i = 0; i < p->exec_count; ++i)
    if (p->exec[i] == addr) return -1;
  if (p->exec_count >= kMaxExec) return -1;
  p->exec[p->exec_count++] = addr;
  return 0;
}

int probe_del_exec(const Device* dev, uint16_t addr) {
  probe_state* p = self_of(dev->self);
  for (int i = 0; i < p->exec_count; ++i) {
    if (p->exec[i] == addr) {
      p->exec[i] = p->exec[--p->exec_count];
      return 0;
    }
  }
  return -1;
}

void probe_clear_exec(const Device* dev) { self_of(dev->self)->exec_count = 0; }

int probe_list_exec(const Device* dev, uint16_t* out, int max) {
  probe_state const* p = self_of(dev->self);
  int n = 0;
  for (int i = 0; i < p->exec_count && n < max; ++i) out[n++] = p->exec[i];
  return n;
}

int probe_add_watch(const Device* dev, uint16_t addr, uint16_t len,
                    uint8_t on_read, uint8_t on_write) {
  probe_state* p = self_of(dev->self);
  if (len == 0) len = 1;
  if (!on_read && !on_write) return -1;
  for (int i = 0; i < p->watch_count; ++i) {
    if (p->watch[i].addr != addr || p->watch[i].len != len) continue;
    // Same range added again: merge directions instead of dropping the
    // second watchpoint (a READ and a WRITE watch may share a range).
    p->watch[i].on_read |= on_read;
    p->watch[i].on_write |= on_write;
    return 0;
  }
  if (p->watch_count >= kMaxWatch) return -1;
  p->watch[p->watch_count++] = WatchEntry{addr, len, on_read, on_write};
  return 0;
}

int probe_del_watch(const Device* dev, uint16_t addr) {
  probe_state* p = self_of(dev->self);
  for (int i = 0; i < p->watch_count; ++i) {
    if (p->watch[i].addr == addr) {
      p->watch[i] = p->watch[--p->watch_count];
      return 0;
    }
  }
  return -1;
}

void probe_clear_watch(const Device* dev) {
  self_of(dev->self)->watch_count = 0;
}

int probe_list_watch(const Device* dev, uint16_t* out, int max) {
  probe_state const* p = self_of(dev->self);
  int n = 0;
  for (int i = 0; i < p->watch_count && n < max; ++i)
    out[n++] = p->watch[i].addr;
  return n;
}

int probe_add_io(const Device* dev, uint16_t value, uint16_t mask,
                 uint8_t on_read, uint8_t on_write) {
  probe_state* p = self_of(dev->self);
  for (int i = 0; i < p->io_count; ++i)
    if (p->io[i].value == value && p->io[i].mask == mask) return -1;
  if (p->io_count >= kMaxIo || (!on_read && !on_write)) return -1;
  p->io[p->io_count++] = IoEntry{value, mask, on_read, on_write};
  return 0;
}

int probe_del_io(const Device* dev, uint16_t value, uint16_t mask) {
  probe_state* p = self_of(dev->self);
  for (int i = 0; i < p->io_count; ++i) {
    if (p->io[i].value == value && p->io[i].mask == mask) {
      p->io[i] = p->io[--p->io_count];
      return 0;
    }
  }
  return -1;
}

void probe_clear_io(const Device* dev) { self_of(dev->self)->io_count = 0; }

int probe_add_tap(const Device* dev, uint16_t addr) {
  probe_state* p = self_of(dev->self);
  for (int i = 0; i < p->tap_count; ++i)
    if (p->tap[i] == addr) return -1;
  if (p->tap_count >= 4) return -1;
  p->tap[p->tap_count++] = addr;
  return 0;
}

void probe_clear_taps(const Device* dev) {
  probe_state* p = self_of(dev->self);
  p->tap_count = 0;
  p->tap_hit = 0;
}

int probe_tap_take(const Device* dev, uint16_t* addr) {
  probe_state* p = self_of(dev->self);
  if (!p->tap_hit) return 0;
  p->tap_hit = 0;
  if (addr != nullptr) *addr = p->tap_hit_addr;
  return 1;
}

int probe_pending(const Device* dev, ProbeHit* out) {
  probe_state const* p = self_of(dev->self);
  if (p->hit.kind == PROBE_HIT_NONE) return 0;
  if (out != nullptr) *out = p->hit;
  return 1;
}

void probe_ack(const Device* dev) { self_of(dev->self)->hit = ProbeHit{}; }

int probe_armed(const Device* dev) {
  probe_state const* p = self_of(dev->self);
  return (p->exec_count || p->watch_count || p->io_count) ? 1 : 0;
}

int probe_active(const Device* dev) {
  // Broader than probe_armed(): a latched hit also keeps the probe live.
  // Matches probe_tick's idle predicate exactly — while this is 0 the tick is a
  // no-op, so the board may skip it (recompose_active). Console taps no longer
  // count: they fire from the machine's own fetch-edge check (service_taps),
  // not the probe, so a tap-only machine keeps the probe dormant and Wake
  // elision available.
  probe_state const* p = self_of(dev->self);
  return (p->exec_count || p->watch_count || p->io_count ||
          p->hit.kind != PROBE_HIT_NONE)
             ? 1
             : 0;
}

}  // extern "C"
