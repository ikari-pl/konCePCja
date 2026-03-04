// LayeredMemory - views over CPC memory.
// CPU-view vs direct debug vs raw bus.

#ifndef LAYERED_MEMORY_H
#define LAYERED_MEMORY_H

#include "types.h"
#include "memory_bus.h"
#include "z80.h"

// LayeredMemory is a thin, allocation-free facade over the existing
// global accessors and MemoryBus. It just forwards calls.
struct LayeredMemory {
  // Direct debug view (old IPC behavior):
  // - read: SmartWatch on upper ROM, no watchpoints
  // - write: raw MemoryBus, no watchpoints
  inline byte read_direct(word addr) const {
    return z80_read_mem(addr);
  }

  inline void write_direct(word addr, byte v) const {
    z80_write_mem(addr, v);
  }

  // CPU view: full layering (watchpoints + SmartWatch/MF2/ASIC + bus).
  inline byte read_cpu(word addr) const {
    return z80_cpu_read_mem(addr);
  }

  inline void write_cpu(word addr, byte v) const {
    z80_cpu_write_mem(addr, v);
  }

  // Raw bus views (no devices, no watchpoints).
  inline byte read_raw(word addr) const {
    return g_memory_bus.read_raw(addr);
  }

  inline void write_raw(word addr, byte v) const {
    g_memory_bus.write_raw(addr, v);
  }

  inline byte read_raw_via_write_bank(word addr) const {
    return g_memory_bus.read_raw_via_write_bank(addr);
  }
};

#endif // LAYERED_MEMORY_H

