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
 * The two can coexist in one binary while the new core is brought up behind the
 * differential harness.
 *
 * No C++ types, no global state: the boundary is plain C so a formally-verified
 * SPARK/Ada core can `pragma Export` these entry points and `Import` the bus
 * callbacks (Convention => C). The bus callbacks sit OUTSIDE a SPARK proof
 * boundary — they are the environment the verified CPU logic runs against.
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
 * the machine (gate array, CRTC, interrupt generation) in lockstep. */
typedef struct z80core_bus {
  void* ctx;
  uint8_t (*read)(void* ctx, uint16_t addr);            /* memory read  */
  void (*write)(void* ctx, uint16_t addr, uint8_t val); /* memory write */
  uint8_t (*in_)(void* ctx, uint16_t port);             /* I/O read     */
  void (*out)(void* ctx, uint16_t port, uint8_t val);   /* I/O write    */
  void (*tick)(void* ctx, int tstates);                 /* advance machine N T-states */
} z80core_bus;

/* Canonical, language-neutral snapshot of ALL observable + internal CPU state.
 * The differential oracle harness and the IPC `regs` command use this to compare
 * any two implementations bit-for-bit. Deliberately a flat POD so an Ada record
 * can map it 1:1. The "internal" fields (wz, q) are what separate a merely
 * plausible Z80 from a correct one. */
typedef struct z80core_regs {
  uint16_t af, bc, de, hl;     /* main register file                         */
  uint16_t af_, bc_, de_, hl_; /* alternate set (AF', BC', DE', HL')          */
  uint16_t ix, iy, sp, pc;
  uint16_t wz;                 /* MEMPTR — sets XF/YF after BIT n,(HL)        */
  uint8_t i, r;                /* interrupt vector / memory-refresh registers */
  uint8_t im;                  /* interrupt mode 0/1/2                        */
  uint8_t iff1, iff2;          /* interrupt-enable flip-flops                 */
  uint8_t q;                   /* last-flags-touched latch (SCF/CCF quirk)    */
  uint8_t halted;              /* 1 while in HALT                             */
  uint64_t tstates;            /* cumulative T-state counter                  */
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

/* `bus` must outlive the returned state. */
z80core_state* z80core_create(const z80core_bus* bus);
void z80core_destroy(z80core_state* cpu);

void z80core_reset(z80core_state* cpu);

/* Execute exactly ONE instruction (including any prefix bytes and the
 * post-instruction interrupt acceptance check). Returns T-states consumed,
 * all of which have already been reported to the machine via bus->tick. */
int z80core_step(z80core_state* cpu);

/* Assert the maskable interrupt line. `bus_value` is the byte the device would
 * place on the data bus (the IM2 vector low byte / IM0 opcode). */
void z80core_irq(z80core_state* cpu, uint8_t bus_value);
void z80core_nmi(z80core_state* cpu);

/* Canonical-state I/O: the basis of differential testing and snapshots. */
void z80core_snapshot(const z80core_state* cpu, z80core_regs* out);
void z80core_restore(z80core_state* cpu, const z80core_regs* in);

uint16_t z80core_get_reg(const z80core_state* cpu, z80core_reg_id id);
void z80core_set_reg(z80core_state* cpu, z80core_reg_id id, uint16_t value);

/* Identifies the linked implementation, e.g. "cpp-cleanroom", "spark-ada",
 * "legacy-caprice32". Surfaced in logs and the IPC `version` reply. */
const char* z80core_impl_name(void);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_Z80CORE_H */
