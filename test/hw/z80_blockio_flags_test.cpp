/* z80_blockio_flags_test.cpp — undocumented flags of the Z80 block I/O group
 * (INI/IND/INIR/INDR/OUTI/OUTD/OTIR/OTDR), including the mid-loop corrections
 * of the repeating variants.
 *
 * Two rule sets are locked here:
 *
 * 1. The documented-undocumented base rules (Sean Young, "The Undocumented Z80
 *    Documented" §4.3): after any block I/O op, NF = bit 7 of the transferred
 *    byte; k = byte + ((C ± 1) & 0xFF) for INI/IND (+1/-1), k = byte + L (after
 *    the HL adjust) for OUTI/OUTD; HF = CF = (k > 255); PF = parity((k & 7) ^
 * B) with B post-decrement; S/Z/YF/XF from the decremented B.
 *
 * 2. The repeat-branch corrections (David Banks, "Undocumented Z80 Flags"
 *    rev. 1.0, 2018-08-21 — github.com/hoglet67/Z80Decoder/wiki/Undocumented-
 *    Flags): on EVERY non-terminating INIR/INDR/OTIR/OTDR iteration the
 *    architectural F is
 *      SF = B.7, ZF = 0, YF = PC.13, XF = PC.11 (PC = the rewound address),
 *      NF = byte.7, and with hcf = (k > 255), p = (k & 7) ^ B:
 *        hcf=1, NF=1: CF=1, HF = (B & 0xF) == 0x0, PF = parity(p ^ ((B-1) & 7))
 *        hcf=1, NF=0: CF=1, HF = (B & 0xF) == 0xF, PF = parity(p ^ ((B+1) & 7))
 *        hcf=0:       CF=0, HF = 0,                PF = parity(p ^ (B & 7))
 *    and MEMPTR = PC+1 (not BC±1). This is committed directly, not deferred.
 *
 * RECONCILED (beads-yjql): the corrected F is committed on every non-final
 * iteration — NOT only when an interrupt cuts the loop, as this file previously
 * assumed. The SingleStepTests (jsmoo) corpus pins these exact values 1:1
 * (z80_singlestep_test.cpp), and five stale FUSE mid-loop expecteds were
 * realigned to match (z80_fuse_test.cpp). This file now uniquely covers the
 * INTERRUPT-cut scenario SST can't reach: an INT/NMI mid-repeat pushes the
 * rewound PC and the ISR runs with the corrected F.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "hw/board.h"
#include "hw/z80.h"

namespace {

constexpr uint8_t SF = 0x80, ZF = 0x40, YF = 0x20, HF = 0x10, XF = 0x08,
                  PF = 0x04, NF = 0x02, CF = 0x01;

/* RAM device (two-phase), as in z80_test.cpp. */
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
  return Device{&dummy,   "clk",    clk_tick, clk_reset,
                clk_size, clk_save, clk_load};
}

/* Fixed-value I/O device: every IN returns `in_val` (the vectors below need a
 * specific transferred byte, decoupled from the port); every OUT is recorded.
 */
struct FixedIo {
  uint8_t in_val;
  uint16_t last_out_port;
  uint8_t last_out_val;
  bool out_seen;
};
void fio_tick(void* self, const Bus* in, Bus* out) {
  FixedIo* io = static_cast<FixedIo*>(self);
  if (in->cpu.iorq && in->cpu.rd) {
    out->cpu.data = io->in_val;
  } else if (in->cpu.iorq && in->cpu.wr) {
    io->last_out_port = in->cpu.addr;
    io->last_out_val = in->cpu.data;
    io->out_seen = true;
  }
}
void fio_reset(void*) {}
size_t fio_size(const void*) { return sizeof(FixedIo); }
void fio_save(const void* s, void* b) { std::memcpy(b, s, sizeof(FixedIo)); }
void fio_load(void* s, const void* b) { std::memcpy(s, b, sizeof(FixedIo)); }
Device fio_device(FixedIo* s) {
  return Device{s, "io", fio_tick, fio_reset, fio_size, fio_save, fio_load};
}

/* Interrupt-line driver: IRQ level from board tick `irq_at`, NMI rising edge at
 * board tick `nmi_at`; <0 = never. */
struct IntLine {
  int irq_at;
  int nmi_at;
  int tick;
};
void il_tick(void* self, const Bus*, Bus* out) {
  IntLine* s = static_cast<IntLine*>(self);
  if (s->irq_at >= 0 && s->tick >= s->irq_at) out->cpu.irq = true;
  if (s->nmi_at >= 0 && s->tick >= s->nmi_at) out->cpu.nmi = true;
  s->tick++;
}
void il_reset(void* self) { static_cast<IntLine*>(self)->tick = 0; }
size_t il_size(const void*) { return sizeof(IntLine); }
void il_save(const void* s, void* b) { std::memcpy(b, s, sizeof(IntLine)); }
void il_load(void* s, const void* b) { std::memcpy(s, b, sizeof(IntLine)); }
Device il_device(IntLine* s) {
  return Device{s, "int", il_tick, il_reset, il_size, il_save, il_load};
}

/* Rig: poke an initial state, then T-step to exact targets and peek. */
struct Rig {
  std::unique_ptr<Ram> ram = std::make_unique<Ram>();
  FixedIo io{0, 0, 0, false};
  IntLine il{-1, -1, 0};
  std::vector<uint8_t> z80mem = std::vector<uint8_t>(z80_state_size());
  Board board;
  Device zdev;

  Rig(const Z80Regs& init, std::initializer_list<uint8_t> code, uint8_t io_in,
      int irq_at = -1, int nmi_at = -1) {
    std::memset(ram->cells, 0, sizeof(Ram::cells));
    size_t a = init.pc;
    for (uint8_t b : code) ram->cells[static_cast<uint16_t>(a++)] = b;
    io.in_val = io_in;
    il = IntLine{irq_at, nmi_at, 0};
    zdev = z80_init(z80mem.data());
    board_init(&board);
    board_add(&board, clock_device());
    board_add(&board, ram_device(ram.get()));
    board_add(&board, fio_device(&io));
    board_add(&board, il_device(&il));
    board_add(&board, zdev);
    board_reset(&board);
    z80_poke(&zdev, &init);  // after reset: poke wins
  }

  /* Tick until exactly `target` T-states have executed (always-on clock: one
   * T-state per board tick, so the stop is exact). */
  Z80Regs run_to(uint64_t target) {
    Z80Regs r{};
    for (int guard = 0; guard < 4000; ++guard) {
      z80_peek(&zdev, &r);
      if (r.tstates >= target) break;
      board_tick(&board);
    }
    EXPECT_EQ(r.tstates, target) << "T-state target overshoot/undershoot";
    return r;
  }
};

uint8_t lo(uint16_t v) { return static_cast<uint8_t>(v & 0xFF); }
uint8_t hi(uint16_t v) { return static_cast<uint8_t>(v >> 8); }

Z80Regs regs(uint16_t af, uint16_t bc, uint16_t hl, uint16_t pc,
             uint16_t sp = 0x8000, uint8_t iff = 0, uint8_t im = 1) {
  Z80Regs r{};
  r.af = af;
  r.bc = bc;
  r.hl = hl;
  r.pc = pc;
  r.sp = sp;
  r.iff1 = r.iff2 = iff;
  r.im = im;
  return r;
}

/* --- (a) Single (non-repeating) INI / OUTI: full Young §4.3 flag rules. --- */

// INI: B=0x69 C=0x07, byte 0x93 in. B-- → 0x68 → S=0 Z=0 YF=1 XF=1 (0x28).
// byte.7=1 → NF. k = 0x93 + ((0x07+1) & 0xFF) = 0x9B ≤ 255 → HF=CF=0.
// PF = parity((0x9B & 7) ^ 0x68) = parity(0x6B) = odd → 0.  F = 0x2A.
TEST(Z80BlockIoFlags, IniDocumentedFlags) {
  Rig rig(regs(0x1200, 0x6907, 0x4000, 0x0000), {0xED, 0xA2}, /*io_in=*/0x93);
  Z80Regs r = rig.run_to(16);
  EXPECT_EQ(lo(r.af), YF | XF | NF) << "F=0x2A";
  EXPECT_EQ(hi(r.af), 0x12) << "A untouched";
  EXPECT_EQ(r.bc, 0x6807) << "B decremented, C kept";
  EXPECT_EQ(r.hl, 0x4001);
  EXPECT_EQ(rig.ram->cells[0x4000], 0x93) << "byte stored at (HL)";
  EXPECT_EQ(r.wz, 0x6908) << "MEMPTR = BC(original)+1";
  EXPECT_EQ(r.pc, 0x0002);
}

// OUTI: B=0x02 C=0x10, (HL)=0xFF at 0x2000. B-- → 0x01 → SZ53 = 0.
// byte.7=1 → NF. HL→0x2001, k = 0xFF + L(0x01) = 0x100 > 255 → HF|CF.
// PF = parity((0x100 & 7) ^ 0x01) = parity(1) = odd → 0.  F = 0x13.
TEST(Z80BlockIoFlags, OutiDocumentedFlags) {
  Rig rig(regs(0x0000, 0x0210, 0x2000, 0x0000), {0xED, 0xA3}, 0x00);
  rig.ram->cells[0x2000] = 0xFF;
  Z80Regs r = rig.run_to(16);
  EXPECT_EQ(lo(r.af), HF | NF | CF) << "F=0x13";
  EXPECT_EQ(r.bc, 0x0110);
  EXPECT_EQ(r.hl, 0x2001);
  EXPECT_TRUE(rig.io.out_seen);
  EXPECT_EQ(rig.io.last_out_port, 0x0110) << "OUT uses the decremented B";
  EXPECT_EQ(rig.io.last_out_val, 0xFF);
  EXPECT_EQ(r.wz, 0x0111) << "MEMPTR = BC(after B--)+1";
  EXPECT_EQ(r.pc, 0x0002);
}

/* --- (b) Repeating variants stopped mid-loop, NO interrupt: the corrected F is
 *     committed directly (Banks/SST). Vectors are the FUSE mid-loop cases, now
 *     realigned from the pre-2018 base flags to the Banks values. --- */

// INIR (was FUSE edb2_1, realigned): AF=8a34 BC=0a40 HL=37ce, IN 0x0a, one 21T
// iteration at PC=0 → corrected F=0x00 (PCH=0 → YF/XF clear, no-carry branch),
// B--, MEMPTR = PC+1.
TEST(Z80BlockIoFlags, InirMidLoopCommitsCorrectedFlags) {
  Z80Regs init = regs(0x8a34, 0x0a40, 0x37ce, 0x0000);
  init.de = 0xd98c;
  Rig rig(init, {0xED, 0xB2}, /*io_in=*/0x0a);
  Z80Regs r = rig.run_to(21);
  EXPECT_EQ(r.af, 0x8a00) << "mid-loop F = Banks correction (YF/XF from PCH=0)";
  EXPECT_EQ(r.bc, 0x0940);
  EXPECT_EQ(r.hl, 0x37cf);
  EXPECT_EQ(r.pc, 0x0000) << "PC rewound to the instruction";
  EXPECT_EQ(r.wz, 0x0001) << "MEMPTR = PC+1, not BC±1";
}

// OTIR (was FUSE edb3_1, realigned): AF=34ab BC=03e0 HL=1d7c, (HL)=0x9d, one 21T
// iteration at PC=0 → corrected F=0x03 (PCH=0 clears YF/XF; the carry rule sets
// HF/PF differently from the base formula), MEMPTR = PC+1.
TEST(Z80BlockIoFlags, OtirMidLoopCommitsCorrectedFlags) {
  Rig rig(regs(0x34ab, 0x03e0, 0x1d7c, 0x0000), {0xED, 0xB3}, 0x00);
  rig.ram->cells[0x1d7c] = 0x9d;
  Z80Regs r = rig.run_to(21);
  EXPECT_EQ(r.af, 0x3403);
  EXPECT_EQ(r.bc, 0x02e0);
  EXPECT_EQ(r.hl, 0x1d7d);
  EXPECT_EQ(r.pc, 0x0000);
  EXPECT_EQ(r.wz, 0x0001);
}

// INDR (was FUSE edba_1, realigned): AF=2567 BC=069f HL=6b55, IN 0x06, one 21T
// iteration at PC=0 → corrected F=0x00, MEMPTR = PC+1 (only WZ moved vs base).
TEST(Z80BlockIoFlags, IndrMidLoopCommitsCorrectedFlags) {
  Z80Regs init = regs(0x2567, 0x069f, 0x6b55, 0x0000);
  init.de = 0xd40d;
  Rig rig(init, {0xED, 0xBA}, /*io_in=*/0x06);
  Z80Regs r = rig.run_to(21);
  EXPECT_EQ(r.af, 0x2500);
  EXPECT_EQ(r.bc, 0x059f);
  EXPECT_EQ(r.hl, 0x6b54);
  EXPECT_EQ(r.pc, 0x0000);
  EXPECT_EQ(r.wz, 0x0001) << "MEMPTR = PC+1";
}

/* --- (b') Repeating variants CUT BY AN INTERRUPT mid-loop: Banks corrections.
 *     Hand-computed vectors, one per correction branch. --- */

// INIR at 0x2800 (PCH=0x28 → YF=1 XF=1), B=0x10 C=0x33, byte 0xD0 in.
// bdec=0x0F, k = 0xD0+0x34 = 0x104 > 255 (hcf), byte.7=1 (NF).
// Committed at T=21 AND seen in the ISR (Banks): SF=0, YF|XF from PCH=0x28 →
//   0x28; NF; CF; HF = ((0x0F & 0xF) == 0) = 0; PF = parity(0x0B ^ ((0x0F-1)&7))
//   = parity(0x0B ^ 6 = 0x0D) = odd → 0.                  = 0x2B.
TEST(Z80BlockIoFlags, InirInterruptedCarryN1Branch) {
  Rig rig(regs(0x0000, 0x1033, 0x4000, 0x2800, 0x8000, /*iff=*/1, /*im=*/1),
          {0xED, 0xB2}, /*io_in=*/0xD0, /*irq_at=*/5);
  Z80Regs mid = rig.run_to(21);
  EXPECT_EQ(lo(mid.af), YF | XF | NF | CF)
      << "corrected F committed at the iteration (= what the ISR sees)";
  EXPECT_EQ(mid.pc, 0x2800);
  Z80Regs r = rig.run_to(21 + 13);  // + IM1 acceptance
  EXPECT_EQ(r.pc, 0x0038) << "IM1 vector taken";
  EXPECT_EQ(lo(r.af), YF | XF | NF | CF)
      << "ISR sees the Banks-corrected F=0x2B";
  EXPECT_EQ(r.bc, 0x0F33);
  EXPECT_EQ(r.hl, 0x4001);
  EXPECT_EQ(r.sp, 0x7FFE);
  EXPECT_EQ(rig.ram->cells[0x7FFE], 0x00);
  EXPECT_EQ(rig.ram->cells[0x7FFF], 0x28) << "rewound PC (0x2800) pushed";
}

// INIR at 0x0000 (PCH=0 → YF=XF=0), B=0x08 C=0x10, byte 0x01, cut by NMI.
// bdec=0x07, k = 0x01+0x11 = 0x12 ≤ 255 (no hcf), byte.7=0.
// Committed at T=21 AND seen by the NMI handler (Banks): PF = parity(p ^ (B&7))
//   = parity(5 ^ 7 = 2) = odd → 0; HF=CF=0.               = 0x00.
TEST(Z80BlockIoFlags, InirNmiInterruptedNoCarryBranch) {
  Rig rig(regs(0x0000, 0x0810, 0x6000, 0x0000), {0xED, 0xB2}, /*io_in=*/0x01,
          /*irq_at=*/-1, /*nmi_at=*/5);
  Z80Regs mid = rig.run_to(21);
  EXPECT_EQ(lo(mid.af), 0x00) << "corrected F committed at the iteration";
  Z80Regs r = rig.run_to(21 + 11);  // + NMI acceptance
  EXPECT_EQ(r.pc, 0x0066) << "NMI vector taken";
  EXPECT_EQ(lo(r.af), 0x00) << "corrected F: no-carry branch flips PF";
  EXPECT_EQ(r.iff1, 0);
}

// OTIR at 0x0000, B=0x10 C=0x22, (HL=0x4081)=0x7F. HL→0x4082,
// bdec=0x0F, k = 0x7F+0x82 = 0x101 > 255 (hcf), byte.7=0.
// Committed at T=21 AND seen in the ISR (Banks): PCH=0 → YF=XF=0;
//   HF = ((0x0F & 0xF) == 0xF) = 1; CF; PF = parity(0x0E ^ 0) = odd → 0. = 0x11.
TEST(Z80BlockIoFlags, OtirInterruptedCarryN0Branch) {
  Rig rig(regs(0x0000, 0x1022, 0x4081, 0x0000, 0x8000, 1, 1), {0xED, 0xB3},
          0x00, /*irq_at=*/5);
  rig.ram->cells[0x4081] = 0x7F;
  Z80Regs mid = rig.run_to(21);
  EXPECT_EQ(lo(mid.af), HF | CF) << "corrected F committed at the iteration";
  Z80Regs r = rig.run_to(21 + 13);
  EXPECT_EQ(r.pc, 0x0038);
  EXPECT_EQ(lo(r.af), HF | CF) << "ISR sees F=0x11: half-borrow HF, XF gone";
  EXPECT_EQ(r.bc, 0x0F22);
  EXPECT_EQ(r.hl, 0x4082);
  EXPECT_EQ(rig.io.last_out_port, 0x0F22);
  EXPECT_EQ(rig.io.last_out_val, 0x7F);
}

// INDR at 0x0800 (PCH=0x08 → YF=0 XF=1), B=0x21 C=0x00, byte 0x85 in.
// bdec=0x20, k = 0x85 + ((0x00-1)&0xFF = 0xFF) = 0x184 > 255 (hcf), byte.7=1.
// Committed at T=21 AND seen in the ISR (Banks): SF=0; PCH=0x08 → XF only; NF;
//   CF; HF = ((0x20 & 0xF) == 0) = 1; PF = parity(0x24 ^ 7 = 0x23) = odd → 0.
//                                                                  = 0x1B.
TEST(Z80BlockIoFlags, IndrInterruptedCarryN1Branch) {
  Rig rig(regs(0x0000, 0x2100, 0x5000, 0x0800, 0x8000, 1, 1), {0xED, 0xBA},
          /*io_in=*/0x85, /*irq_at=*/5);
  Z80Regs mid = rig.run_to(21);
  EXPECT_EQ(lo(mid.af), XF | NF | HF | CF)
      << "corrected F committed at the iteration";
  EXPECT_EQ(mid.pc, 0x0800);
  Z80Regs r = rig.run_to(21 + 13);
  EXPECT_EQ(r.pc, 0x0038);
  EXPECT_EQ(lo(r.af), XF | NF | HF | CF) << "ISR sees the corrected F=0x1B";
  EXPECT_EQ(r.bc, 0x2000);
  EXPECT_EQ(r.hl, 0x4FFF);
  EXPECT_EQ(rig.ram->cells[0x7FFF], 0x08) << "rewound PC (0x0800) pushed";
}

/* --- (c) Final iteration: base flags stand, and NO stale correction leaks
 *     into a later interrupt. --- */

// INIR B=2 C=0x40, byte 0x0A: iteration 1 repeats (21T, stashes a correction),
// iteration 2 finishes (16T, B=0 → ZF; PF = parity(3 ^ 0) = even → PF; F=0x44).
// IRQ is raised only AFTER the loop ends, so acceptance (after the following
// 4T NOP) must show the final base F — a stale iteration-1 correction (0x04)
// leaking through would be caught here.
TEST(Z80BlockIoFlags, FinalIterationThenInterruptKeepsBaseFlags) {
  Rig rig(regs(0x0000, 0x0240, 0x9000, 0x0000, 0x8000, 1, 1),
          {0xED, 0xB2, 0x00},
          /*io_in=*/0x0a, /*irq_at=*/39);
  Z80Regs done = rig.run_to(21 + 16);
  EXPECT_EQ(lo(done.af), ZF | PF) << "final iteration: base rules, B=0 → ZF";
  EXPECT_EQ(done.pc, 0x0002);
  EXPECT_EQ(hi(done.bc), 0x00);
  Z80Regs r = rig.run_to(21 + 16 + 4 + 13);  // NOP + IM1 acceptance
  EXPECT_EQ(r.pc, 0x0038);
  EXPECT_EQ(lo(r.af), ZF | PF) << "no stale mid-loop correction applied";
}

}  // namespace
