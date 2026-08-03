// konCePCja — every keyboard matrix row must be readable through the real
// scan protocol, on every tier.
//
// WHY THIS FILE EXISTS
//
// beads-qgxr: in headless runs, keys on rows 1-15 never reached the firmware
// while row 0 worked — and every link that was inspected in isolation looked
// healthy. This test stops inspecting links: a Z80 program performs the
// firmware's own scan sequence (select AY register 14, then for each row:
// drive the row number + AY READ state onto PPI port C and IN from port A)
// against a distinct injected column pattern per row, under Faithful, Wake
// and Fast. Whichever tier serves the wrong bytes fails here deterministically
// — and if all three pass, the machine is exonerated and the bug is host-side.
//
// The fail-proof for this test is fault injection of the identified broken
// link (the plan's U1 evidence split): it cannot be assumed to fail on any
// given tree, because it asserts the machine half of a chain whose break may
// be host-side.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "../../src/subcycle/machine.h"

namespace {

// One distinct, non-trivial pattern per row; bit 7 spared so the value can't
// be confused with the floating bus and bit patterns differ across rows.
uint8_t row_pattern(uint8_t row) { return static_cast<uint8_t>(0xA5 ^ row); }

constexpr uint16_t kProg = 0x8000;
constexpr uint16_t kDst = 0x9000;  // results: one byte per row, 16 rows

// The firmware's scan protocol, minus the firmware:
//   control = &82  (mode 0, port A output, port C output)
//   port A  = 14   (AY register number)
//   port C  = &C0 then &00   (BDIR+BC1 pulse: latch address 14)
//   control = &92  (port A input)
//   for row 0..15:
//     port C = &40 | row     (BC1=1, BDIR=0: AY read state + row select)
//     IN from port A -> (kDst + row)
//   park.
constexpr uint8_t kProgram[] = {
    0xF3,              // di
    0x01, 0x00, 0xF7,  // ld bc,&F700   ; PPI control
    0x3E, 0x82,        // ld a,&82      ; A out, C out
    0xED, 0x79,        // out (c),a
    0x01, 0x00, 0xF4,  // ld bc,&F400   ; port A
    0x3E, 0x0E,        // ld a,14
    0xED, 0x79,        // out (c),a
    0x01, 0x00, 0xF6,  // ld bc,&F600   ; port C
    0x3E, 0xC0,        // ld a,&C0      ; latch AY register address
    0xED, 0x79,        // out (c),a
    0x3E, 0x00,        // ld a,&00      ; AY inactive
    0xED, 0x79,        // out (c),a
    0x01, 0x00, 0xF7,  // ld bc,&F700
    0x3E, 0x92,        // ld a,&92      ; port A input
    0xED, 0x79,        // out (c),a
    // row loop: E = row, HL = dst
    0x1E, 0x00,        // ld e,0
    0x21, 0x00, 0x90,  // ld hl,&9000
    // loop:
    0x7B,              // ld a,e        ; row
    0xF6, 0x40,        // or &40        ; AY read state
    0x01, 0x00, 0xF6,  // ld bc,&F600
    0xED, 0x79,        // out (c),a     ; select row, AY read
    0x01, 0x00, 0xF4,  // ld bc,&F400
    0xED, 0x78,        // in a,(c)      ; read columns
    0x77,              // ld (hl),a
    0x23,              // inc hl
    0x1C,              // inc e
    0x7B,              // ld a,e
    0xFE, 0x10,        // cp 16
    0x20, 0xEB,        // jr nz,loop    ; -21, back to ld a,e
    0x18, 0xFE,        // jr $
};

void run_scan(subcycle::Machine& m) {
  for (size_t i = 0; i < sizeof(kProgram); i++)
    m.poke_mem(static_cast<uint16_t>(kProg + i), kProgram[i]);
  Z80Regs regs = m.regs();
  regs.pc = kProg;
  m.set_regs(regs);
  m.run_frame();  // warm-up frame retires nothing
  m.run_frame();
}

void expect_all_rows(subcycle::Machine& m, const char* tier) {
  for (uint8_t row = 0; row < 16; row++) {
    EXPECT_EQ(m.peek_mem(static_cast<uint16_t>(kDst + row)), row_pattern(row))
        << tier << ": row " << static_cast<int>(row)
        << " served the wrong columns — a stale row select serves row 0's "
           "byte for every row (the beads-qgxr asymmetry)";
  }
}

void boot(subcycle::Machine& m, std::vector<uint8_t>& rom,
          subcycle::Machine::RunTier tier) {
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  m.set_run_tier(tier);
  for (uint8_t row = 0; row < 16; row++) m.set_key_row(row, row_pattern(row));
}

}  // namespace

TEST(KeyboardRowSelect, AllSixteenRowsReadBackFaithful) {
  std::vector<uint8_t> rom(0x8000, 0x00);
  subcycle::Machine m;
  boot(m, rom, subcycle::Machine::RunTier::Faithful);
  run_scan(m);
  expect_all_rows(m, "faithful");
}

TEST(KeyboardRowSelect, AllSixteenRowsReadBackWake) {
  std::vector<uint8_t> rom(0x8000, 0x00);
  subcycle::Machine m;
  boot(m, rom, subcycle::Machine::RunTier::Wake);
  run_scan(m);
  expect_all_rows(m, "wake");
}

TEST(KeyboardRowSelect, AllSixteenRowsReadBackFast) {
  std::vector<uint8_t> rom(0x8000, 0x00);
  subcycle::Machine m;
  boot(m, rom, subcycle::Machine::RunTier::Fast);
  run_scan(m);
  expect_all_rows(m, "fast");
}
