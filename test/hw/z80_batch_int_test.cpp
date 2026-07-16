/* z80_batch_int_test.cpp — dual-mode equivalence of the interrupt-acceptance
 * paths (beads-95kn, plan §5). The FUSE corpus never asserts INT/NMI, so the
 * batch engine's acceptance sequence (z80_batch_step's boundary logic +
 * MC::IOACK arithmetic) needs its own oracle: run each scenario on the real
 * per-cycle GA board (ticked quantiser, an interrupt-line device) AND through
 * z80_batch_step on the same program, then require identical T-totals, full
 * architectural state, and memory (the stack pushes).
 *
 * Covered: IM0/IM1/IM2 acceptance, NMI (IFF2 preserved), the EI one-
 * instruction shadow, HALT wake-on-interrupt, and the halted R-refresh
 * cadence (one bump per 4 halted T-states) that z80_batch_halt reproduces in
 * closed form.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hw/board.h"
#include "hw/gate_array.h"
#include "hw/z80.h"

namespace {

struct Ram {
  uint8_t cells[0x10000];
};
void iram_tick(void* self, const Bus* in, Bus* out) {
  Ram* ram = static_cast<Ram*>(self);
  if (in->cpu.mreq && in->cpu.wr)
    ram->cells[in->cpu.addr] = in->cpu.data;
  else if (in->cpu.mreq && in->cpu.rd)
    out->cpu.data = ram->cells[in->cpu.addr];
}
void ino_reset(void*) {}
size_t iram_size(const void*) { return sizeof(Ram); }
void iram_save(const void* s, void* b) { std::memcpy(b, s, sizeof(Ram)); }
void iram_load(void* s, const void* b) { std::memcpy(s, b, sizeof(Ram)); }
Device iram_device(Ram* s) {
  return Device{s,         "ram",     iram_tick, ino_reset,
                iram_size, iram_save, iram_load};
}

// Interrupt-line driver: asserts INT and/or NMI as levels from tick 0. The
// data bus is left floating during the acknowledge, so the per-cycle engine
// latches the rest-bus 0xFF — the same vector the batch calls pass explicitly
// (the CPC's floating bus).
struct IntLine {
  bool irq = false;
  bool nmi = false;
};
void int_tick(void* self, const Bus*, Bus* out) {
  IntLine* line = static_cast<IntLine*>(self);
  if (line->irq) out->cpu.irq = true;
  if (line->nmi) out->cpu.nmi = true;
}
size_t int_size(const void*) { return sizeof(IntLine); }
void int_save(const void* s, void* b) { std::memcpy(b, s, sizeof(IntLine)); }
void int_load(void* s, const void* b) { std::memcpy(s, b, sizeof(IntLine)); }
Device int_device(IntLine* line) {
  return Device{line,     "int",    int_tick, ino_reset,
                int_size, int_save, int_load};
}

constexpr uint16_t kProg = 0x0100;
constexpr uint16_t kIsland = 0x0200;  // HALT for IM2 vectoring
constexpr uint16_t kStack = 0xFF00;

struct IntCase {
  std::string name;
  std::vector<uint8_t> prog;  // at kProg
  Z80Regs init;
  bool irq = false;  // INT level from tick 0 (per-cycle) / every step (batch)
  bool nmi = false;  // NMI edge at tick 0 (per-cycle) / pre-latched (batch)
};

Z80Regs base_init() {
  Z80Regs s{};
  s.sp = kStack;
  s.pc = kProg;
  return s;
}

void seed_ram(Ram* ram, const IntCase& c) {
  std::memset(ram->cells, 0, sizeof(Ram::cells));
  ram->cells[0x0038] = 0x76;  // IM0/IM1 target
  ram->cells[0x0066] = 0x76;  // NMI target
  ram->cells[kIsland] = 0x76;
  // IM2 vector table for I=0x80, floating-bus vector 0xFF → (0x80FF/0x8100).
  ram->cells[0x80FF] = static_cast<uint8_t>(kIsland & 0xFF);
  ram->cells[0x8100] = static_cast<uint8_t>(kIsland >> 8);
  for (size_t i = 0; i < c.prog.size(); ++i) ram->cells[kProg + i] = c.prog[i];
}

// Per-cycle run on the GA board until halted; -1 on hang.
int64_t run_percycle(const IntCase& c, Z80Regs* out_regs, Ram* ram) {
  seed_ram(ram, c);
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  IntLine line{c.irq, c.nmi};

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, iram_device(ram));
  board_add(&board, int_device(&line));
  board_add(&board, zdev);
  board_reset(&board);
  z80_poke(&zdev, &c.init);

  Z80Regs r{};
  for (int tick = 0; tick < 200000; ++tick) {
    board_tick(&board);
    z80_peek(&zdev, &r);
    // Stop only at a PARKED halt — one no asserted interrupt can wake
    // (acceptance clears IFF1, so the scenario-terminating HALT qualifies).
    // A protected mid-scenario HALT (EI shadow) is observable as halted for
    // a tick before the wake; breaking there would skip the wake path under
    // test.
    if (r.halted && !(c.irq && r.iff1 != 0)) {
      *out_regs = r;
      return static_cast<int64_t>(r.tstates);
    }
  }
  return -1;
}

uint8_t bmem_read(void* ctx, uint16_t addr, uint64_t) {
  return static_cast<Ram*>(ctx)->cells[addr];
}
void bmem_write(void* ctx, uint16_t addr, uint8_t val, uint64_t) {
  static_cast<Ram*>(ctx)->cells[addr] = val;
}
uint8_t bio_read(void*, uint16_t, uint64_t) { return 0xFF; }
void bio_write(void*, uint16_t, uint8_t, uint64_t) {}

// Batch run (grid on) until halted; -1 on hang. INT stays asserted across
// steps, mirroring the per-cycle level; the terminating HALT is left alone
// (the per-cycle loop stops at its first halted observation too).
int64_t run_batch(const IntCase& c, Z80Regs* out_regs, Ram* ram) {
  seed_ram(ram, c);
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  z80_poke(&zdev, &c.init);
  if (c.nmi) z80_batch_nmi(&zdev);

  const Z80BatchIO bio{ram, bmem_read, bmem_write, bio_read, bio_write};
  Z80Regs r{};
  for (int steps = 0; steps < 50000; ++steps) {
    z80_batch_step(&zdev, &bio, c.irq ? 1 : 0, /*vector=*/0xFF, /*grid=*/1);
    z80_peek(&zdev, &r);
    // Same parked-halt condition as the per-cycle loop: a protected HALT
    // (IFF1 still set, INT asserted) is stepped THROUGH — the next
    // z80_batch_step wakes and accepts, which is the path under test.
    if (r.halted && !(c.irq && r.iff1 != 0)) {
      *out_regs = r;
      return static_cast<int64_t>(r.tstates);
    }
  }
  return -1;
}

std::string diff_regs(const Z80Regs& a, const Z80Regs& b) {
  std::string d;
  auto cmp = [&](const char* n, uint64_t g, uint64_t e) {
    if (g != e) {
      char buf[64];
      std::snprintf(buf, sizeof buf, " %s=%llX!=%llX", n,
                    (unsigned long long)g, (unsigned long long)e);
      d += buf;
    }
  };
  cmp("AF", a.af, b.af);
  cmp("BC", a.bc, b.bc);
  cmp("DE", a.de, b.de);
  cmp("HL", a.hl, b.hl);
  cmp("SP", a.sp, b.sp);
  cmp("PC", a.pc, b.pc);
  cmp("WZ", a.wz, b.wz);
  cmp("I", a.i, b.i);
  cmp("R", a.r, b.r);
  cmp("IM", a.im, b.im);
  cmp("IFF1", a.iff1, b.iff1);
  cmp("IFF2", a.iff2, b.iff2);
  cmp("HALT", a.halted, b.halted);
  cmp("T", a.tstates, b.tstates);
  cmp("IC", a.instr_count, b.instr_count);
  return d;
}

void run_dual(const IntCase& c) {
  auto ram_pc = std::make_unique<Ram>();
  auto ram_b = std::make_unique<Ram>();
  Z80Regs regs_pc{}, regs_b{};
  const int64_t t_pc = run_percycle(c, &regs_pc, ram_pc.get());
  ASSERT_GE(t_pc, 0) << c.name << ": per-cycle never halted";
  const int64_t t_b = run_batch(c, &regs_b, ram_b.get());
  ASSERT_GE(t_b, 0) << c.name << ": batch never halted";
  EXPECT_EQ(t_b, t_pc) << c.name << ": T-total diverges";
  EXPECT_EQ(diff_regs(regs_b, regs_pc), "") << c.name << ": state diverges";
  EXPECT_EQ(std::memcmp(ram_b->cells, ram_pc->cells, sizeof(Ram::cells)), 0)
      << c.name << ": memory (stack pushes) diverges";
}

}  // namespace

// IM1: interrupts enabled at poke, INT asserted from tick 0 — accepted at the
// very first instruction boundary, before anything at kProg runs. 13T ack +
// grid alignment, RST-style vector to 0x38.
TEST(Z80BatchInt, IM1ImmediateAccept) {
  IntCase c;
  c.name = "IM1 immediate";
  c.prog = {0x00, 0x76};  // never reached
  c.init = base_init();
  c.init.im = 1;
  c.init.iff1 = c.init.iff2 = 1;
  c.irq = true;
  run_dual(c);
}

// IM0: the floating-bus 0xFF is executed as RST 38 — same target, distinct
// acceptance arm (int_vec & 0x38).
TEST(Z80BatchInt, IM0FloatingBusRst38) {
  IntCase c;
  c.name = "IM0 immediate";
  c.prog = {0x00, 0x76};
  c.init = base_init();
  c.init.im = 0;
  c.init.iff1 = c.init.iff2 = 1;
  c.irq = true;
  run_dual(c);
}

// IM2: vector through (I<<8 | 0xFF) → the kIsland HALT. Exercises the two
// extra table reads of the 19T sequence.
TEST(Z80BatchInt, IM2VectorTable) {
  IntCase c;
  c.name = "IM2 vector";
  c.prog = {0x00, 0x76};
  c.init = base_init();
  c.init.im = 2;
  c.init.i = 0x80;
  c.init.iff1 = c.init.iff2 = 1;
  c.irq = true;
  run_dual(c);
}

// NMI: edge latched before the first boundary; IFF1 cleared, IFF2 PRESERVED;
// 11T ack to 0x0066.
TEST(Z80BatchInt, NmiPreservesIff2) {
  IntCase c;
  c.name = "NMI";
  c.prog = {0x00, 0x76};
  c.init = base_init();
  c.init.im = 1;
  c.init.iff1 = c.init.iff2 = 1;  // IFF2 must survive as 1
  c.nmi = true;
  run_dual(c);
}

// EI shadow: interrupts start disabled; EI enables them but shields exactly
// one following instruction (INC A) before the acceptance.
TEST(Z80BatchInt, EiDelayShadowsOneInstruction) {
  IntCase c;
  c.name = "EI shadow";
  c.prog = {0xFB, 0x3C, 0x76};  // EI; INC A; HALT (HALT never fetched)
  c.init = base_init();
  c.init.im = 1;
  c.irq = true;  // pending the whole time; accepted only after INC A
  run_dual(c);
}

// HALT wake: EI's shadow lets the HALT execute despite the asserted INT; the
// halted CPU then wakes at the next boundary and vectors out — the wake tick
// IS the acceptance M1 T1 (no extra T-state), in both engines. The scenario
// ends halted at 0x38's HALT.
TEST(Z80BatchInt, HaltWakeOnInterrupt) {
  IntCase c;
  c.name = "HALT wake";
  c.prog = {0xFB, 0x76};  // EI; HALT
  c.init = base_init();
  c.init.im = 1;
  c.irq = true;
  run_dual(c);
}

// Halted time: with no interrupt ever arriving, the per-cycle engine burns
// T-states in place, bumping R once per 4 (the halt_t cadence).
// z80_batch_halt must reproduce the same R and tstates in closed form,
// including a cadence remainder (target deliberately not a multiple of 4).
TEST(Z80BatchInt, HaltedRefreshCadence) {
  IntCase c;
  c.name = "halted cadence";
  c.prog = {0x76};
  c.init = base_init();

  // Per-cycle: run the GA board for a while, then read tstates as the target.
  auto ram_pc = std::make_unique<Ram>();
  seed_ram(ram_pc.get(), c);
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, iram_device(ram_pc.get()));
  board_add(&board, zdev);
  board_reset(&board);
  z80_poke(&zdev, &c.init);
  for (int tick = 0; tick < 4001; ++tick) board_tick(&board);
  Z80Regs regs_pc{};
  z80_peek(&zdev, &regs_pc);
  ASSERT_TRUE(regs_pc.halted);
  ASSERT_GT(regs_pc.tstates, 8u);  // sanity: well past the HALT fetch

  // Batch: execute the HALT, then burn exactly to the same target.
  auto ram_b = std::make_unique<Ram>();
  seed_ram(ram_b.get(), c);
  std::vector<uint8_t> zmem_b(z80_state_size());
  Device zdev_b = z80_init(zmem_b.data());
  z80_poke(&zdev_b, &c.init);
  const Z80BatchIO bio{ram_b.get(), bmem_read, bmem_write, bio_read,
                       bio_write};
  z80_batch_step(&zdev_b, &bio, 0, 0xFF, 1);  // the HALT instruction
  Z80Regs regs_b{};
  z80_peek(&zdev_b, &regs_b);
  ASSERT_TRUE(regs_b.halted);
  ASSERT_LT(regs_b.tstates, regs_pc.tstates);
  EXPECT_EQ(z80_batch_step(&zdev_b, &bio, 0, 0xFF, 1), 0u)
      << "halted with no interrupt must consume nothing";
  z80_batch_halt(&zdev_b,
                 static_cast<uint32_t>(regs_pc.tstates - regs_b.tstates));
  z80_peek(&zdev_b, &regs_b);
  EXPECT_EQ(diff_regs(regs_b, regs_pc), "") << c.name << ": state diverges";
}
