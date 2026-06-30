/* z80core.h — language-neutral C ABI contract for a swappable Z80 CPU core.
 *
 * This header is THE seam of the clean-room Z80 effort (see
 * docs/plans/2026-06-30-z80-cleanroom-core.md). Any implementation that exports
 * these symbols with C linkage — C++, Ada/SPARK, Rust — can be linked in as the
 * CPU and selected at build or run time. The CPC machine talks to the CPU ONLY
 * through this contract; the CPU reaches the machine ONLY through the bus
 * callbacks below.
 *
 * Everything is prefixed `z80core_` so it never collides with the legacy
 * Caprice32 core (which owns the bare `z80_*` namespace, e.g. `z80_reset()`).
 *
 * No C++ types, no global state: the boundary is plain C so a formally-verified
 * SPARK/Ada core can `pragma Export` these entry points and `Import` the bus
 * callbacks (Convention => C). The bus callbacks are named by what they access
 * (`mem_read`/`mem_write`, `io_read`/`io_write`) — symmetric, and free of any
 * C/Ada/Rust reserved word, so a GNAT record maps them 1:1. The callbacks sit
 * OUTSIDE a SPARK proof boundary — they are the environment the verified CPU
 * logic runs against.
 *
 * Timing granularity: this contract is *per-M-cycle*, not per-T-state — the core
 * reports a whole machine cycle's worth of T-states to `tick` after each bus
 * access. That is sufficient to match the legacy Caprice32 oracle (itself not
 * T-state-exact); finer sub-instruction granularity is a future, ABI-compatible
 * option (tick is already a running count).
 *
 * Interrupt-line ownership: the maskable-INT and NMI *lines* are owned by the
 * machine (a device asserts them via z80core_irq/z80core_nmi) and are therefore
 * NOT part of z80core_regs. A save-state restores CPU architectural + sequencing
 * state (including iff_delay); the machine re-asserts any pending line after
 * restore. The CPU's internal sequencing latch that callers CANNOT re-supply —
 * the one-boundary EI/DI deferral — IS serialized, as `iff_delay`.
 */
#ifndef KONCPC_Z80CORE_H
#define KONCPC_Z80CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bus the CPU drives. The machine supplies the callbacks plus an opaque
 * context pointer; these calls are the CPU's only window onto memory, I/O and
 * the passage of time. `tick` is how a cycle-stepped core advances the rest of
 * the machine (gate array, CRTC, interrupt generation) in lockstep.
 *
 * `mem_read`, `mem_write`, `io_read` and `io_write` are REQUIRED (must be
 * non-NULL). `tick` is OPTIONAL (may be NULL) for callers that don't model time. */
typedef struct z80core_bus {
  void* ctx;
  uint8_t (*mem_read)(void* ctx, uint16_t addr);             /* memory read  (required) */
  void (*mem_write)(void* ctx, uint16_t addr, uint8_t val);  /* memory write (required) */
  uint8_t (*io_read)(void* ctx, uint16_t port);              /* I/O read     (required) */
  void (*io_write)(void* ctx, uint16_t port, uint8_t val);   /* I/O write    (required) */
  void (*tick)(void* ctx, int32_t tstates);                  /* advance N T-states (optional) */
} z80core_bus;

/* Canonical, language-neutral snapshot of ALL observable + internal CPU state.
 * The differential oracle harness and the IPC `regs` command use this to compare
 * any two implementations bit-for-bit. Deliberately a flat POD so an Ada record
 * can map it 1:1. The "internal" fields (wz, q, iff_delay) are what separate a
 * merely plausible Z80 from a correct one — they are kept here from day one
 * because retrofitting the published struct later would break every consumer. */
typedef struct z80core_regs {
  uint16_t af, bc, de, hl;     /* main register file                          */
  uint16_t af_, bc_, de_, hl_; /* alternate set (AF', BC', DE', HL')           */
  uint16_t ix, iy, sp, pc;
  uint16_t wz;                 /* MEMPTR — sets XF/YF after BIT n,(HL)         */
  uint8_t i, r;                /* interrupt vector / memory-refresh registers  */
  uint8_t im;                  /* interrupt mode 0/1/2                         */
  uint8_t iff1, iff2;          /* interrupt-enable flip-flops                  */
  uint8_t iff_delay;           /* 1 = maskable IRQ deferred one boundary (EI)  */
  uint8_t q;                   /* last-flags-touched latch (SCF/CCF quirk)     */
  uint8_t halted;              /* 1 while in HALT                              */
  uint64_t tstates;            /* cumulative T-state counter                   */
} z80core_regs;

/* Register ids for scalar get/set (harness + IPC poke). */
typedef enum z80core_reg_id {
  Z80CORE_AF, Z80CORE_BC, Z80CORE_DE, Z80CORE_HL,
  Z80CORE_AF_, Z80CORE_BC_, Z80CORE_DE_, Z80CORE_HL_,
  Z80CORE_IX, Z80CORE_IY, Z80CORE_SP, Z80CORE_PC,
  Z80CORE_WZ, Z80CORE_IR
} z80core_reg_id;

/* Opaque handle; each implementation owns its own internal layout. */
typedef struct z80core_state z80core_state;

/* ---- The contract every implementation MUST export ---- */

/* Returns NULL if `bus` is NULL, a required callback is NULL, or allocation
 * fails. `bus` must outlive the returned state. */
z80core_state* z80core_create(const z80core_bus* bus);
void z80core_destroy(z80core_state* cpu);

void z80core_reset(z80core_state* cpu);

/* Execute exactly ONE instruction (including any prefix bytes and the
 * post-instruction interrupt acceptance check). Returns T-states consumed,
 * all of which have already been reported to the machine via bus->tick. */
int32_t z80core_step(z80core_state* cpu);

/* Assert the maskable interrupt line. `bus_value` is the byte the device would
 * place on the data bus (the IM2 vector low byte / IM0 opcode). */
void z80core_irq(z80core_state* cpu, uint8_t bus_value);
void z80core_nmi(z80core_state* cpu);

/* Canonical-state I/O: the basis of differential testing and snapshots. */
void z80core_snapshot(const z80core_state* cpu, z80core_regs* out);
void z80core_restore(z80core_state* cpu, const z80core_regs* in);

uint16_t z80core_get_reg(const z80core_state* cpu, z80core_reg_id id);
void z80core_set_reg(z80core_state* cpu, z80core_reg_id id, uint16_t value);

/* Bring-up aid: returns 1 if the most recent z80core_step hit an opcode the
 * implementation does not yet decode, and writes that opcode to *opcode_out
 * (if non-NULL). Lets the differential harness stop at the FIRST unimplemented
 * instruction instead of chasing a PC desync. Returns 0 otherwise. */
int z80core_last_unimplemented(const z80core_state* cpu, uint8_t* opcode_out);

/* Identifies the linked implementation, e.g. "cpp-cleanroom", "spark-ada",
 * "legacy-caprice32". Surfaced in logs and the IPC `version` reply. */
const char* z80core_impl_name(void);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_Z80CORE_H */
