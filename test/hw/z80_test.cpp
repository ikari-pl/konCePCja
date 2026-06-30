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

/* I/O latch device: 256 ports keyed by the low address byte. Reads return a
 * deterministic seed (port_low ^ 0xFF) until written; writes latch the value so
 * an OUT followed by an IN of the same port reads it back — exercising both
 * directions without exposing device state to the test. */
struct IoDev {
  uint8_t latch[256];
};
void io_tick(void* self, const Bus* in, Bus* out) {
  IoDev* io = static_cast<IoDev*>(self);
  const uint8_t port = static_cast<uint8_t>(in->cpu.addr & 0xFF);
  if (in->cpu.iorq && in->cpu.wr) {
    io->latch[port] = in->cpu.data;
  } else if (in->cpu.iorq && in->cpu.rd) {
    out->cpu.data = io->latch[port];
  }
}
void io_reset(void* self) {
  IoDev* io = static_cast<IoDev*>(self);
  for (int i = 0; i < 256; ++i) io->latch[i] = static_cast<uint8_t>(i ^ 0xFF);
}
size_t io_size(const void*) { return sizeof(IoDev); }
void io_save(const void* s, void* b) { std::memcpy(b, s, sizeof(IoDev)); }
void io_load(void* s, const void* b) { std::memcpy(s, b, sizeof(IoDev)); }
Device io_device(IoDev* s) {
  return Device{s, "io", io_tick, io_reset, io_size, io_save, io_load};
}

/* Interrupt-line driver: holds IRQ (level) and pulses NMI to a rising edge at a
 * given board tick. `tick`/config are plain fields; reset only zeroes the tick. */
struct IntLine {
  bool irq;
  int nmi_at;  // board tick at which NMI goes high (rising edge); <0 = never
  int tick;
};
void intline_tick(void* self, const Bus*, Bus* out) {
  IntLine* s = static_cast<IntLine*>(self);
  if (s->irq) out->cpu.irq = true;                       // WIRED-OR level
  if (s->nmi_at >= 0 && s->tick >= s->nmi_at) out->cpu.nmi = true;
  s->tick++;
}
void intline_reset(void* self) { static_cast<IntLine*>(self)->tick = 0; }
size_t intline_size(const void*) { return sizeof(IntLine); }
void intline_save(const void* s, void* b) { std::memcpy(b, s, sizeof(IntLine)); }
void intline_load(void* s, const void* b) { std::memcpy(s, b, sizeof(IntLine)); }
Device intline_device(IntLine* s) {
  return Device{s, "int", intline_tick, intline_reset, intline_size, intline_save, intline_load};
}

/* Run a program from 0x0000 until the Z80 halts (programs end with HALT 0x76).
 * Returns the final architectural state, with tstates frozen at the HALT. */
struct Rig {
  std::unique_ptr<Ram> ram = std::make_unique<Ram>();
  std::unique_ptr<IoDev> io = std::make_unique<IoDev>();
  IntLine il{false, -1, 0};
  std::vector<uint8_t> z80mem = std::vector<uint8_t>(z80_state_size());
  Board board;
  Device zdev;
};

Z80Regs run_core(const std::vector<uint8_t>& program, bool irq, int nmi_at) {
  static Rig rig;  // reused; reset below
  rig.ram = std::make_unique<Ram>();
  std::memset(rig.ram->cells, 0, sizeof(Ram::cells));
  size_t i = 0;
  for (uint8_t b : program) rig.ram->cells[i++] = b;

  rig.il = IntLine{irq, nmi_at, 0};
  rig.z80mem.assign(z80_state_size(), 0);
  rig.zdev = z80_init(rig.z80mem.data());

  board_init(&rig.board);
  board_add(&rig.board, clock_device());
  board_add(&rig.board, ram_device(rig.ram.get()));
  board_add(&rig.board, io_device(rig.io.get()));
  board_add(&rig.board, intline_device(&rig.il));
  board_add(&rig.board, rig.zdev);
  board_reset(&rig.board);

  Z80Regs r{};
  for (int tick = 0; tick < 4000; ++tick) {
    board_tick(&rig.board);
    z80_peek(&rig.zdev, &r);
    // Stop at HALT — but if an NMI edge is scheduled, run well past it first so a
    // HALT meant to be *woken* by that NMI doesn't end the run prematurely.
    if (r.halted && (nmi_at < 0 || tick > nmi_at + 80)) break;
  }
  return r;
}

Z80Regs run(const std::vector<uint8_t>& program) { return run_core(program, false, -1); }
Z80Regs run(std::initializer_list<uint8_t> program) {
  return run_core(std::vector<uint8_t>(program), false, -1);
}
// Run with IRQ held (level) and/or an NMI pulse at board tick `nmi_at`.
Z80Regs run_int(const std::vector<uint8_t>& program, bool irq, int nmi_at = -1) {
  return run_core(program, irq, nmi_at);
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

TEST(Z80a, EveryDefinedOpcodeIsDecoded) {
  // The whole defined instruction set is decoded; only the genuinely-undefined
  // ED-page opcodes (2-byte NOPs on real silicon) remain flagged. A clean op
  // never silently computes garbage, and the decoder spans all five pages.
  EXPECT_TRUE(run({0xED, 0x00, 0x76}).unimplemented) << "undefined ED opcode";
  EXPECT_TRUE(run({0xED, 0xFF, 0x76}).unimplemented) << "undefined ED opcode";
  // A sampling across every page stays clean:
  EXPECT_FALSE(run({0x78, 0x76}).unimplemented) << "main: LD A,B";
  EXPECT_FALSE(run({0xCB, 0x47, 0x76}).unimplemented) << "CB: BIT 0,A";
  EXPECT_FALSE(run({0xED, 0xB0, 0x76}).unimplemented) << "ED: LDIR";
  EXPECT_FALSE(run({0xDD, 0x09, 0x76}).unimplemented) << "DD: ADD IX,BC";
  EXPECT_FALSE(run({0xFD, 0x21, 0x00, 0x00, 0x76}).unimplemented) << "FD: LD IY,nn";
  EXPECT_FALSE(run({0xDD, 0xCB, 0x00, 0x46, 0x76}).unimplemented) << "DDCB: BIT 0,(IX+0)";
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

// ---- Round Z80-c (2/2): the ED prefix (NEG, 16-bit ADC/SBC, LD A,I/R, IM, RRD/RLD) ----

TEST(Z80c, Neg) {
  Z80Regs r = run({0x3E, 0x01, 0xED, 0x44, 0x76});  // LD A,1 ; NEG ; HALT
  EXPECT_EQ(hi(r.af), 0xFF) << "0 - 1 = 0xFF";
  EXPECT_TRUE(lo(r.af) & CF) << "NEG of nonzero sets carry";
  EXPECT_TRUE(lo(r.af) & NF) << "NEG sets N";
  EXPECT_EQ(r.tstates, 7u + 8u + 4u) << "NEG = 8T";

  Z80Regs o = run({0x3E, 0x80, 0xED, 0x44, 0x76});  // LD A,0x80 ; NEG
  EXPECT_EQ(hi(o.af), 0x80) << "NEG 0x80 = 0x80";
  EXPECT_TRUE(lo(o.af) & PF) << "NEG 0x80 overflows → P/V set";
}

TEST(Z80c, Sbc16AndAdc16) {
  // XOR A (clear carry) ; LD HL,5 ; LD DE,3 ; SBC HL,DE ; HALT
  Z80Regs s = run({0xAF, 0x21, 0x05, 0x00, 0x11, 0x03, 0x00, 0xED, 0x52, 0x76});
  EXPECT_EQ(s.hl, 0x0002u) << "5 - 3 = 2";
  EXPECT_EQ(s.tstates, 4u + 10u + 10u + 15u + 4u) << "SBC HL,rr = 15T";

  // XOR A ; LD HL,0x1000 ; LD DE,0x0234 ; ADC HL,DE ; HALT
  Z80Regs a = run({0xAF, 0x21, 0x00, 0x10, 0x11, 0x34, 0x02, 0xED, 0x5A, 0x76});
  EXPECT_EQ(a.hl, 0x1234u) << "0x1000 + 0x0234 = 0x1234";
}

TEST(Z80c, LoadAFromIAndIM) {
  // LD A,0x42 ; LD I,A ; LD A,0 ; LD A,I ; HALT
  Z80Regs r = run({0x3E, 0x42, 0xED, 0x47, 0x3E, 0x00, 0xED, 0x57, 0x76});
  EXPECT_EQ(r.i, 0x42) << "I = A";
  EXPECT_EQ(hi(r.af), 0x42) << "A = I";

  Z80Regs m = run({0xED, 0x5E, 0x76});  // IM 2 ; HALT
  EXPECT_EQ(m.im, 2) << "IM 2";
}

TEST(Z80c, Rrd) {
  // LD HL,0x0040 ; LD (HL),0x12 ; LD A,0x34 ; RRD ; LD B,(HL) ; HALT
  Z80Regs r = run({0x21, 0x40, 0x00, 0x36, 0x12, 0x3E, 0x34, 0xED, 0x67, 0x46, 0x76});
  EXPECT_EQ(hi(r.af), 0x32) << "RRD: A = (A&0xF0) | ((HL)&0x0F) = 0x30|0x02";
  EXPECT_EQ(hi(r.bc), 0x41) << "RRD: (HL) = (A_low<<4) | ((HL)>>4) = 0x41";
}

TEST(Z80c, Rld) {
  // LD HL,0x0040 ; LD (HL),0x12 ; LD A,0x34 ; RLD ; LD B,(HL) ; HALT
  Z80Regs r = run({0x21, 0x40, 0x00, 0x36, 0x12, 0x3E, 0x34, 0xED, 0x6F, 0x46, 0x76});
  EXPECT_EQ(hi(r.af), 0x31) << "RLD: A = (A&0xF0) | ((HL)>>4) = 0x30|0x01";
  EXPECT_EQ(hi(r.bc), 0x24) << "RLD: (HL) = ((HL)<<4) | (A&0x0F) = 0x24";
}

// ---- Z80-c hardening: the subtle flag paths the reviewer flagged ----

TEST(Z80c, BitFromHLTakesXYFromMemptr) {
  // Seed WZ via LD A,(BC) (WZ=BC+1=0x2800), then BIT 0,(HL): undoc X/Y come from
  // WZ.hi (0x28), NOT from the tested value.
  Z80Regs r = run({0x06, 0x27, 0x0E, 0xFF, 0x0A,        // LD B,0x27; LD C,0xFF; LD A,(BC) → WZ=0x2800
                   0x21, 0x40, 0x00, 0x36, 0x01,        // LD HL,0x40; LD (HL),0x01
                   0xCB, 0x46, 0x76});                  // BIT 0,(HL); HALT
  EXPECT_EQ(r.wz, 0x2800u);
  EXPECT_EQ(lo(r.af) & static_cast<uint8_t>(YF | XF), 0x28) << "BIT n,(HL): X/Y from WZ.hi";
}

TEST(Z80c, Adc16OverflowSignHalf) {
  // XOR A ; LD HL,0x7FFF ; LD DE,1 ; ADC HL,DE → 0x8000
  Z80Regs r = run({0xAF, 0x21, 0xFF, 0x7F, 0x11, 0x01, 0x00, 0xED, 0x5A, 0x76});
  EXPECT_EQ(r.hl, 0x8000u);
  EXPECT_TRUE(lo(r.af) & SF) << "0x8000 → S set";
  EXPECT_TRUE(lo(r.af) & PF) << "0x7FFF+1 → signed overflow";
  EXPECT_FALSE(lo(r.af) & CF) << "no 16-bit carry";
}

TEST(Z80c, SraAndSllShifts) {
  // XOR A ; LD A,0x81 ; SRA A → 0xC0 (sign-extended), carry = old bit0
  Z80Regs sra = run({0xAF, 0x3E, 0x81, 0xCB, 0x2F, 0x76});
  EXPECT_EQ(hi(sra.af), 0xC0) << "SRA sign-extends bit 7";
  EXPECT_TRUE(lo(sra.af) & CF);
  // XOR A ; LD A,0x80 ; SLL A → 0x01 (bit0 forced to 1), carry = old bit7
  Z80Regs sll = run({0xAF, 0x3E, 0x80, 0xCB, 0x37, 0x76});
  EXPECT_EQ(hi(sll.af), 0x01) << "SLL forces bit 0";
  EXPECT_TRUE(lo(sll.af) & CF);
}

TEST(Z80c, NegOfZero) {
  Z80Regs r = run({0x3E, 0x00, 0xED, 0x44, 0x76});  // LD A,0 ; NEG
  EXPECT_EQ(hi(r.af), 0x00);
  EXPECT_TRUE(lo(r.af) & ZF) << "NEG 0 = 0 → Z set";
  EXPECT_FALSE(lo(r.af) & CF) << "NEG 0 → no borrow, carry clear";
}

// ---- Unprefixed accumulator/misc: rotates, DAA, CPL, SCF/CCF, ADD HL,rr, EX ----

TEST(Z80d, AccumulatorRotates) {
  Z80Regs r = run({0x3E, 0x80, 0x07, 0x76});  // LD A,0x80 ; RLCA
  EXPECT_EQ(hi(r.af), 0x01) << "RLCA 0x80 → 0x01";
  EXPECT_TRUE(lo(r.af) & CF) << "carry = old bit 7";
  EXPECT_EQ(r.tstates, 7u + 4u + 4u) << "RLCA = 4T";
}

TEST(Z80d, ScfCcfCplDaa) {
  EXPECT_TRUE(lo(run({0x37, 0x76}).af) & CF) << "SCF sets carry";
  Z80Regs ccf = run({0x37, 0x3F, 0x76});  // SCF ; CCF
  EXPECT_FALSE(lo(ccf.af) & CF) << "CCF clears the just-set carry";
  EXPECT_TRUE(lo(ccf.af) & HF) << "CCF: H = old carry";
  Z80Regs cpl = run({0x3E, 0x0F, 0x2F, 0x76});  // LD A,0x0F ; CPL
  EXPECT_EQ(hi(cpl.af), 0xF0) << "CPL = ~A";
  EXPECT_TRUE((lo(cpl.af) & HF) && (lo(cpl.af) & NF)) << "CPL sets H,N";
  // XOR A clears carry first (INC A preserves CF; reset F has it set), so DAA
  // applies only the low-nibble +6 adjust, not the +0x60 high-nibble one.
  Z80Regs daa = run({0xAF, 0x3E, 0x09, 0x3C, 0x27, 0x76});  // XOR A;LD A,9;INC A;DAA
  EXPECT_EQ(hi(daa.af), 0x10) << "0x0A DAA → 0x10 (BCD adjust)";
}

TEST(Z80d, AddHL16AndExAf) {
  Z80Regs r = run({0x21, 0x00, 0x10, 0x01, 0x34, 0x02, 0x09, 0x76});  // LD HL,0x1000;LD BC,0x0234;ADD HL,BC
  EXPECT_EQ(r.hl, 0x1234u);
  EXPECT_EQ(r.tstates, 10u + 10u + 11u + 4u) << "ADD HL,rr = 11T";
  // LD A,0x11 ; EX AF,AF' ; LD A,0x22 ; EX AF,AF' → A back to 0x11
  Z80Regs ex = run({0x3E, 0x11, 0x08, 0x3E, 0x22, 0x08, 0x76});
  EXPECT_EQ(hi(ex.af), 0x11) << "EX AF,AF' swaps the alt set back";
}

// ---- Control flow: jumps, calls, returns, stack, RST, EX ----

TEST(Z80e, JpAndJpHl) {
  // LD A,0x11 ; JP 0x0007 ; LD A,0xFF (skipped) ; HALT@7
  Z80Regs jp = run({0x3E, 0x11, 0xC3, 0x07, 0x00, 0x3E, 0xFF, 0x76});
  EXPECT_EQ(hi(jp.af), 0x11) << "JP skipped the LD A,0xFF";
  EXPECT_EQ(jp.wz, 0x0007u) << "JP sets WZ = target";
  EXPECT_EQ(jp.tstates, 7u + 10u + 4u) << "JP nn = 10T";
  // LD A,0x99 ; LD HL,0x0008 ; JP (HL) ; LD A,0x00 (skipped) ; HALT@8
  Z80Regs jphl = run({0x3E, 0x99, 0x21, 0x08, 0x00, 0xE9, 0x3E, 0x00, 0x76});
  EXPECT_EQ(hi(jphl.af), 0x99) << "JP (HL) jumped past LD A,0x00";
}

TEST(Z80e, JrTakenAndConditionalNotTaken) {
  // LD A,0x22 ; JR +2 → HALT@6 (skips LD A,0xFF)
  Z80Regs jr = run({0x3E, 0x22, 0x18, 0x02, 0x3E, 0xFF, 0x76});
  EXPECT_EQ(hi(jr.af), 0x22) << "JR +2 skipped the LD A,0xFF";
  EXPECT_EQ(jr.tstates, 7u + 12u + 4u) << "JR taken = 12T";
  // XOR A (Z set) ; JR NZ,+2 (NOT taken) ; LD A,0xFF ; HALT
  Z80Regs nz = run({0xAF, 0x20, 0x02, 0x3E, 0xFF, 0x76});
  EXPECT_EQ(hi(nz.af), 0xFF) << "JR NZ not taken (Z set) → fell through to LD A,0xFF";
}

TEST(Z80e, DjnzLoop) {
  // LD B,3 ; LD C,0 ; loop: INC C ; DJNZ loop ; HALT
  Z80Regs r = run({0x06, 0x03, 0x0E, 0x00, 0x0C, 0x10, 0xFD, 0x76});
  EXPECT_EQ(lo(r.bc), 0x03) << "C incremented once per DJNZ iteration";
  EXPECT_EQ(hi(r.bc), 0x00) << "B counted down to 0";
}

TEST(Z80e, CallRetRoundTrip) {
  // CALL 0x0006 ; HALT@3 ; pad ; 0x0006: LD A,0x55 ; RET
  Z80Regs r = run({0xCD, 0x06, 0x00, 0x76, 0x00, 0x00, 0x3E, 0x55, 0xC9});
  EXPECT_EQ(hi(r.af), 0x55) << "subroutine ran";
  EXPECT_EQ(r.sp, 0xFFFFu) << "RET restored SP (CALL pushed, RET popped)";
  EXPECT_EQ(r.tstates, 17u + 7u + 10u + 4u) << "CALL 17T + LD 7T + RET 10T + HALT 4T";
  // CALL NZ when Z set → not taken, SP untouched
  Z80Regs nt = run({0xAF, 0xC4, 0x08, 0x00, 0x76});
  EXPECT_EQ(nt.sp, 0xFFFFu) << "CALL NZ not taken pushed nothing";
}

TEST(Z80e, PushPopAndExSp) {
  // LD BC,0x1234 ; PUSH BC ; POP HL → HL=0x1234, SP restored
  Z80Regs pp = run({0x01, 0x34, 0x12, 0xC5, 0xE1, 0x76});
  EXPECT_EQ(pp.hl, 0x1234u) << "PUSH BC / POP HL transfers via the stack";
  EXPECT_EQ(pp.sp, 0xFFFFu) << "push then pop balances SP";
  // LD HL,0xAAAA ; PUSH HL ; LD HL,0xBBBB ; EX (SP),HL ; POP BC
  Z80Regs ex = run({0x21, 0xAA, 0xAA, 0xE5, 0x21, 0xBB, 0xBB, 0xE3, 0xC1, 0x76});
  EXPECT_EQ(ex.hl, 0xAAAAu) << "EX (SP),HL loaded HL from the stacked word";
  EXPECT_EQ(ex.bc, 0xBBBBu) << "EX (SP),HL wrote HL onto the stack";
}

TEST(Z80e, RstAndExxExDeHl) {
  // RST 0x10 → jumps to 0x0010 (HALT there)
  std::vector<uint8_t> prog(0x11, 0x00);
  prog[0] = 0xD7;   // RST 10h
  prog[0x10] = 0x76;  // HALT
  Z80Regs rst = run(prog);
  EXPECT_EQ(rst.wz, 0x0010u) << "RST 10h sets PC/WZ = 0x0010";
  EXPECT_EQ(rst.sp, 0xFFFDu) << "RST pushed the return address (2 bytes)";
  // LD HL,0x1111 ; LD DE,0x2222 ; EX DE,HL
  Z80Regs ex = run({0x21, 0x11, 0x11, 0x11, 0x22, 0x22, 0xEB, 0x76});
  EXPECT_EQ(ex.hl, 0x2222u);
  EXPECT_EQ(ex.de, 0x1111u);
  // LD BC,0x0102 ; EXX → BC' holds it, BC reads the (zero) alt set
  Z80Regs exx = run({0x01, 0x02, 0x01, 0xD9, 0x76});
  EXPECT_EQ(exx.bc_, 0x0102u) << "EXX moved BC into the alternate set";
}

// ---- Absolute (nn) memory loads + I/O ----

TEST(Z80f, AbsoluteMemoryLoads) {
  // LD HL,0x1234 ; LD (0x4000),HL ; LD HL,0 ; LD HL,(0x4000)
  Z80Regs hl = run({0x21, 0x34, 0x12, 0x22, 0x00, 0x40, 0x21, 0x00, 0x00,
                    0x2A, 0x00, 0x40, 0x76});
  EXPECT_EQ(hl.hl, 0x1234u) << "LD (nn),HL then LD HL,(nn) round-trips";
  EXPECT_EQ(hl.wz, 0x4001u) << "WZ = nn+1";
  // LD A,0x77 ; LD (0x4001),A ; LD A,0 ; LD A,(0x4001)
  Z80Regs a = run({0x3E, 0x77, 0x32, 0x01, 0x40, 0x3E, 0x00, 0x3A, 0x01, 0x40, 0x76});
  EXPECT_EQ(hi(a.af), 0x77) << "LD (nn),A then LD A,(nn) round-trips";
  // LD BC,0xCAFE ; LD (0x4002),BC ; LD BC,0 ; LD BC,(0x4002)  (ED 43 / ED 4B)
  Z80Regs bc = run({0x01, 0xFE, 0xCA, 0xED, 0x43, 0x02, 0x40, 0x01, 0x00, 0x00,
                    0xED, 0x4B, 0x02, 0x40, 0x76});
  EXPECT_EQ(bc.bc, 0xCAFEu) << "ED LD (nn),rr / LD rr,(nn) round-trips";
}

TEST(Z80f, InOutPortIO) {
  // IN A,(0x55): latch seeds port 0x55 to 0x55^0xFF = 0xAA
  Z80Regs in = run({0xDB, 0x55, 0x76});
  EXPECT_EQ(hi(in.af), 0xAA) << "IN A,(n) reads the port latch";
  // LD A,0x3C ; OUT (0x20),A ; LD A,0 ; IN A,(0x20) → 0x3C
  Z80Regs rt = run({0x3E, 0x3C, 0xD3, 0x20, 0x3E, 0x00, 0xDB, 0x20, 0x76});
  EXPECT_EQ(hi(rt.af), 0x3C) << "OUT (n),A then IN A,(n) round-trips through the latch";
  EXPECT_EQ(rt.tstates, 7u + 11u + 7u + 11u + 4u) << "IN/OUT (n) = 11T each";
  // LD BC,0x0042 ; LD A,9 ; OUT (C),A ; IN D,(C) → D=9
  Z80Regs c = run({0x01, 0x42, 0x00, 0x3E, 0x09, 0xED, 0x79, 0xED, 0x50, 0x76});
  EXPECT_EQ(hi(c.de), 0x09) << "OUT (C),A then IN D,(C) round-trips";
  EXPECT_FALSE(lo(c.af) & ZF) << "IN r,(C) sets flags from the value (9 → not zero)";
}

// ---- Block operations + RETN/RETI ----

TEST(Z80g, Ldir) {
  // LD HL,0x000F ; LD DE,0x4000 ; LD BC,3 ; LDIR ; LD A,(0x4002) ; HALT ; data
  Z80Regs r = run({0x21, 0x0F, 0x00, 0x11, 0x00, 0x40, 0x01, 0x03, 0x00,
                   0xED, 0xB0, 0x3A, 0x02, 0x40, 0x76, 0xAA, 0xBB, 0xCC});
  EXPECT_EQ(hi(r.af), 0xCC) << "LDIR copied 3 bytes; last is 0xCC";
  EXPECT_EQ(r.bc, 0x0000u) << "LDIR ran BC down to 0";
  EXPECT_EQ(r.hl, 0x0012u) << "HL advanced past the source block";
  EXPECT_EQ(r.de, 0x4003u) << "DE advanced past the dest block";
}

TEST(Z80g, Cpir) {
  // LD A,0xCC ; LD HL,0x000F ; LD BC,3 ; CPIR ; HALT ; pad ; data AA BB CC
  Z80Regs r = run({0x3E, 0xCC, 0x21, 0x0F, 0x00, 0x01, 0x03, 0x00, 0xED, 0xB1,
                   0x76, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC});
  EXPECT_TRUE(lo(r.af) & ZF) << "CPIR found 0xCC → Z set";
  EXPECT_EQ(r.hl, 0x0012u) << "HL stopped just past the match";
  EXPECT_EQ(r.bc, 0x0000u) << "BC exhausted exactly at the match";
}

TEST(Z80g, IniAndOuti) {
  // INI: read port 0x42 (latch seed 0x42^0xFF=0xBD) into (0x4000); B 1→0
  Z80Regs ini = run({0x21, 0x00, 0x40, 0x01, 0x42, 0x01, 0xED, 0xA2,
                     0x3A, 0x00, 0x40, 0x76});
  EXPECT_EQ(hi(ini.af), 0xBD) << "INI stored the input byte at (HL)";
  EXPECT_EQ(hi(ini.bc), 0x00) << "INI decremented B";
  // OUTI: (HL)=0x77 out to port (B-1=0,C=0x33); read it back with IN A,(0x33)
  Z80Regs outi = run({0x21, 0x0C, 0x00, 0x01, 0x33, 0x01, 0xED, 0xA3,
                      0xDB, 0x33, 0x76, 0x00, 0x77});
  EXPECT_EQ(hi(outi.af), 0x77) << "OUTI wrote (HL) to the port; IN reads it back";
}

TEST(Z80g, RetiPopsLikeRet) {
  // CALL 0x0006 ; HALT ; pad ; RETI — verifies the POP-PC mechanics (the
  // IFF1<-IFF2 restore is covered non-vacuously in Z80i.RetnRestoresIff).
  Z80Regs r = run({0xCD, 0x06, 0x00, 0x76, 0x00, 0x00, 0xED, 0x4D});
  EXPECT_EQ(r.sp, 0xFFFFu) << "RETI popped the return address";
  EXPECT_EQ(r.pc, 0x0004u) << "RETI returned to the instruction after CALL";
}

// ---- DD/FD index prefix: IX/IY, (IX+d), DDCB ----

TEST(Z80h, LoadIxAndPairOps) {
  // DD 21 nn  LD IX,0x1234 ; DD 23 INC IX ; DD 09 ADD IX,BC (BC=1)
  Z80Regs r = run({0xDD, 0x21, 0x34, 0x12, 0xDD, 0x23, 0x01, 0x01, 0x00,
                   0xDD, 0x09, 0x76});
  EXPECT_EQ(r.ix, 0x1236u) << "LD IX,nn (0x1234) + INC IX (0x1235) + ADD IX,BC(1) = 0x1236";
  EXPECT_EQ(r.hl, 0x0000u) << "HL untouched — the prefix retargets IX only";
  // Exact prefixed timing: LD IX,nn 14T + INC IX 10T + LD BC,nn 10T + ADD IX,BC 15T + HALT 4T
  EXPECT_EQ(r.tstates, 14u + 10u + 10u + 15u + 4u) << "DD-prefixed timings (each +4 for the prefix M1)";
  // FD 21 LD IY,0xBEEF
  Z80Regs iy = run({0xFD, 0x21, 0xEF, 0xBE, 0x76});
  EXPECT_EQ(iy.iy, 0xBEEFu) << "FD prefix selects IY";
}

TEST(Z80h, IxDisplacementLoadStore) {
  // LD IX,0x4000 ; LD (IX+2),0x99 ; LD A,(IX+2) ; HALT
  Z80Regs r = run({0xDD, 0x21, 0x00, 0x40, 0xDD, 0x36, 0x02, 0x99,
                   0xDD, 0x7E, 0x02, 0x76});
  EXPECT_EQ(hi(r.af), 0x99) << "LD (IX+d),n then LD A,(IX+d) round-trips";
  EXPECT_EQ(r.wz, 0x4002u) << "WZ = the (IX+d) effective address";
  // LD IX,0x4000 ; LD B,0x55 ; LD (IX+1),B ; LD C,(IX+1)
  Z80Regs rb = run({0xDD, 0x21, 0x00, 0x40, 0x06, 0x55, 0xDD, 0x70, 0x01,
                    0xDD, 0x4E, 0x01, 0x76});
  EXPECT_EQ(lo(rb.bc), 0x55) << "LD (IX+d),B then LD C,(IX+d): register operand keeps real B/C";
}

TEST(Z80h, IxhHalfRegisterAndNegativeDisplacement) {
  // DD 26 nn = LD IXH,0xAB (undocumented half-register), then LD A,IXH (DD 7C)
  Z80Regs r = run({0xDD, 0x26, 0xAB, 0xDD, 0x7C, 0x76});
  EXPECT_EQ(hi(r.af), 0xABu) << "LD IXH,n / LD A,IXH use the index high half, not H";
  EXPECT_EQ(hi(r.hl), 0x00) << "real H is untouched";
  // negative displacement: LD IX,0x4010 ; LD (IX-1),0x7E ; LD A,(IX-1)
  Z80Regs neg = run({0xDD, 0x21, 0x10, 0x40, 0xDD, 0x36, 0xFF, 0x7E,
                     0xDD, 0x7E, 0xFF, 0x76});
  EXPECT_EQ(hi(neg.af), 0x7E) << "displacement is signed: (IX-1) addresses 0x400F";
  EXPECT_EQ(neg.wz, 0x400Fu);
}

TEST(Z80h, DdcbBitSetAndRes) {
  // LD IX,0x4000 ; LD (IX+0),0x00 ; SET 3,(IX+0) ; LD A,(IX+0)
  Z80Regs set = run({0xDD, 0x21, 0x00, 0x40, 0xDD, 0x36, 0x00, 0x00,
                     0xDD, 0xCB, 0x00, 0xDE, 0xDD, 0x7E, 0x00, 0x76});
  EXPECT_EQ(hi(set.af), 0x08) << "SET 3,(IX+0) set bit 3";
  // LD IX,0x4000 ; LD (IX+0),0xFF ; RES 0,(IX+0) ; LD A,(IX+0)
  Z80Regs res = run({0xDD, 0x21, 0x00, 0x40, 0xDD, 0x36, 0x00, 0xFF,
                     0xDD, 0xCB, 0x00, 0x86, 0xDD, 0x7E, 0x00, 0x76});
  EXPECT_EQ(hi(res.af), 0xFE) << "RES 0,(IX+0) cleared bit 0";
}

// ---- Interrupts: IM1, IM2, NMI, HALT-wake, EI delay ----

TEST(Z80i, MaskableIm1) {
  // IM 1 ; EI ; JR -2 (loop) ; handler@0x38: LD A,0x42 ; HALT
  std::vector<uint8_t> prog(0x3A, 0x00);
  prog[0] = 0xED; prog[1] = 0x56;   // IM 1
  prog[2] = 0xFB;                   // EI
  prog[3] = 0x18; prog[4] = 0xFE;   // JR -2 (self loop)
  prog[0x38] = 0x3E; prog[0x39] = 0x42;  // LD A,0x42
  prog[0x3A] = 0x76;                // HALT
  Z80Regs r = run_int(prog, /*irq=*/true);
  EXPECT_EQ(hi(r.af), 0x42) << "IM1 vectored to 0x0038 and ran the handler";
  EXPECT_EQ(r.sp, 0xFFFDu) << "INT pushed the 2-byte return address";
  EXPECT_EQ(r.iff1, 0) << "INT acceptance cleared IFF1";
}

TEST(Z80i, Im1AcceptanceTStatesAndRefresh) {
  // Deterministic, loop-free path so the totals are exact:
  //   IM1 (8T, 2 M1/refreshes) ; EI (4T,1) ; NOP (4T,1; ei_delay defers INT here)
  //   ; <NOP slot> — INT accepted at this boundary ; handler@0x38: HALT.
  // IM1 ack = 13T (1 ack refresh). Handler HALT M1 = 4T (1 refresh).
  // Total = 8+4+4 (pre-INT) + 13 (ack) + 4 (HALT) = 33T. R = 2+1+1 +1 +1 = 6.
  std::vector<uint8_t> prog(0x39, 0x00);
  prog[0] = 0xED; prog[1] = 0x56;   // IM 1
  prog[2] = 0xFB;                   // EI
  prog[3] = 0x00;                   // NOP (the post-EI instruction; INT deferred)
  prog[4] = 0x00;                   // NOP slot — INT is accepted at this fetch
  prog[0x38] = 0x76;               // HALT
  Z80Regs r = run_int(prog, /*irq=*/true);
  EXPECT_EQ(r.tstates, 33u) << "IM1 acknowledge is 13T (a +1 here would mean 34)";
  EXPECT_EQ(r.r & 0x7F, 6u) << "the INT acknowledge bumps R exactly once";
}

TEST(Z80i, MaskableIm2Vectors) {
  // Build a vector table entry at (I<<8 | 0xFF) pointing to the handler@0x0050.
  // The data bus floats to 0xFF during the ack (CPC behaviour), so vector=0xFF.
  std::vector<uint8_t> prog(0x53, 0x00);
  prog[0] = 0x21; prog[1] = 0x50; prog[2] = 0x00;  // LD HL,0x0050
  prog[3] = 0x22; prog[4] = 0xFF; prog[5] = 0x90;  // LD (0x90FF),HL
  prog[6] = 0x3E; prog[7] = 0x90;                  // LD A,0x90
  prog[8] = 0xED; prog[9] = 0x47;                  // LD I,A  (I=0x90)
  prog[10] = 0xED; prog[11] = 0x5E;                // IM 2
  prog[12] = 0xFB;                                 // EI
  prog[13] = 0x18; prog[14] = 0xFE;                // JR -2
  prog[0x50] = 0x3E; prog[0x51] = 0x99;            // LD A,0x99
  prog[0x52] = 0x76;                               // HALT
  Z80Regs r = run_int(prog, /*irq=*/true);
  EXPECT_EQ(hi(r.af), 0x99) << "IM2 read the table at (I<<8|0xFF) and jumped to the handler";
}

TEST(Z80i, NmiVectorsTo0066) {
  // JR -2 (loop) ; handler@0x66: LD A,0x77 ; HALT.  NMI pulse mid-run.
  std::vector<uint8_t> prog(0x69, 0x00);
  prog[0] = 0x18; prog[1] = 0xFE;          // JR -2 (self loop)
  prog[0x66] = 0x3E; prog[0x67] = 0x77;    // LD A,0x77
  prog[0x68] = 0x76;                       // HALT
  Z80Regs r = run_int(prog, /*irq=*/false, /*nmi_at=*/30);
  EXPECT_EQ(hi(r.af), 0x77) << "NMI vectored to 0x0066 regardless of IFF1";
  EXPECT_EQ(r.sp, 0xFFFDu) << "NMI pushed the return address";
}

TEST(Z80i, NmiWakesFromHalt) {
  // HALT immediately ; NMI later wakes it → handler@0x66 sets A
  std::vector<uint8_t> prog(0x69, 0x00);
  prog[0] = 0x76;                          // HALT
  prog[0x66] = 0x3E; prog[0x67] = 0x33;    // LD A,0x33
  prog[0x68] = 0x76;                       // HALT
  Z80Regs r = run_int(prog, /*irq=*/false, /*nmi_at=*/30);
  EXPECT_EQ(hi(r.af), 0x33) << "NMI woke the CPU out of HALT and ran the handler";
}

// ============================================================================
// Z80j — review-driven coverage: taken conditional branches, undocumented
// flag sources, D-variant block ops, IY/(IY+d)/FDCB, IM0, and the Q rule across
// a prefix (locking in that DD/FD is transparent to the SCF/CCF Q latch).
// ============================================================================

TEST(Z80j, ConditionalReturnTaken) {
  // XOR A (Z set) ; CALL 0x0006 ; HALT@4 ; pad ; 0x6: LD A,0x42 ; RET Z
  Z80Regs r = run({0xAF, 0xCD, 0x06, 0x00, 0x76, 0x00, 0x3E, 0x42, 0xC8});
  EXPECT_EQ(hi(r.af), 0x42) << "subroutine ran";
  EXPECT_EQ(r.sp, 0xFFFFu) << "RET Z taken popped the return address";
  EXPECT_EQ(r.tstates, 4u + 17u + 7u + 11u + 4u) << "RET cc taken = 11T";
}

TEST(Z80j, ConditionalCallAndJumpsTaken) {
  // LD A,1 ; OR A (clears Z — LD alone doesn't, and reset F has Z set) ;
  // CALL NZ,0x0008 ; HALT@6 ; pad ; LD A,0x42 ; RET
  Z80Regs call = run({0x3E, 0x01, 0xB7, 0xC4, 0x08, 0x00, 0x76, 0x00, 0x3E, 0x42, 0xC9});
  EXPECT_EQ(hi(call.af), 0x42) << "CALL NZ taken entered the subroutine";
  EXPECT_EQ(call.sp, 0xFFFFu);
  // XOR A (Z set) ; JR Z,+2 ; LD A,0xFF ; HALT@5 ; HALT@6
  Z80Regs jr = run({0xAF, 0x28, 0x02, 0x3E, 0xFF, 0x76, 0x76});
  EXPECT_EQ(hi(jr.af), 0x00) << "JR Z taken skipped LD A,0xFF";
  EXPECT_EQ(jr.tstates, 4u + 12u + 4u) << "JR cc taken = 12T";
  // XOR A (Z set) ; JP Z,0x0006 ; LD A,0xFF ; HALT@6
  Z80Regs jp = run({0xAF, 0xCA, 0x06, 0x00, 0x3E, 0xFF, 0x76});
  EXPECT_EQ(hi(jp.af), 0x00) << "JP Z taken skipped LD A,0xFF";
  EXPECT_EQ(jp.wz, 0x0006u);
}

TEST(Z80j, LdirUndocumentedXYFlags) {
  // n = A + transferred byte = 0x01 + 0x05 = 0x06 → YF (n bit1) set, XF (n bit3) clear.
  // Source byte 0x05 sits at index 14 = 0x000E, so HL must be 0x000E.
  Z80Regs r = run({0x3E, 0x01, 0x21, 0x0E, 0x00, 0x11, 0x00, 0x40, 0x01, 0x01, 0x00,
                   0xED, 0xB0, 0x76, 0x05});
  EXPECT_NE(lo(r.af) & YF, 0u) << "LDIR YF = bit 1 of (A + byte)";
  EXPECT_EQ(lo(r.af) & XF, 0u) << "LDIR XF = bit 3 of (A + byte)";
}

TEST(Z80j, ScfUsesPriorInstructionQ) {
  // LD A,0 ; CP 0x28 (writes F incl. YF|XF from operand; A stays 0; Q = that F) ; SCF.
  // SCF X/Y = ((Q ^ F) | A) & 0x28; with Q==F and A==0 the term cancels → X/Y = 0.
  // A regression using Q=0 would instead yield 0x28 — so this pins the Q source.
  Z80Regs r = run({0x3E, 0x00, 0xFE, 0x28, 0x37, 0x76});
  EXPECT_EQ(lo(r.af) & static_cast<uint8_t>(YF | XF), 0u)
      << "SCF consumes the prior instruction's Q (cancels here), not 0";
  EXPECT_TRUE(lo(r.af) & CF);
}

TEST(Z80j, QLatchIsTransparentAcrossPrefix) {
  // Same as above but with a DD prefix before SCF. A DD/FD prefix is part of its
  // own instruction and must NOT reset the Q latch — so DD SCF sees CP's Q just
  // like plain SCF, and X/Y still cancel to 0. (If the prefix wrongly committed
  // Q=0, this would read 0x28.)
  Z80Regs r = run({0x3E, 0x00, 0xFE, 0x28, 0xDD, 0x37, 0x76});
  EXPECT_EQ(lo(r.af) & static_cast<uint8_t>(YF | XF), 0u)
      << "the DD prefix is transparent to the Q latch";
}

TEST(Z80j, DdcbCopiesResultIntoRegister) {
  // DD CB 00 00 = RLC (IX+0) with z=0 → undocumented: result also stored into B.
  // RLC 0x81 = 0x03 (carry from bit 7).
  Z80Regs r = run({0xDD, 0x21, 0x00, 0x40, 0xDD, 0x36, 0x00, 0x81,
                   0xDD, 0xCB, 0x00, 0x00, 0x78, 0x76});
  EXPECT_EQ(hi(r.af), 0x03) << "LD A,B reads the DDCB result copied into B";
  EXPECT_EQ(hi(r.bc), 0x03) << "DDCB z!=6 stores the result into register z (B)";
  EXPECT_TRUE(lo(r.af) & CF) << "RLC 0x81 sets carry";
}

TEST(Z80j, DecrementingBlockOpAndIY) {
  // LDD: (DE)<-(HL), then HL--, DE--, BC--.
  Z80Regs ldd = run({0x21, 0x0D, 0x00, 0x11, 0x00, 0x40, 0x01, 0x01, 0x00,
                     0xED, 0xA8, 0x76, 0x00, 0xBB});
  EXPECT_EQ(ldd.hl, 0x000Cu) << "LDD decremented HL";
  EXPECT_EQ(ldd.de, 0x3FFFu) << "LDD decremented DE";
  EXPECT_EQ(ldd.bc, 0x0000u);
  // (IY+d) load/store via the FD prefix.
  Z80Regs iy = run({0xFD, 0x21, 0x00, 0x40, 0xFD, 0x36, 0x02, 0x99,
                    0xFD, 0x7E, 0x02, 0x76});
  EXPECT_EQ(hi(iy.af), 0x99) << "LD (IY+2),n then LD A,(IY+2)";
  EXPECT_EQ(iy.wz, 0x4002u);
  // FDCB: RES 0,(IY+0).
  Z80Regs fdcb = run({0xFD, 0x21, 0x00, 0x40, 0xFD, 0x36, 0x00, 0xFF,
                      0xFD, 0xCB, 0x00, 0x86, 0xFD, 0x7E, 0x00, 0x76});
  EXPECT_EQ(hi(fdcb.af), 0xFE) << "FDCB RES 0,(IY+0)";
}

TEST(Z80j, BitAndCpUndocumentedSources) {
  // BIT 0,A with A=0x28: Z set (bit 0 clear), X/Y from A → 0x28, H set.
  Z80Regs bit = run({0x3E, 0x28, 0xCB, 0x47, 0x76});
  EXPECT_TRUE(lo(bit.af) & ZF);
  EXPECT_EQ(lo(bit.af) & static_cast<uint8_t>(YF | XF), 0x28) << "BIT r X/Y from the register value";
  EXPECT_TRUE(lo(bit.af) & HF);
  // CP 0x28 with A=0: X/Y come from the OPERAND (0x28), not the result (0xD8, bit5=0).
  Z80Regs cp = run({0x3E, 0x00, 0xFE, 0x28, 0x76});
  EXPECT_NE(lo(cp.af) & YF, 0u) << "CP X/Y from the operand, not the result";
}

TEST(Z80i, Im0VectorsViaBusRst) {
  // IM 0 ; EI ; JR -2.  The CPC bus floats 0xFF → IM0 executes RST 0x38.
  std::vector<uint8_t> prog(0x3B, 0x00);
  prog[0] = 0xED; prog[1] = 0x46;          // IM 0
  prog[2] = 0xFB;                          // EI
  prog[3] = 0x18; prog[4] = 0xFE;          // JR -2
  prog[0x38] = 0x3E; prog[0x39] = 0x42;    // LD A,0x42
  prog[0x3A] = 0x76;                       // HALT
  Z80Regs r = run_int(prog, /*irq=*/true);
  EXPECT_EQ(hi(r.af), 0x42) << "IM0 executed RST 0x38 from the floating-0xFF bus";
}

TEST(Z80i, RetnRestoresIff) {
  // EI (IFF1=IFF2=1) ; HALT.  NMI clears IFF1 (keeps IFF2=1), jumps 0x66 ; RETN
  // copies IFF2 back into IFF1 ; returns to a second HALT.
  std::vector<uint8_t> prog(0x68, 0x00);
  prog[0] = 0xFB;                          // EI
  prog[1] = 0x76;                          // HALT (woken by NMI)
  prog[2] = 0x76;                          // HALT (RETN return target)
  prog[0x66] = 0xED; prog[0x67] = 0x45;    // RETN
  Z80Regs r = run_int(prog, /*irq=*/false, /*nmi_at=*/30);
  EXPECT_EQ(r.iff1, 1) << "RETN restored IFF1 from IFF2 (NMI had cleared it)";
  EXPECT_EQ(r.iff2, 1);
}

TEST(Z80i, InterruptBlockedBetweenPrefixAndOpcode) {
  // A held IRQ must only be accepted at a true instruction boundary, never
  // between a DD prefix and its opcode. IM1 ; EI ; then a tight loop built from a
  // DD-prefixed op so a boundary recurs with index briefly set; the handler runs
  // (proving acceptance still happens) and A is set — the point is it does not
  // hang or mis-accept mid-prefix.
  std::vector<uint8_t> prog(0x3B, 0x00);
  prog[0] = 0xED; prog[1] = 0x56;          // IM 1
  prog[2] = 0xFB;                          // EI
  prog[3] = 0xDD; prog[4] = 0x23;          // INC IX (DD-prefixed)
  prog[5] = 0x18; prog[6] = 0xFB;          // JR -5 → back to the DD INC IX
  prog[0x38] = 0x3E; prog[0x39] = 0x42;    // LD A,0x42
  prog[0x3A] = 0x76;                       // HALT
  Z80Regs r = run_int(prog, /*irq=*/true);
  EXPECT_EQ(hi(r.af), 0x42) << "INT accepted at a clean boundary around the DD-prefixed loop";
}
