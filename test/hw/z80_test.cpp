/* z80_test.cpp — Round Z80-a: the cycle-stepped Z80 device on a board, validating
 * final register/flag state and exact T-state totals for the 8-bit register/
 * immediate group. (Per-T bus-trace vs FUSE is offset by the bus model's one-hop
 * latency — see hw-spec §2 — so we check totals + final state here.)
 */

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "hw/board.h"
#include "hw/z80.h"

namespace {

constexpr uint8_t SF = 0x80, ZF = 0x40, YF = 0x20, HF = 0x10, XF = 0x08, PF = 0x04,
                  NF = 0x02, CF = 0x01;

/* RAM device (two-phase). */
struct Ram {
  uint8_t cells[0x10000];
};
void ram_tick(void* self, const Bus* in, Bus* out) {
  Ram* ram = static_cast<Ram*>(self);
  if (in->cpu.mreq && in->cpu.wr) {
    ram->cells[in->cpu.addr] = in->cpu.data;
  } else if (in->cpu.mreq && in->cpu.rd) {
    out->cpu.data = ram->cells[in->cpu.addr];
  }
}
void ram_reset(void*) {}
size_t ram_size(const void*) { return sizeof(Ram); }
void ram_save(const void* s, void* b) { std::memcpy(b, s, sizeof(Ram)); }
void ram_load(void* s, const void* b) { std::memcpy(s, b, sizeof(Ram)); }
Device ram_device(Ram* s) {
  return Device{s, "ram", ram_tick, ram_reset, ram_size, ram_save, ram_load};
}

/* Clock that always enables the CPU: one Z80 T-state per board tick. */
void clk_tick(void*, const Bus*, Bus* out) { out->clk.cpu = true; }
void clk_reset(void*) {}
size_t clk_size(const void*) { return 1; }
void clk_save(const void*, void* b) { *static_cast<uint8_t*>(b) = 0; }
void clk_load(void*, const void*) {}
Device clock_device() {
  static uint8_t dummy = 0;
  return Device{&dummy, "clk", clk_tick, clk_reset, clk_size, clk_save, clk_load};
}

/* Free-running WAIT generator: clk.cpu always on, cpu.wait asserted on alternate
 * master cycles (NOT reactive to mreq — WAIT on the real GA is free-running). */
void waitclk_tick(void* self, const Bus*, Bus* out) {
  uint8_t* ctr = static_cast<uint8_t*>(self);
  *ctr = static_cast<uint8_t>(*ctr + 1);
  out->clk.cpu = true;
  out->cpu.wait = (*ctr & 1) != 0;
}
Device waitclock_device(uint8_t* ctr) {
  return Device{ctr, "waitclk", waitclk_tick, clk_reset, clk_size, clk_save, clk_load};
}

/* Run a program from 0x0000 until the Z80 halts (programs end with HALT 0x76).
 * Returns the final architectural state, with tstates frozen at the HALT. */
struct Rig {
  std::unique_ptr<Ram> ram = std::make_unique<Ram>();
  std::vector<uint8_t> z80mem = std::vector<uint8_t>(z80_state_size());
  Board board;
  Device zdev;
};

Z80Regs run(std::initializer_list<uint8_t> program) {
  static Rig rig;  // reused; reset below
  rig.ram = std::make_unique<Ram>();
  std::memset(rig.ram->cells, 0, sizeof(Ram::cells));
  size_t i = 0;
  for (uint8_t b : program) rig.ram->cells[i++] = b;

  rig.z80mem.assign(z80_state_size(), 0);
  rig.zdev = z80_init(rig.z80mem.data());

  board_init(&rig.board);
  board_add(&rig.board, clock_device());
  board_add(&rig.board, ram_device(rig.ram.get()));
  board_add(&rig.board, rig.zdev);
  board_reset(&rig.board);

  Z80Regs r{};
  for (int tick = 0; tick < 4000; ++tick) {
    board_tick(&rig.board);
    z80_peek(&rig.zdev, &r);
    if (r.halted) break;
  }
  return r;
}

uint8_t lo(uint16_t v) { return static_cast<uint8_t>(v & 0xFF); }
uint8_t hi(uint16_t v) { return static_cast<uint8_t>(v >> 8); }

}  // namespace

TEST(Z80a, NopTimingAndHalt) {
  Z80Regs r = run({0x00, 0x76});  // NOP ; HALT
  EXPECT_EQ(r.halted, 1);
  EXPECT_EQ(r.pc, 0x0002);
  EXPECT_EQ(r.tstates, 8u) << "NOP (4) + HALT M1 (4)";
  EXPECT_FALSE(r.unimplemented);
}

TEST(Z80a, LoadImmediateTiming) {
  Z80Regs r = run({0x3E, 0x12, 0x76});  // LD A,0x12 ; HALT
  EXPECT_EQ(hi(r.af), 0x12) << "A loaded";
  EXPECT_EQ(r.tstates, 11u) << "LD A,n (7) + HALT (4)";
}

TEST(Z80a, RegisterToRegister) {
  Z80Regs r = run({0x3E, 0x55, 0x47, 0x76});  // LD A,0x55 ; LD B,A ; HALT
  EXPECT_EQ(hi(r.af), 0x55);
  EXPECT_EQ(hi(r.bc), 0x55) << "B = A";
  EXPECT_EQ(r.tstates, 15u) << "7 + 4 + 4";
}

TEST(Z80a, AddSetsCarryZeroHalf) {
  Z80Regs r = run({0x3E, 0xFF, 0xC6, 0x01, 0x76});  // LD A,0xFF ; ADD A,1 ; HALT
  EXPECT_EQ(hi(r.af), 0x00) << "0xFF + 1 = 0x00";
  EXPECT_EQ(lo(r.af), static_cast<uint8_t>(ZF | HF | CF)) << "Z,H,C set; S,N,P clear";
}

TEST(Z80a, SubOverflowAndSign) {
  Z80Regs r = run({0x3E, 0x80, 0xD6, 0x01, 0x76});  // LD A,0x80 ; SUB 1 ; HALT
  EXPECT_EQ(hi(r.af), 0x7F);
  // 0x80-1=0x7F: S=0, Z=0, Y=1(0x20), H=1, X=1(0x08), P/V=1(overflow), N=1, C=0
  EXPECT_EQ(lo(r.af), static_cast<uint8_t>(YF | HF | XF | PF | NF));
}

TEST(Z80a, CompareDoesNotChangeA) {
  Z80Regs r = run({0x3E, 0x42, 0xFE, 0x42, 0x76});  // LD A,0x42 ; CP 0x42 ; HALT
  EXPECT_EQ(hi(r.af), 0x42) << "CP leaves A unchanged";
  EXPECT_TRUE(lo(r.af) & ZF) << "equal → Z set";
  EXPECT_TRUE(lo(r.af) & NF) << "CP sets N";
}

TEST(Z80a, IncOverflowAndDecHalf) {
  // XOR A first to clear carry (reset F is 0xFF; INC/DEC preserve carry).
  Z80Regs ri = run({0xAF, 0x3E, 0x7F, 0x3C, 0x76});  // XOR A ; LD A,0x7F ; INC A ; HALT
  EXPECT_EQ(hi(ri.af), 0x80);
  EXPECT_EQ(lo(ri.af), static_cast<uint8_t>(SF | HF | PF)) << "INC 0x7F→0x80: S,H,P/V; N,C clear";

  Z80Regs rd = run({0xAF, 0x06, 0x00, 0x05, 0x76});  // XOR A ; LD B,0 ; DEC B ; HALT
  EXPECT_EQ(hi(rd.bc), 0xFF);
  EXPECT_EQ(lo(rd.af), static_cast<uint8_t>(SF | YF | HF | XF | NF)) << "DEC 0→0xFF flags";
}

TEST(Z80a, XorAClearsToKnownState) {
  Z80Regs r = run({0x3E, 0x5A, 0xAF, 0x76});  // LD A,0x5A ; XOR A ; HALT
  EXPECT_EQ(hi(r.af), 0x00) << "XOR A = 0";
  EXPECT_EQ(lo(r.af), static_cast<uint8_t>(ZF | PF)) << "Z + even parity; H,N,C clear";
}

TEST(Z80a, UnimplementedOpcodesAreFlagged) {
  // Opcodes not yet decoded (control flow / prefixes / 16-bit) must set
  // `unimplemented`, never silently compute garbage. Single-byte so [op, HALT]
  // halts cleanly (the guard doesn't consume operand bytes).
  EXPECT_TRUE(run({0xC9, 0x76}).unimplemented) << "RET";
  EXPECT_TRUE(run({0xF3, 0x76}).unimplemented) << "DI";
  EXPECT_TRUE(run({0x08, 0x76}).unimplemented) << "EX AF,AF'";
  EXPECT_TRUE(run({0x07, 0x76}).unimplemented) << "RLCA";
  // ...while implemented ops stay clean:
  EXPECT_FALSE(run({0x78, 0x76}).unimplemented) << "LD A,B";
  EXPECT_FALSE(run({0x7E, 0x76}).unimplemented) << "LD A,(HL) (now implemented)";
}

TEST(Z80a, QClearedByNonFlagInstruction) {
  // Rak's Q must commit to 0 after an instruction that writes no flags (else the
  // NMOS SCF/CCF formula breaks in Z80-e). Reset F is 0xFF, so the old init bug
  // (qq = F) would leave Q = 0xFF. Run NOPs with no HALT so nothing re-commits Q.
  // (HALT itself is a non-flag op and would also force Q=0, masking the bug.)
  EXPECT_EQ(run({0x00}).q, 0u) << "Q=0 after NOP (init must clear, not copy F)";
  EXPECT_EQ(run({0x3E, 0x55}).q, 0u) << "Q=0 after LD A,n then NOPs";
}

TEST(Z80a, RefreshIncrements) {
  Z80Regs r = run({0x00, 0x00, 0x00, 0x76});  // 3×NOP ; HALT
  EXPECT_EQ(r.r & 0x7F, 4u) << "R bumped once per M1 (4 instructions incl. HALT)";
}

// ---- Round Z80-b: the (HL) memory-operand group (RAM scratch at 0x0040) ----
// HL is set via 8-bit immediates (16-bit loads arrive in Z80-b-2).

TEST(Z80b, StoreImmediateAndLoadFromHL) {
  // LD H,0 ; LD L,0x40 ; LD (HL),0xAB ; LD A,(HL) ; HALT
  Z80Regs r = run({0x26, 0x00, 0x2E, 0x40, 0x36, 0xAB, 0x7E, 0x76});
  EXPECT_EQ(hi(r.af), 0xAB) << "A = (HL) after storing 0xAB there";
  EXPECT_EQ(r.tstates, 7u + 7u + 10u + 7u + 4u) << "7+7 (LD r,n) + 10 (LD (HL),n) + 7 (LD A,(HL)) + 4";
}

TEST(Z80b, StoreRegisterToHL) {
  // LD H,0 ; LD L,0x40 ; LD A,0x99 ; LD (HL),A ; LD A,0 ; LD A,(HL) ; HALT
  Z80Regs r = run({0x26, 0x00, 0x2E, 0x40, 0x3E, 0x99, 0x77, 0x3E, 0x00, 0x7E, 0x76});
  EXPECT_EQ(hi(r.af), 0x99) << "stored A to (HL), cleared A, read it back";
}

TEST(Z80b, AluFromHL) {
  // LD H,0 ; LD L,0x40 ; LD (HL),0x05 ; LD A,0x03 ; ADD A,(HL) ; HALT
  Z80Regs r = run({0x26, 0x00, 0x2E, 0x40, 0x36, 0x05, 0x3E, 0x03, 0x86, 0x76});
  EXPECT_EQ(hi(r.af), 0x08) << "3 + (HL)=5 = 8";
  EXPECT_FALSE(r.unimplemented);
}

TEST(Z80b, IncHLReadModifyWrite) {
  // LD H,0 ; LD L,0x40 ; LD (HL),0x7F ; INC (HL) ; LD A,(HL) ; HALT
  Z80Regs r = run({0x26, 0x00, 0x2E, 0x40, 0x36, 0x7F, 0x34, 0x7E, 0x76});
  EXPECT_EQ(hi(r.af), 0x80) << "(HL) read-modify-written 0x7F→0x80";
  EXPECT_EQ(r.tstates, 7u + 7u + 10u + 11u + 7u + 4u) << "INC (HL) is 11T (4+3+1+3)";
}

TEST(Z80b, DecHL) {
  // LD H,0 ; LD L,0x40 ; LD (HL),0x00 ; DEC (HL) ; LD A,(HL) ; HALT
  Z80Regs r = run({0x26, 0x00, 0x2E, 0x40, 0x36, 0x00, 0x35, 0x7E, 0x76});
  EXPECT_EQ(hi(r.af), 0xFF) << "(HL): 0x00 → 0xFF";
}

TEST(Z80b, Load16BitImmediate) {
  // LD BC,0x1234 ; LD SP,0xFFF0 ; HALT
  Z80Regs r = run({0x01, 0x34, 0x12, 0x31, 0xF0, 0xFF, 0x76});
  EXPECT_EQ(r.bc, 0x1234u);
  EXPECT_EQ(r.sp, 0xFFF0u);
  EXPECT_EQ(r.tstates, 10u + 10u + 4u) << "LD rr,nn = 10T each";
}

TEST(Z80b, Inc16AndDec16NoFlags) {
  // LD BC,0x00FF ; INC BC ; HALT  → 0x0100, 6T for INC rr
  Z80Regs ri = run({0x01, 0xFF, 0x00, 0x03, 0x76});
  EXPECT_EQ(ri.bc, 0x0100u);
  EXPECT_EQ(ri.tstates, 10u + 6u + 4u) << "INC rr = 6T (4+2 internal)";

  // LD HL,0x0000 ; DEC HL ; HALT  → 0xFFFF
  Z80Regs rd = run({0x21, 0x00, 0x00, 0x2B, 0x76});
  EXPECT_EQ(rd.hl, 0xFFFFu);
}

TEST(Z80b, LoadAFromBCSetsMemptr) {
  // LD HL,0x0040 ; LD (HL),0x77 ; LD BC,0x0040 ; LD A,(BC) ; HALT
  Z80Regs r = run({0x21, 0x40, 0x00, 0x36, 0x77, 0x01, 0x40, 0x00, 0x0A, 0x76});
  EXPECT_EQ(hi(r.af), 0x77) << "A = (BC)";
  EXPECT_EQ(r.wz, 0x0041u) << "MEMPTR = BC + 1";
}

TEST(Z80b, WaitStallExtendsMemoryCyclesNotState) {
  // The CPC §3.1 WAIT contract: the GA stalls memory M-cycles. Final state must
  // be identical to the no-wait run; only the T-state count grows.
  auto ram = std::make_unique<Ram>();
  std::memset(ram->cells, 0, sizeof(Ram::cells));
  const uint8_t prog[] = {0x26, 0x00, 0x2E, 0x40, 0x36, 0xAB, 0x7E, 0x76};  // store + load
  std::memcpy(ram->cells, prog, sizeof(prog));
  std::vector<uint8_t> zmem(z80_state_size(), 0);
  Device zdev = z80_init(zmem.data());
  uint8_t wctr = 0;

  Board board;
  board_init(&board);
  board_add(&board, waitclock_device(&wctr));
  board_add(&board, ram_device(ram.get()));
  board_add(&board, zdev);
  board_reset(&board);

  Z80Regs r{};
  for (int i = 0; i < 8000; ++i) {
    board_tick(&board);
    z80_peek(&zdev, &r);
    if (r.halted) break;
  }
  EXPECT_EQ(hi(r.af), 0xAB) << "value correct despite WAIT stalls (one latch, right byte)";
  EXPECT_GT(r.tstates, 35u) << "WAIT inserted Tw cycles → more than the 35T no-wait total";
}

TEST(Z80b, StoreAToDESetsMemptr) {
  // LD DE,0x0041 ; LD A,0x88 ; LD (DE),A ; LD HL,0x0041 ; LD A,0 ; LD A,(HL) ; HALT
  Z80Regs r = run({0x11, 0x41, 0x00, 0x3E, 0x88, 0x12, 0x21, 0x41, 0x00, 0x3E, 0x00, 0x7E, 0x76});
  EXPECT_EQ(hi(r.af), 0x88) << "stored A to (DE), read it back";
  EXPECT_EQ(r.wz, 0x8842u) << "MEMPTR = (A<<8) | ((E+1)&0xFF) = 0x8842";
}

// ---- Round Z80-c: the CB prefix (rotates/shifts, BIT/RES/SET) ----

TEST(Z80c, RlcRegisterAndDoubleM1Refresh) {
  // LD A,0x85 ; RLC A ; HALT  → 0x0B, carry set; CB ops are 8T (two M1).
  Z80Regs r = run({0x3E, 0x85, 0xCB, 0x07, 0x76});
  EXPECT_EQ(hi(r.af), 0x0B) << "RLC 0x85 → 0x0B (bit7→bit0+carry)";
  EXPECT_TRUE(lo(r.af) & CF) << "carry = old bit 7";
  EXPECT_EQ(r.tstates, 7u + 8u + 4u) << "RLC r = 8T (M1 CB + M1 opcode)";
  EXPECT_EQ(r.r & 0x7F, 4u) << "LD(1) + RLC(2, double-M1) + HALT(1) = 4 refreshes";
}

TEST(Z80c, BitRegister) {
  // BIT 7,A with bit set → Z clear, S set
  Z80Regs s = run({0x3E, 0x80, 0xCB, 0x7F, 0x76});
  EXPECT_FALSE(lo(s.af) & ZF) << "bit 7 of 0x80 is set → Z clear";
  EXPECT_TRUE(lo(s.af) & SF) << "BIT 7 with bit set → S set";
  EXPECT_TRUE(lo(s.af) & HF) << "BIT always sets H";
  // BIT 0,A with bit clear → Z set
  Z80Regs c = run({0x3E, 0x80, 0xCB, 0x47, 0x76});
  EXPECT_TRUE(lo(c.af) & ZF) << "bit 0 of 0x80 is clear → Z set";
}

TEST(Z80c, SetAndResRegister) {
  Z80Regs rs = run({0x06, 0x00, 0xCB, 0xD8, 0x76});  // LD B,0 ; SET 3,B ; HALT
  EXPECT_EQ(hi(rs.bc), 0x08) << "SET 3 → bit 3";
  Z80Regs rr = run({0x06, 0xFF, 0xCB, 0x98, 0x76});  // LD B,0xFF ; RES 3,B ; HALT
  EXPECT_EQ(hi(rr.bc), 0xF7) << "RES 3 → clear bit 3";
}

TEST(Z80c, RlcHLReadModifyWrite) {
  // LD HL,0x0040 ; LD (HL),0x80 ; RLC (HL) ; LD A,(HL) ; HALT
  Z80Regs r = run({0x21, 0x40, 0x00, 0x36, 0x80, 0xCB, 0x06, 0x7E, 0x76});
  EXPECT_EQ(hi(r.af), 0x01) << "RLC 0x80 → 0x01, written back to (HL)";
  EXPECT_EQ(r.tstates, 10u + 10u + 15u + 7u + 4u) << "RLC (HL) = 15T (4+4+3+1+3)";
}

TEST(Z80c, BitFromHLTiming) {
  // LD HL,0x0040 ; LD (HL),0x80 ; BIT 7,(HL) ; HALT
  Z80Regs r = run({0x21, 0x40, 0x00, 0x36, 0x80, 0xCB, 0x7E, 0x76});
  EXPECT_FALSE(lo(r.af) & ZF) << "bit 7 set → Z clear";
  EXPECT_EQ(r.tstates, 10u + 10u + 12u + 4u) << "BIT n,(HL) = 12T (4+4+3+1, no write)";
}
