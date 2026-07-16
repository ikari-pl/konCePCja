/* smartwatch_test.cpp — the DS1216 phantom RTC Device: the 64-bit
 * recognition pattern on A0/A2, the D0 override under /ROMDIS with the
 * ROM's bits 7..1 passing through, the upper-ROM-enable gate, and the
 * abort/reset paths. See docs/hardware/smartwatch-device.md. */

#include "hw/smartwatch.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/board.h"
#include "hw/memory.h"

namespace {

struct SwRig {
  std::vector<uint8_t> mmem = std::vector<uint8_t>(mem_state_size());
  std::vector<uint8_t> smem = std::vector<uint8_t>(smartwatch_state_size());
  std::vector<uint8_t> rom = std::vector<uint8_t>(0x4000, 0xAA);  // D0 = 0
  Board board;
  Device mem;
  Device sw;
};

void make_rig(SwRig& rig) {
  rig.mem = mem_init(rig.mmem.data());
  rig.sw = smartwatch_init(rig.smem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.mem);
  board_add(&rig.board, rig.sw);
  board_reset(&rig.board);
  mem_load_upper_rom(&rig.mem, rig.rom.data(), rig.rom.size());
}

// A held memory read (6 ticks, sample at the end — the T3 discipline).
uint8_t rom_read(SwRig& rig, uint16_t addr) {
  for (int i = 0; i < 6; ++i) {
    Bus b = rig.board.bus;  // device outputs persist across the access
    b.cpu.m1 = b.cpu.iorq = b.cpu.wr = false;
    b.cpu.mreq = b.cpu.rd = true;
    b.cpu.addr = addr;
    rig.board.bus = b;
    board_tick(&rig.board);
  }
  const uint8_t val = rig.board.bus.cpu.data;
  for (int i = 0; i < 2; ++i) {  // release the bus between accesses
    Bus b = rig.board.bus;
    b.cpu.mreq = b.cpu.rd = false;
    rig.board.bus = b;
    board_tick(&rig.board);
  }
  return val;
}

// Send the DS1216 recognition pattern: 64 reads, data on A0, A2 low.
void send_pattern(SwRig& rig) {
  const uint8_t pat[8] = {0xC5, 0x3A, 0xA3, 0x5C, 0xC5, 0x3A, 0xA3, 0x5C};
  for (int i = 0; i < 64; ++i) {
    const uint8_t bit = (pat[i / 8] >> (i % 8)) & 1;
    rom_read(rig, static_cast<uint16_t>(0xC000 | bit));  // A2 = 0
  }
}

// Read the 64 time bits back (A2 = 1) and assemble the 8 BCD bytes.
void read_time(SwRig& rig, uint8_t out[8]) {
  for (int i = 0; i < 8; ++i) out[i] = 0;
  for (int i = 0; i < 64; ++i) {
    const uint8_t val = rom_read(rig, 0xC004);  // A2 = 1
    out[i / 8] |= static_cast<uint8_t>((val & 1) << (i % 8));
    EXPECT_EQ(val & 0xFE, 0xAA & 0xFE)
        << "bits 7..1 pass through from the ROM chip (bit " << i << ")";
  }
}

}  // namespace

TEST(Smartwatch, PatternMatchDeliversTheTimeOnD0) {
  SwRig rig;
  make_rig(rig);
  smartwatch_set_plugged(&rig.sw, 1);
  const uint8_t t8[8] = {0x00, 0x34, 0x12, 0x80 | 0x23,
                         0x05, 0x28, 0x06, 0x26};  // 23:12:34 Fri 28/06/26
  smartwatch_set_time(&rig.sw, t8);

  EXPECT_EQ(rom_read(rig, 0xC004), 0xAA) << "idle A2=1 reads pass through";
  send_pattern(rig);
  SmartwatchRegs r{};
  smartwatch_peek(&rig.sw, &r);
  EXPECT_EQ(r.state, 2) << "pattern matched: reading";

  uint8_t got[8];
  read_time(rig, got);
  for (int i = 0; i < 8; ++i) EXPECT_EQ(got[i], t8[i]) << "byte " << i;
  smartwatch_peek(&rig.sw, &r);
  EXPECT_EQ(r.state, 0) << "64 bits delivered: back to idle";
  EXPECT_EQ(rom_read(rig, 0xC004), 0xAA) << "and the ROM answers again";
}

TEST(Smartwatch, WrongPatternAndAbortsStayInvisible) {
  SwRig rig;
  make_rig(rig);
  smartwatch_set_plugged(&rig.sw, 1);

  for (int i = 0; i < 64; ++i) rom_read(rig, 0xC000);  // all-zero "pattern"
  SmartwatchRegs r{};
  smartwatch_peek(&rig.sw, &r);
  EXPECT_EQ(r.state, 0) << "no match: idle";

  // A2=1 mid-match resets the FSM.
  rom_read(rig, 0xC001);
  rom_read(rig, 0xC004);
  smartwatch_peek(&rig.sw, &r);
  EXPECT_EQ(r.state, 0);
  EXPECT_EQ(rom_read(rig, 0xC004), 0xAA);
}

TEST(Smartwatch, UpperRomDisableGatesTheSocket) {
  SwRig rig;
  make_rig(rig);
  smartwatch_set_plugged(&rig.sw, 1);

  // GA config write: upper-ROM disable (function 2, bit 3) — the socket's
  // /CE goes away, so pattern reads must not advance the FSM.
  {
    Bus b = bus_resting();
    b.cpu.iorq = b.cpu.wr = true;
    b.cpu.addr = 0x7F00;
    b.cpu.data = 0x80 | 0x08;
    rig.board.bus = b;
    board_tick(&rig.board);
    rig.board.bus = bus_resting();
    board_tick(&rig.board);
  }
  rom_read(rig, 0xC001);
  SmartwatchRegs r{};
  smartwatch_peek(&rig.sw, &r);
  EXPECT_EQ(r.state, 0) << "no /CE, no protocol";
}
