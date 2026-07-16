/* z80_timing_test.cpp — the GA WAIT µs-quantiser, validated against the legacy
 * cc_op[] golden master. Runs each opcode two ways:
 *   - raw board  (always-on clock, phase always 0): datasheet T-states
 * (FUSE-proven).
 *   - GA board   (Gate Array drives the ÷4 clock + µs phase): CPC-adjusted
 * timing. The CPC rule is PER-M-CYCLE alignment: every bus machine cycle
 * (fetch, memory read/write, I/O) starts on a µs boundary; internal cycles run
 * freely. A plain per-instruction roundup(raw,4) agrees for single-access
 * opcodes but undercounts multi-access ones — the distinguishing anchors here
 * (PUSH=16, RST=16, LD HL,(nn)=20, EX (SP),HL=24) come straight from the legacy
 * cc_op table and separate the two models. See
 * docs/hardware/gate-array-device.md §3, §6. */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "hw/board.h"
#include "hw/gate_array.h"
#include "hw/z80.h"

namespace {

struct Ram {
  uint8_t cells[0x10000];
};
void tram_tick(void* self, const Bus* in, Bus* out) {
  Ram* ram = static_cast<Ram*>(self);
  if (in->cpu.mreq && in->cpu.wr)
    ram->cells[in->cpu.addr] = in->cpu.data;
  else if (in->cpu.mreq && in->cpu.rd)
    out->cpu.data = ram->cells[in->cpu.addr];
}
void tno_reset(void*) {}
size_t tram_size(const void*) { return sizeof(Ram); }
void tram_save(const void* s, void* b) { std::memcpy(b, s, sizeof(Ram)); }
void tram_load(void* s, const void* b) { std::memcpy(s, b, sizeof(Ram)); }
Device tram_device(Ram* s) {
  return Device{s,         "ram",     tram_tick, tno_reset,
                tram_size, tram_save, tram_load};
}

// Always-on clock: phase stays 0 → the quantiser is a no-op → raw datasheet
// timing.
void tclk_tick(void*, const Bus*, Bus* out) { out->clk.cpu = true; }
size_t tone_size(const void*) { return 1; }
void tone_save(const void*, void* b) { *static_cast<uint8_t*>(b) = 0; }
void tone_load(void*, const void*) {}
Device tclk_device() {
  static uint8_t d = 0;
  return Device{&d,        "clk",     tclk_tick, tno_reset,
                tone_size, tone_save, tone_load};
}

// Run `program` from PC=0 with a poked initial state until HALT; return
// T-states at the halt. `use_ga` selects the real Gate Array clock (quantised)
// vs the raw clock.
uint64_t run_timed(const std::vector<uint8_t>& program, const Z80Regs& init,
                   bool use_ga) {
  auto ram = std::make_unique<Ram>();
  std::memset(ram->cells, 0, sizeof(Ram::cells));
  for (size_t i = 0; i < program.size(); ++i) ram->cells[i] = program[i];

  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());

  Board board;
  board_init(&board);
  if (use_ga)
    board_add(&board, gdev);
  else
    board_add(&board, tclk_device());
  board_add(&board, tram_device(ram.get()));
  board_add(&board, zdev);
  board_reset(&board);
  z80_poke(&zdev, &init);

  Z80Regs r{};
  for (int tick = 0; tick < 200000; ++tick) {
    board_tick(&board);
    z80_peek(&zdev, &r);
    if (r.halted) break;
  }
  return r.tstates;
}

// Each case: a program (opcode + operands) followed by HALT. HALT itself is 4 T
// on both boards (its fetch starts already µs-aligned), so cpc(opcode) = ga
// - 4.
struct TimingCase {
  const char* name;
  std::vector<uint8_t> program;  // ends with 0x76 HALT
  uint16_t hl, sp, ix;
  int cc_op;  // legacy CPC total for the opcode (0 = golden anchor not
              // asserted)
};

Z80Regs init_state(const TimingCase& c) {
  Z80Regs s{};
  s.af = 0xFFFF;
  s.sp = c.sp;
  s.hl = c.hl;
  s.ix = c.ix;
  s.bc = 0x0050;
  s.de = 0x0050;
  return s;
}

// A DMA master that asserts BUSRQ while `on` is true.
struct BusrqInjector {
  bool on;
};
void busrq_tick(void* self, const Bus*, Bus* out) {
  if (static_cast<BusrqInjector*>(self)->on) out->cpu.busrq = true;
}
Device busrq_device(BusrqInjector* s) {
  return Device{s,         "busrq",   busrq_tick, tno_reset,
                tone_size, tone_save, tone_load};
}

}  // namespace

TEST(Z80Timing, WaitQuantiserMatchesCcOp) {
  const std::vector<TimingCase> cases = {
      // name              program                     hl      sp      ix cc_op
      {"NOP", {0x00, 0x76}, 0, 0xFFF0, 0, 4},
      {"LD BC,nn", {0x01, 0x34, 0x12, 0x76}, 0, 0xFFF0, 0, 12},
      {"LD (BC),A", {0x02, 0x76}, 0, 0xFFF0, 0, 8},
      {"INC BC", {0x03, 0x76}, 0, 0xFFF0, 0, 8},
      {"INC B", {0x04, 0x76}, 0, 0xFFF0, 0, 4},
      {"LD B,n", {0x06, 0xAB, 0x76}, 0, 0xFFF0, 0, 8},
      {"ADD HL,BC", {0x09, 0x76}, 0x1000, 0xFFF0, 0, 12},
      {"LD A,n", {0x3E, 0x99, 0x76}, 0, 0xFFF0, 0, 8},
      {"INC (HL)", {0x34, 0x76}, 0x0040, 0xFFF0, 0, 12},
      {"LD (HL),n", {0x36, 0x55, 0x76}, 0x0040, 0xFFF0, 0, 12},
      {"LD A,(HL)", {0x7E, 0x76}, 0x0040, 0xFFF0, 0, 8},
      {"ADD A,B", {0x80, 0x76}, 0, 0xFFF0, 0, 4},
      {"ADD A,(HL)", {0x86, 0x76}, 0x0040, 0xFFF0, 0, 8},
      {"POP BC", {0xC1, 0x76}, 0, 0xFFF0, 0, 12},
      {"PUSH BC", {0xC5, 0x76}, 0, 0xFFF0, 0, 16},
      {"ADD A,n", {0xC6, 0x01, 0x76}, 0, 0xFFF0, 0, 8},
      {"EXX", {0xD9, 0x76}, 0, 0xFFF0, 0, 4},
      {"EX (SP),HL", {0xE3, 0x76}, 0x0040, 0xFFF0, 0, 24},
      {"LD SP,HL", {0xF9, 0x76}, 0x0040, 0xFFF0, 0, 8},
      // Multi-M-cycle distinguishers (the cases where per-M-cycle alignment and
      // a
      // per-instruction roundup(raw,4) disagree — goldens from the legacy
      // cc_op):
      {"LD (nn),HL", {0x22, 0x40, 0x00, 0x76}, 0x1234, 0xFFF0, 0, 20},
      {"LD HL,(nn)", {0x2A, 0x40, 0x00, 0x76}, 0, 0xFFF0, 0, 20},
      {"LD (nn),A", {0x32, 0x40, 0x00, 0x76}, 0, 0xFFF0, 0, 16},
      {"JP nn", {0xC3, 0x04, 0x00, 0x00, 0x76}, 0, 0xFFF0, 0, 12},
      {"CALL nn", {0xCD, 0x04, 0x00, 0x00, 0x76}, 0, 0xFFF0, 0, 20},
      // RET: the return address (0x0001 = the HALT) lives in the program bytes
      // at
      // 0x0004..5, and SP points there.
      {"RET", {0xC9, 0x76, 0x00, 0x00, 0x01, 0x00}, 0, 0x0004, 0, 12},
      // RST 08 jumps to 0x0008, where the program plants a HALT.
      {"RST 08", {0xCF, 0x76, 0, 0, 0, 0, 0, 0, 0x76}, 0, 0xFFF0, 0, 16},
      {"DJNZ +0 (taken)", {0x10, 0x00, 0x76}, 0, 0xFFF0, 0, 16},
      {"OUT (n),A", {0xD3, 0x7F, 0x76}, 0, 0xFFF0, 0, 12},
      {"IN A,(n)", {0xDB, 0x7F, 0x76}, 0, 0xFFF0, 0, 12},
      // Prefixed (goldens from the legacy prefixed cc tables / WinAPE):
      {"RLC B (CB)", {0xCB, 0x00, 0x76}, 0, 0xFFF0, 0, 8},
      {"RLC (HL) (CB)", {0xCB, 0x06, 0x76}, 0x0040, 0xFFF0, 0, 16},
      {"SBC HL,BC (ED)", {0xED, 0x42, 0x76}, 0x1000, 0xFFF0, 0, 16},
      {"ADD IX,BC (DD)", {0xDD, 0x09, 0x76}, 0, 0xFFF0, 0x1000, 16},
      {"LD A,(IX+d) (DD)", {0xDD, 0x7E, 0x00, 0x76}, 0, 0xFFF0, 0x0040, 20},
  };

  for (const auto& c : cases) {
    const Z80Regs init = init_state(c);
    const uint64_t raw = run_timed(c.program, init, /*use_ga=*/false);
    const uint64_t ga = run_timed(c.program, init, /*use_ga=*/true);
    ASSERT_GT(raw, 0u) << c.name << ": raw run reached HALT";
    if (c.cc_op != 0) {
      EXPECT_EQ(ga - 4, static_cast<uint64_t>(c.cc_op))
          << c.name << ": CPC total matches the legacy cc_op golden master"
          << " (raw datasheet was " << raw - 4 << "T)";
    }
  }
}

// BUSRQ / BUSAK arbitration: a DMA master takes the bus, the CPU grants BUSAK
// and freezes, then resumes when the bus is returned. (Increment C-0 — the
// prerequisite for the ASIC DMA sound sequencer.)
TEST(Z80Busrq, GrantsBusakFreezesThenResumes) {
  auto ram = std::make_unique<Ram>();
  std::memset(ram->cells, 0, sizeof(Ram::cells));
  ram->cells[0] = 0x3C;  // INC A
  ram->cells[1] = 0x18;  // JR -3  (tight forever loop)
  ram->cells[2] = 0xFD;

  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  BusrqInjector inj{false};

  Board board;
  board_init(&board);
  board_add(&board, tclk_device());  // always-on clock: raw timing
  board_add(&board, tram_device(ram.get()));
  board_add(&board, busrq_device(&inj));
  board_add(&board, zdev);
  board_reset(&board);
  Z80Regs init{};
  init.af = 0xFFFF;
  z80_poke(&zdev, &init);

  for (int i = 0; i < 200; ++i) board_tick(&board);  // let it run
  Z80Regs before{};
  z80_peek(&zdev, &before);
  ASSERT_GT(before.tstates, 0u) << "the CPU executed before the DMA request";

  inj.on = true;  // request the bus
  for (int i = 0; i < 100; ++i) board_tick(&board);
  Z80Regs held{};
  z80_peek(&zdev, &held);
  EXPECT_TRUE(board.bus.cpu.busak) << "BUSAK granted while BUSRQ is held";
  const uint64_t frozen_t = held.tstates;

  for (int i = 0; i < 200; ++i) board_tick(&board);  // held: no progress
  Z80Regs still{};
  z80_peek(&zdev, &still);
  EXPECT_EQ(still.tstates, frozen_t) << "no T-state advances while BUSAK held";
  EXPECT_EQ(still.pc, held.pc) << "PC frozen during the DMA hold";

  inj.on = false;  // return the bus
  for (int i = 0; i < 200; ++i) board_tick(&board);
  Z80Regs after{};
  z80_peek(&zdev, &after);
  EXPECT_GT(after.tstates, frozen_t) << "the CPU resumes after BUSRQ deasserts";
  EXPECT_FALSE(board.bus.cpu.busak) << "BUSAK released with BUSRQ";
}
