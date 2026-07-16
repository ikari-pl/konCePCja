// LayeredMemory - views over CPC memory.
// CPU-view vs direct debug vs raw bus.

#pragma once

#include "memory_bus.h"
#include "types.h"
#include "z80_view.h"

// LayeredMemory is a thin, allocation-free facade over the existing
// global accessors and MemoryBus. It just forwards calls.
struct LayeredMemory {
  // Direct debug view (old IPC behavior):
  // - read: SmartWatch on upper ROM, no watchpoints, no MF2/ASIC
  // - write: raw MemoryBus, no watchpoints, no MF2/ASIC
  byte read_direct(word addr) const { return z80_read_mem(addr); }

  void write_direct(word addr, byte v) const { z80_write_mem(addr, v); }

  // CPU view: full device layering without watchpoints.
  // - read: SmartWatch on upper ROM, raw bus otherwise
  // - write: MF2 RAM redirect + ASIC register page + bus + IPC events
  // Safe for IPC and UI — won't trigger watchpoint hits.
  byte read_cpu(word addr) const { return z80_cpu_read_mem(addr); }

  void write_cpu(word addr, byte v) const { z80_cpu_write_mem(addr, v); }

  // Raw bus views (no devices, no watchpoints).
  byte read_raw(word addr) const { return g_memory_bus.read_raw(addr); }

  void write_raw(word addr, byte v) const { g_memory_bus.write_raw(addr, v); }

  byte read_raw_via_write_bank(word addr) const {
    return g_memory_bus.read_raw_via_write_bank(addr);
  }
};
