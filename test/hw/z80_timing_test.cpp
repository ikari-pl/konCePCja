/* z80_timing_test.cpp — the GA WAIT µs-quantiser, validated against the legacy
 * cc_op[] golden master. Runs each opcode two ways:
 *   - raw board  (always-on clock, phase always 0): datasheet T-states (FUSE-proven).
 *   - GA board   (Gate Array drives the ÷4 clock + µs phase): CPC-adjusted timing.
 * The GA quantiser must produce cpc = roundup(raw, 4), and that must equal cc_op.
 * See docs/hardware/gate-array-device.md §3, §6. */

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "hw/board.h"
#include "hw/gate_array.h"
#include "hw/z80.h"

namespace {

struct Ram {
  uint8_t cells[0x10000];
};
void tram_tick(void* self, const Bus* in, Bus* out) {
  Ram* ram = static_cast<Ram*>(self);
  if (in->cpu.mreq && in->cpu.wr) ram->cells[in->cpu.addr] = in->cpu.data;
  else if (in->cpu.mreq && in->cpu.rd) out->cpu.data = ram->cells[in->cpu.addr];
}
void tno_reset(void*) {}
size_t tram_size(const void*) { return sizeof(Ram); }
void tram_save(const void* s, void* b) { std::memcpy(b, s, sizeof(Ram)); }
void tram_load(void* s, const void* b) { std::memcpy(s, b, sizeof(Ram)); }
Device tram_device(Ram* s) {
  return Device{s, "ram", tram_tick, tno_reset, tram_size, tram_save, tram_load};
}

// Always-on clock: phase stays 0 → the quantiser is a no-op → raw datasheet timing.
void tclk_tick(void*, const Bus*, Bus* out) { out->clk.cpu = true; }
size_t tone_size(const void*) { return 1; }
void tone_save(const void*, void* b) { *static_cast<uint8_t*>(b) = 0; }
void tone_load(void*, const void*) {}
Device tclk_device() {
  static uint8_t d = 0;
  return Device{&d, "clk", tclk_tick, tno_reset, tone_size, tone_save, tone_load};
}

// Run `program` from PC=0 with a poked initial state until HALT; return T-states at
// the halt. `use_ga` selects the real Gate Array clock (quantised) vs the raw clock.
uint64_t run_timed(const std::vector<uint8_t>& program, const Z80Regs& init, bool use_ga) {
  auto ram = std::make_unique<Ram>();
  std::memset(ram->cells, 0, sizeof(Ram::cells));
  for (size_t i = 0; i < program.size(); ++i) ram->cells[i] = program[i];

  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());

  Board board;
  board_init(&board);
  if (use_ga) board_add(&board, gdev);
  else board_add(&board, tclk_device());
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

uint64_t roundup4(uint64_t v) { return (v + 3) & ~static_cast<uint64_t>(3); }

// Each case: a program (opcode + operands) followed by HALT. HALT itself is 4 T
// on both boards (its fetch starts already µs-aligned), so cpc(opcode) = ga - 4.
struct TimingCase {
  const char* name;
  std::vector<uint8_t> program;  // ends with 0x76 HALT
  uint16_t hl, sp, ix;
  int cc_op;  // legacy CPC total for the opcode (0 = golden anchor not asserted)
};

Z80Regs init_state(const TimingCase& c) {
  Z80Regs s{};
  s.af = 0xFFFF; s.sp = c.sp; s.hl = c.hl; s.ix = c.ix; s.bc = 0x0050; s.de = 0x0050;
  return s;
}

}  // namespace

TEST(Z80Timing, WaitQuantiserMatchesCcOp) {
  const std::vector<TimingCase> cases = {
      // name              program                     hl      sp      ix      cc_op
      {"NOP",              {0x00, 0x76},               0,      0xFFF0, 0,      4},
      {"LD BC,nn",         {0x01, 0x34, 0x12, 0x76},   0,      0xFFF0, 0,      12},
      {"LD (BC),A",        {0x02, 0x76},               0,      0xFFF0, 0,      8},
      {"INC BC",           {0x03, 0x76},               0,      0xFFF0, 0,      8},
      {"INC B",            {0x04, 0x76},               0,      0xFFF0, 0,      4},
      {"LD B,n",           {0x06, 0xAB, 0x76},         0,      0xFFF0, 0,      8},
      {"ADD HL,BC",        {0x09, 0x76},               0x1000, 0xFFF0, 0,      12},
      {"LD A,n",           {0x3E, 0x99, 0x76},         0,      0xFFF0, 0,      8},
      {"INC (HL)",         {0x34, 0x76},               0x0040, 0xFFF0, 0,      12},
      {"LD (HL),n",        {0x36, 0x55, 0x76},         0x0040, 0xFFF0, 0,      12},
      {"LD A,(HL)",        {0x7E, 0x76},               0x0040, 0xFFF0, 0,      8},
      {"ADD A,B",          {0x80, 0x76},               0,      0xFFF0, 0,      4},
      {"ADD A,(HL)",       {0x86, 0x76},               0x0040, 0xFFF0, 0,      8},
      {"POP BC",           {0xC1, 0x76},               0,      0xFFF0, 0,      12},
      {"PUSH BC",          {0xC5, 0x76},               0,      0xFFF0, 0,      12},
      {"ADD A,n",          {0xC6, 0x01, 0x76},         0,      0xFFF0, 0,      8},
      {"EXX",              {0xD9, 0x76},               0,      0xFFF0, 0,      4},
      {"EX (SP),HL",       {0xE3, 0x76},               0x0040, 0xFFF0, 0,      20},
      {"LD SP,HL",         {0xF9, 0x76},               0x0040, 0xFFF0, 0,      8},
      // Prefixed: golden anchor omitted (cc_op split across tables); quantiser
      // property (ga == roundup(raw,4)) still checked.
      {"RLC B (CB)",       {0xCB, 0x00, 0x76},         0,      0xFFF0, 0,      0},
      {"RLC (HL) (CB)",    {0xCB, 0x06, 0x76},         0x0040, 0xFFF0, 0,      0},
      {"SBC HL,BC (ED)",   {0xED, 0x42, 0x76},         0x1000, 0xFFF0, 0,      0},
      {"ADD IX,BC (DD)",   {0xDD, 0x09, 0x76},         0,      0xFFF0, 0x1000, 0},
      {"LD A,(IX+d) (DD)", {0xDD, 0x7E, 0x00, 0x76},   0,      0xFFF0, 0x0040, 0},
  };

  for (const auto& c : cases) {
    const Z80Regs init = init_state(c);
    const uint64_t raw = run_timed(c.program, init, /*use_ga=*/false);
    const uint64_t ga = run_timed(c.program, init, /*use_ga=*/true);
    // The quantiser rounds each instruction up to a µs; HALT is 4 T on both.
    EXPECT_EQ(ga, roundup4(raw - 4) + 4) << c.name << ": GA time = roundup(raw,4)";
    if (c.cc_op != 0) {
      EXPECT_EQ(ga - 4, static_cast<uint64_t>(c.cc_op))
          << c.name << ": CPC total matches the legacy cc_op golden master";
    }
  }
}
