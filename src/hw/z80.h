/* z80.h — the Z80 CPU as a Device. See docs/hardware/z80.md and
 * docs/hw-spec.md.
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

/* Architectural register snapshot (for tests / IPC; not the serialized blob).
 */
typedef struct Z80Regs {
  uint16_t af, bc, de, hl, af_, bc_, de_, hl_, ix, iy, sp, pc, wz;
  uint8_t i, r, im, iff1, iff2, q, halted;
  uint64_t tstates; /* T-states executed since reset */
  uint64_t
      instr_count; /* instructions completed since reset (a finish() each) */
  uint8_t last_opcode;   /* most recently decoded opcode (bring-up aid) */
  uint8_t unimplemented; /* 1 if the last decode hit an undecoded opcode */
} Z80Regs;

size_t z80_state_size(void);
Device z80_init(void* storage);
void z80_peek(const Device* dev, Z80Regs* out);

/* Load architectural state (inverse of z80_peek) and reset the engine to a
 * clean instruction boundary, with `tstates`/`instr_count` zeroed. For test
 * oracles (e.g. FUSE) that set up a starting state and run one instruction.
 * Only the register/flag/interrupt fields of *in are consumed. */
void z80_poke(const Device* dev, const Z80Regs* in);

/* --- Batch (instruction-granularity) execution — the RunTier::Fast driver.
 *
 * Runs the SAME micro-op sequences as the per-cycle engine (dual-mode, not a
 * second interpreter — z80.md §batch), satisfying each M-cycle synchronously
 * through these seams instead of the two-phase bus. `now` is z80's `tstates`
 * timestamp of the access, chosen per seam to match what the two-phase bus
 * delivers (the Fast scheduler catches devices up to `now`, then applies):
 *  - mem_read/mem_write: the M-cycle's (grid-aligned) T1;
 *  - io_write:           the I/O cycle's T1 — the CPU drives the strobes from
 *    T1 and drive-and-hold keeps them up through the Tw stretch, so snooping
 *    devices see the write one hop after T1 (a write at T-state tau lands
 *    after CRTC char floor(tau/4) — crtc.h §batch);
 *  - io_read:            the released auto-Tw slot, where the responding
 *    device's driven value is what the CPU's sample latches;
 *  - int_ack (optional, may be null): the INT-acknowledge M-cycle's T1 — the
 *    M1+IORQ signature devices like the Gate Array clear their counters on
 *    (ga_batch_int_ack). Fires for MASKABLE acceptance only, and RETURNS the
 *    data-bus vector the acknowledge latches (the Plus ASIC drives its IM2
 *    vector there; a classic machine returns the floating 0xFF) — computed
 *    post-catch-up, exactly as the bus delivers it. When null, the
 *    z80_batch_step irq_vector parameter is latched instead. */
typedef struct Z80BatchIO {
  void* ctx;
  uint8_t (*mem_read)(void* ctx, uint16_t addr, uint64_t now);
  void (*mem_write)(void* ctx, uint16_t addr, uint8_t val, uint64_t now);
  uint8_t (*io_read)(void* ctx, uint16_t port, uint64_t now);
  void (*io_write)(void* ctx, uint16_t port, uint8_t val, uint64_t now);
  uint8_t (*int_ack)(void* ctx, uint64_t now);
} Z80BatchIO;

/* Execute ONE instruction (or one accepted interrupt) in batch mode and
 * return the T-states consumed (also added to the peekable `tstates`).
 *
 * `irq` is the INT line level sampled at this instruction boundary;
 * `irq_vector` is the data-bus byte an acknowledge would latch (the CPC
 * floats 0xFF). NMI is edge-triggered: latch it with z80_batch_nmi().
 *
 * `cpc_grid` applies the Gate Array timing fabric as closed-form arithmetic,
 * exactly mirroring the per-cycle quantiser (z80.cpp): memory-class M-cycles
 * (M1/READ/WRITE/int-ack) hold T1 to the 4 T-state µs grid; INTERNAL cycles
 * run free; I/O cycles free-run T1 and stretch their automatic Tw to the
 * released /WAIT slot (T-slot 1 of the µs — gate_array.h). This requires the
 * caller to keep `tstates` µs-locked: tstates % 4 == the machine's T-slot
 * (true from reset/poke; frame boundaries are µs multiples; a DMA bus steal
 * must re-establish it). With `cpc_grid` false, raw datasheet timing (FUSE).
 *
 * When halted with no acceptable interrupt this returns 0 and consumes
 * nothing — advance halted time explicitly with z80_batch_halt(), which
 * keeps the R-refresh cadence (one bump per 4 halted T-states). */
uint32_t z80_batch_step(const Device* dev, const Z80BatchIO* io, int irq,
                        uint8_t irq_vector, int cpc_grid);
void z80_batch_halt(const Device* dev, uint32_t tstates);

/* Latch an NMI edge for acceptance at the next z80_batch_step boundary (the
 * scheduler's edge event — z80.md §batch). */
void z80_batch_nmi(const Device* dev);

/* Nonzero when the engine sits at a clean instruction boundary from which
 * z80_batch_step may drive it: a fresh M1 with no prefix, index, or interrupt
 * sequence mid-flight (a halted CPU qualifies — batch handles HALT). The
 * per-cycle quantiser's pre-T1 hold cycles keep this true, so a scheduler
 * ticking per-cycle reaches a ready point within one instruction. */
int z80_batch_ready(const Device* dev);

/* Cheap boundary-loop accessors: the batch scheduler polls these once per
 * instruction, where a full z80_peek struct copy is measurable overhead. */
uint64_t z80_batch_tstates(const Device* dev);
int z80_batch_halted(const Device* dev);
int z80_batch_iff1(const Device* dev);
uint16_t z80_batch_pc(const Device* dev);

/* Nonzero when the NEXT z80_batch_step would accept a pending interrupt (NMI
 * latched, or irq asserted with IFF1 set and no EI shadow) instead of
 * fetching the instruction at PC. The batch scheduler's firmware-vector taps
 * gate on this: a preempted fetch must not fire a tap (per-cycle, the probe
 * only latches the M1 that actually happens). Mirrors the acceptance branch
 * in z80_batch_step exactly. */
int z80_batch_will_accept(const Device* dev, int irq);

/* Hand the bus back after a batch run: clears the drive-and-hold latch so a
 * resuming per-cycle tick re-drives nothing stale from before the batch. */
void z80_batch_release_bus(const Device* dev);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_Z80_H */
