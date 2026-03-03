// MemoryBus - thin, non-owning view over CPC memory banking.
// Phase 2: helper only, not yet wired into hot paths.
//
// Design notes for humans:
// - This is intentionally just a pair of pointers to the existing
//   membank_read/membank_write arrays (4 x 16KB banks selected by addr>>14).
// - It does NOT know about SmartWatch, ASIC, or watchpoints; those stay
//   in the higher-level accessors in z80.cpp for now.
// - The goal is to make it easy to thread a single "bus" object through
//   helpers and tools without changing the memory layout or timing.

#ifndef MEMORY_BUS_H
#define MEMORY_BUS_H

#include "types.h"

struct MemoryBus {
  // Pointers to the 16KB bank tables used by the core:
  //   read_banks [0..3]  -> base of each readable bank
  //   write_banks[0..3]  -> base of each writable bank
  byte* const* read_banks  = nullptr;
  byte* const* write_banks = nullptr;

  // Raw banked access using the existing 4x16KB scheme.
  // Callers are expected to layer any device/watchpoint logic on top.
  inline byte read_raw(word addr) const {
    return *(read_banks[addr >> 14] + (addr & 0x3fff));
  }

  inline void write_raw(word addr, byte v) const {
    *(write_banks[addr >> 14] + (addr & 0x3fff)) = v;
  }

  // Convenience helpers to get the base pointer for a 16KB bank.
  inline byte* bank_read_ptr(int bank) const {
    return read_banks[bank];
  }

  inline byte* bank_write_ptr(int bank) const {
    return write_banks[bank];
  }
};

extern MemoryBus g_memory_bus;

#endif // MEMORY_BUS_H

