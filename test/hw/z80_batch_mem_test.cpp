/* z80_batch_mem_test.cpp — F2 meets F3 (beads-f0bq): the smallest possible
 * RunTier::Fast machine — z80_batch_step driving the memory device's batch
 * seam (mem_fast_read/write, OUTs routed to mem_fast_io_write) — runs a
 * banking-exercise program in LOCKSTEP against the per-cycle reference (the
 * same Z80 + Gate Array + memory Devices ticked on the two-phase bus).
 *
 * Equality required at HALT: full Z80 architectural state INCLUDING the
 * CPC-grid T-total, the memory device's latches (MemRegs), the whole base
 * 64K, and the whole expansion RAM. This is the template the F4 scheduler
 * grows from: the batch callbacks here are exactly what the machine's
 * Z80BatchIO will do.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hw/board.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/z80.h"

namespace {

constexpr uint16_t kProg = 0x8000;  // slot 2: stable across the configs used

// The program: hop through PAL configs writing a distinct marker at &4000
// each time, park one write at &C000 under config 1, then flip the ROM
// enables and the upper-ROM select while sampling reads into &9000-&9004.
// Config 2 is deliberately absent — it would unmap the program's own slot
// (the twin-device unit test covers its mapping instead).
const uint8_t kProgram[] = {
    0x01, 0xC4, 0x7F,  // LD BC,&7FC4
    0xED, 0x49,        // OUT (C),C      ; cfg 4: slot1 -> expansion page 0
    0x21, 0x00, 0x40,  // LD HL,&4000
    0x36, 0x11,        // LD (HL),&11
    0x01, 0xC5, 0x7F,  // LD BC,&7FC5    ; cfg 5: slot1 -> expansion page 1
    0xED, 0x49,        // OUT (C),C
    0x36, 0x22,        // LD (HL),&22
    0x01, 0xC3, 0x7F,  // LD BC,&7FC3    ; cfg 3: slot1 -> base page 3
    0xED, 0x49,        // OUT (C),C
    0x36, 0x33,        // LD (HL),&33
    0x01, 0xC1, 0x7F,  // LD BC,&7FC1    ; cfg 1: slot3 -> expansion page 3
    0xED, 0x49,        // OUT (C),C
    0x3E, 0x44,        // LD A,&44
    0x32, 0x00, 0xC0,  // LD (&C000),A   ; write-through under the upper ROM
    0x01, 0xC0, 0x7F,  // LD BC,&7FC0    ; cfg 0: all base
    0xED, 0x49,        // OUT (C),C
    0x36, 0x55,        // LD (HL),&55
    0x3A, 0x00, 0x00,  // LD A,(&0000)   ; lower-ROM byte
    0x32, 0x00, 0x90,  // LD (&9000),A
    0x01, 0x8C, 0x7F,  // LD BC,&7F8C    ; fn2: both ROMs OFF
    0xED, 0x49,        // OUT (C),C
    0x3A, 0x00, 0x00,  // LD A,(&0000)   ; now RAM
    0x32, 0x01, 0x90,  // LD (&9001),A
    0x3A, 0x00, 0xC0,  // LD A,(&C000)   ; upper RAM (base page 3)
    0x32, 0x02, 0x90,  // LD (&9002),A
    0x01, 0x80, 0x7F,  // LD BC,&7F80    ; ROMs back ON
    0xED, 0x49,        // OUT (C),C
    0x3A, 0x00, 0xC0,  // LD A,(&C000)   ; upper-ROM byte (BASIC marker)
    0x32, 0x03, 0x90,  // LD (&9003),A
    0x01, 0x07, 0xDF,  // LD BC,&DF07    ; upper-ROM select 7
    0xED, 0x49,        // OUT (C),C
    0x3A, 0x00, 0xC0,  // LD A,(&C000)   ; the ROM-7 marker
    0x32, 0x04, 0x90,  // LD (&9004),A
    0x76,              // HALT
};

// One memory device with distinguishable content everywhere.
struct MemSide {
  std::vector<uint8_t> storage = std::vector<uint8_t>(mem_state_size());
  std::vector<uint8_t> expansion = std::vector<uint8_t>(128 * 1024);
  std::vector<uint8_t> rom7 = std::vector<uint8_t>(0x4000, 0x70);
  Device dev{};
};

void seed_side(MemSide& side) {
  side.dev = mem_init(side.storage.data());
  std::vector<uint8_t> lo(0x4000, 0x01), hi(0x4000, 0x02);
  mem_load_lower_rom(&side.dev, lo.data(), lo.size());
  mem_load_upper_rom(&side.dev, hi.data(), hi.size());
  mem_attach_rom(&side.dev, 7, side.rom7.data());
  mem_attach_expansion(&side.dev, side.expansion.data(),
                       side.expansion.size());
  for (uint32_t a = 0; a < 0x10000; ++a)
    mem_write_ram(&side.dev, static_cast<uint16_t>(a),
                  static_cast<uint8_t>(0xA0 + (a >> 14)));
  for (size_t i = 0; i < side.expansion.size(); ++i)
    side.expansion[i] = static_cast<uint8_t>(0xE0 + (i >> 14));
  for (size_t i = 0; i < sizeof(kProgram); ++i)
    mem_write_ram(&side.dev, static_cast<uint16_t>(kProg + i), kProgram[i]);
}

Z80Regs start_regs() {
  Z80Regs regs{};
  regs.pc = kProg;
  regs.sp = 0xBF00;
  return regs;
}

// Per-cycle reference: Z80 + GA + memory on the two-phase bus.
Z80Regs run_percycle(MemSide& side) {
  seed_side(side);
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, side.dev);
  board_add(&board, zdev);
  board_reset(&board);  // resets latches only; the seeded contents persist
  const Z80Regs init = start_regs();
  z80_poke(&zdev, &init);

  Z80Regs r{};
  for (int tick = 0; tick < 400000; ++tick) {
    board_tick(&board);
    z80_peek(&zdev, &r);
    if (r.halted) return r;
  }
  r.halted = 0;
  return r;
}

// Fast side: the batch driver over the memory seam — no board, no GA.
uint8_t fmem_read(void* ctx, uint16_t addr, uint64_t) {
  return mem_fast_read(static_cast<const Device*>(ctx), addr);
}
void fmem_write(void* ctx, uint16_t addr, uint8_t val, uint64_t) {
  mem_fast_write(static_cast<const Device*>(ctx), addr, val);
}
uint8_t fio_read(void*, uint16_t, uint64_t) { return 0xFF; }
void fio_write(void* ctx, uint16_t port, uint8_t val, uint64_t) {
  mem_fast_io_write(static_cast<const Device*>(ctx), port, val);
}

Z80Regs run_batch(MemSide& side) {
  seed_side(side);
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  const Z80Regs init = start_regs();
  z80_poke(&zdev, &init);

  const Z80BatchIO bio{&side.dev, fmem_read, fmem_write, fio_read, fio_write};
  Z80Regs r{};
  for (int steps = 0; steps < 100000; ++steps) {
    z80_batch_step(&zdev, &bio, /*irq=*/0, /*vector=*/0xFF, /*grid=*/1);
    z80_peek(&zdev, &r);
    if (r.halted) return r;
  }
  r.halted = 0;
  return r;
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
  cmp("R", a.r, b.r);
  cmp("T", a.tstates, b.tstates);
  cmp("IC", a.instr_count, b.instr_count);
  return d;
}

}  // namespace

TEST(Z80BatchMem, BankingProgramLockstep) {
  MemSide pc_side, b_side;
  const Z80Regs regs_pc = run_percycle(pc_side);
  ASSERT_TRUE(regs_pc.halted) << "per-cycle reference never reached HALT";
  const Z80Regs regs_b = run_batch(b_side);
  ASSERT_TRUE(regs_b.halted) << "batch machine never reached HALT";

  // Z80 state including the CPC-grid T-total.
  EXPECT_EQ(diff_regs(regs_b, regs_pc), "");

  // Memory-device latches.
  MemRegs mem_pc{}, mem_b{};
  mem_peek(&pc_side.dev, &mem_pc);
  mem_peek(&b_side.dev, &mem_b);
  EXPECT_EQ(mem_b.rom_config, mem_pc.rom_config);
  EXPECT_EQ(mem_b.ram_config, mem_pc.ram_config);
  EXPECT_EQ(mem_b.ram_ext, mem_pc.ram_ext);
  EXPECT_EQ(mem_b.rom_select, mem_pc.rom_select);

  // The whole store: base 64K + expansion, byte for byte.
  for (uint32_t a = 0; a < 0x10000; ++a) {
    ASSERT_EQ(mem_read_ram(&b_side.dev, static_cast<uint16_t>(a)),
              mem_read_ram(&pc_side.dev, static_cast<uint16_t>(a)))
        << "base RAM diverged at " << a;
  }
  ASSERT_EQ(b_side.expansion, pc_side.expansion) << "expansion RAM diverged";

  // Sanity anchors so a doubly-broken pair cannot vacuously agree: the
  // sampled reads really saw ROM / RAM / ROM-7, and the banked writes landed.
  EXPECT_EQ(mem_read_ram(&b_side.dev, 0x9000), 0x01);  // lower ROM
  EXPECT_EQ(mem_read_ram(&b_side.dev, 0x9003), 0x02);  // upper ROM (BASIC)
  EXPECT_EQ(mem_read_ram(&b_side.dev, 0x9004), 0x70);  // ROM 7
  EXPECT_EQ(mem_read_ram(&b_side.dev, 0x4000), 0x55);  // cfg-0 write
  EXPECT_EQ(b_side.expansion[0x0000], 0x11);           // cfg-4 slot1 write
  EXPECT_EQ(b_side.expansion[0x4000], 0x22);           // cfg-5 slot1 write
  EXPECT_EQ(mem_read_ram(&b_side.dev, 0xC000), 0x33);  // cfg-3 aliased write
  EXPECT_EQ(b_side.expansion[0xC000], 0x44);           // cfg-1 slot3 write
}
