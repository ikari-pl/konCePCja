// IoBus - thin wrapper around IO dispatch.
// Phase 2: helper only, Z80 still calls z80_IN/OUT handlers directly.
//
// This provides a place to hang IO abstractions without changing how
// io_dispatch works today.

#ifndef IO_BUS_H
#define IO_BUS_H

#include "z80.h"
#include "io_dispatch.h"

struct IoBus {
  inline byte in(reg_pair port, byte current_val) const {
    return io_dispatch_in(port, current_val);
  }

  inline void out(reg_pair port, byte val) const {
    io_dispatch_out(port, val);
  }
};

extern IoBus g_io_bus;

#endif // IO_BUS_H

