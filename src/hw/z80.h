/* z80.h — the Z80 CPU as a Device. See docs/hardware/z80.md and docs/hw-spec.md.
 *
 * Caller-owned, no heap: allocate z80_state_size() bytes, hand them to
 * z80_init(storage), get a ready Device. z80_peek() exposes architectural state
 * for tests and the IPC `regs` command. */
#ifndef KONCPC_HW_Z80_H
#define KONCPC_HW_Z80_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Architectural register snapshot (for tests / IPC; not the serialized blob). */
typedef struct Z80Regs {
  uint16_t af, bc, de, hl, af_, bc_, de_, hl_, ix, iy, sp, pc, wz;
  uint8_t i, r, im, iff1, iff2, q, halted;
  uint64_t tstates;     /* T-states executed since reset */
  uint64_t instr_count; /* instructions completed since reset (a finish() each) */
  uint8_t last_opcode;  /* most recently decoded opcode (bring-up aid) */
  uint8_t unimplemented; /* 1 if the last decode hit an undecoded opcode */
} Z80Regs;

size_t z80_state_size(void);
Device z80_init(void* storage);
void z80_peek(const Device* dev, Z80Regs* out);

/* Load architectural state (inverse of z80_peek) and reset the engine to a clean
 * instruction boundary, with `tstates`/`instr_count` zeroed. For test oracles
 * (e.g. FUSE) that set up a starting state and run one instruction. Only the
 * register/flag/interrupt fields of *in are consumed. */
void z80_poke(const Device* dev, const Z80Regs* in);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_Z80_H */
