/* mf2_test.cpp — the Multiface II Device: paging via &FEE8/&FEEA, the
 * ROM/RAM overlay under /ROMDIS+/RAMDIS (with the one-tick settle a real
 * held-strobe access tolerates), exclusive writes to the freeze RAM, the
 * bus-snooped hardware shadow, INVISIBLE semantics, and the STOP button's
 * /NMI. See docs/hardware/multiface-device.md. */

#include "hw/mf2.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "hw/board.h"
#include "hw/memory.h"
#include "subcycle/machine.h"

namespace {

struct Mf2Rig {
  std::vector<uint8_t> mmem = std::vector<uint8_t>(mem_state_size());
  std::vector<uint8_t> fmem = std::vector<uint8_t>(mf2_state_size());
  std::vector<uint8_t> rom = std::vector<uint8_t>(0x2000, 0x00);
  std::vector<uint8_t> firmware = std::vector<uint8_t>(0x4000, 0x00);
  Board board;
  Device mem;
  Device mf2;
};

void make_rig(Mf2Rig& rig) {
  rig.mem = mem_init(rig.mmem.data());
  rig.mf2 = mf2_init(rig.fmem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.mem);
  board_add(&rig.board, rig.mf2);
  board_reset(&rig.board);
  for (size_t i = 0; i < rig.rom.size(); ++i)
    rig.rom[i] = static_cast<uint8_t>(0xA0 + (i & 0x0F));  // MF2 ROM pattern
  for (size_t i = 0; i < rig.firmware.size(); ++i)
    rig.firmware[i] = static_cast<uint8_t>(0x10 + (i & 0x0F));  // firmware
  mem_load_lower_rom(&rig.mem, rig.firmware.data(), rig.firmware.size());
  mf2_attach_rom(&rig.mf2, rig.rom.data(), rig.rom.size());
}

// Hold the crafted CPU pins across ticks while PRESERVING what the devices
// drove last tick (data, romdis, ramdis) — the rig's stand-in for a real
// Z80 access, which keeps its strobes up while expansion lines settle.
void hold_tick(Mf2Rig& rig, void (*craft)(Bus&, uint32_t), uint32_t arg) {
  Bus b = rig.board.bus;  // device outputs persist (the CPU only re-drives
  b.cpu.addr = 0;         // its own pins)
  b.cpu.data = b.cpu.data;
  b.cpu.m1 = b.cpu.mreq = b.cpu.iorq = b.cpu.rd = b.cpu.wr = false;
  craft(b, arg);
  rig.board.bus = b;
  board_tick(&rig.board);
}

void idle(Mf2Rig& rig, int n = 2) {
  for (int i = 0; i < n; ++i) {
    Bus b = rig.board.bus;
    b.cpu.mreq = b.cpu.iorq = b.cpu.rd = b.cpu.wr = b.cpu.m1 = false;
    rig.board.bus = b;
    board_tick(&rig.board);
  }
}

uint8_t bus_read(Mf2Rig& rig, uint16_t addr) {
  for (int i = 0; i < 6; ++i)  // 6 held ticks ≈ the read half of an M cycle
    hold_tick(
        rig,
        [](Bus& b, uint32_t a) {
          b.cpu.addr = static_cast<uint16_t>(a);
          b.cpu.mreq = b.cpu.rd = true;
        },
        addr);
  const uint8_t val = rig.board.bus.cpu.data;  // sampled at the END (T3)
  idle(rig);
  return val;
}

void bus_write(Mf2Rig& rig, uint16_t addr, uint8_t val) {
  for (int i = 0; i < 4; ++i)
    hold_tick(
        rig,
        [](Bus& b, uint32_t av) {
          b.cpu.addr = static_cast<uint16_t>(av >> 8);
          b.cpu.data = static_cast<uint8_t>(av & 0xFF);
          b.cpu.mreq = b.cpu.wr = true;
        },
        (static_cast<uint32_t>(addr) << 8) | val);
  idle(rig);
}

void io_write(Mf2Rig& rig, uint16_t addr, uint8_t val) {
  for (int i = 0; i < 4; ++i)
    hold_tick(
        rig,
        [](Bus& b, uint32_t av) {
          b.cpu.addr = static_cast<uint16_t>(av >> 8);
          b.cpu.data = static_cast<uint8_t>(av & 0xFF);
          b.cpu.iorq = b.cpu.wr = true;
        },
        (static_cast<uint32_t>(addr) << 8) | val);
  idle(rig);
}

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

}  // namespace

TEST(Mf2, PagingOverlaysRomAndRamOverTheBottom16K) {
  Mf2Rig rig;
  make_rig(rig);

  EXPECT_EQ(bus_read(rig, 0x0105), rig.firmware[0x0105])
      << "unplugged: the firmware ROM answers";
  mf2_set_plugged(&rig.mf2, 1);
  io_write(rig, 0xFEE8, 0x00);  // page in
  EXPECT_EQ(bus_read(rig, 0x0105), rig.rom[0x0105]) << "MF2 ROM at 0x0000";
  EXPECT_EQ(bus_read(rig, 0x0105), rig.rom[0x0105]) << "stable across reads";

  bus_write(rig, 0x2345, 0x5A);  // MF2 RAM while paged in
  EXPECT_EQ(bus_read(rig, 0x2345), 0x5A) << "MF2 RAM readback";
  EXPECT_EQ(mf2_ram_peek(&rig.mf2, 0x0345), 0x5A);

  io_write(rig, 0xFEEA, 0x00);  // page out
  EXPECT_EQ(bus_read(rig, 0x0105), rig.firmware[0x0105])
      << "firmware ROM restored";
  EXPECT_NE(bus_read(rig, 0x2345), 0x5A)
      << "the freeze write never reached internal memory (exclusivity)";
}

TEST(Mf2, WritesLandInternallyWhenNotPagedIn) {
  Mf2Rig rig;
  make_rig(rig);
  mf2_set_plugged(&rig.mf2, 1);

  // Lower ROM covers reads at 0x2345, so disable it via the GA mode register
  // (function 2, bit 2 = lower-ROM disable) to see the RAM underneath.
  io_write(rig, 0x7F00, 0x80 | 0x04);
  bus_write(rig, 0x2345, 0x77);
  EXPECT_EQ(bus_read(rig, 0x2345), 0x77) << "internal RAM took the write";
  EXPECT_NE(mf2_ram_peek(&rig.mf2, 0x0345), 0x77)
      << "the MF2 RAM stays untouched while paged out";
}

TEST(Mf2, ShadowCellsFollowHardwareWrites) {
  Mf2Rig rig;
  make_rig(rig);
  mf2_set_plugged(&rig.mf2, 1);

  io_write(rig, 0x7F00, 0x02);         // GA: pen select 2
  io_write(rig, 0x7F00, 0x40 | 0x0B);  // GA: ink 11
  io_write(rig, 0x7F00, 0x80 | 0x01);  // GA: mode 1 / ROM config
  io_write(rig, 0x7F00, 0xC0 | 0x05);  // PAL: RAM config 5
  io_write(rig, 0xBC00, 0x06);         // CRTC: select R6
  io_write(rig, 0xBD00, 0x19);         // CRTC: R6 = 25
  io_write(rig, 0xDF00, 0x07);         // upper-ROM select 7

  EXPECT_EQ(mf2_ram_peek(&rig.mf2, 0x3FCF - 0x2000), 0x02);
  EXPECT_EQ(mf2_ram_peek(&rig.mf2, 0x3FEF - 0x2000), 0x40 | 0x0B);
  EXPECT_EQ(mf2_ram_peek(&rig.mf2, 0x3FFF - 0x2000), 0x80 | 0x01);
  EXPECT_EQ(mf2_ram_peek(&rig.mf2, 0x37FF - 0x2000), 0xC0 | 0x05);
  EXPECT_EQ(mf2_ram_peek(&rig.mf2, 0x3CFF - 0x2000), 0x06);
  EXPECT_EQ(mf2_ram_peek(&rig.mf2, (0x3DB0 | 6) - 0x2000), 0x19);
  EXPECT_EQ(mf2_ram_peek(&rig.mf2, 0x3AAC - 0x2000), 0x07);
}

TEST(Mf2, StopFreezesAndExitHidesUntilReset) {
  Mf2Rig rig;
  make_rig(rig);
  mf2_set_plugged(&rig.mf2, 1);

  mf2_stop(&rig.mf2);
  idle(rig, 1);
  EXPECT_TRUE(rig.board.bus.cpu.nmi) << "STOP drives /NMI onto the bus";
  Mf2Regs r{};
  mf2_peek(&rig.mf2, &r);
  EXPECT_EQ(r.active, 1) << "paged in for the &0066 vector fetch";
  idle(rig, 32);
  EXPECT_FALSE(rig.board.bus.cpu.nmi) << "the pulse releases";

  io_write(rig, 0xFEEA, 0x00);  // the freeze menu's exit path
  mf2_peek(&rig.mf2, &r);
  EXPECT_EQ(r.active, 0);
  EXPECT_EQ(r.invisible, 1) << "hidden after the session (anti-detection)";
  io_write(rig, 0xFEE8, 0x00);
  mf2_peek(&rig.mf2, &r);
  EXPECT_EQ(r.active, 0) << "&FEE8 is dead while invisible";

  board_reset(&rig.board);
  mf2_peek(&rig.mf2, &r);
  EXPECT_EQ(r.invisible, 0) << "a machine reset un-hides the cartridge";
  io_write(rig, 0xFEE8, 0x00);
  mf2_peek(&rig.mf2, &r);
  EXPECT_EQ(r.active, 1);
}

TEST(Mf2, StopOnTheFullMachineEntersTheMf2Rom) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  // A synthetic 8K MF2 ROM: the NMI vector code at 0x0066 parks in a tight
  // loop we can observe (the real ROM is optional and not in the repo).
  std::vector<uint8_t> mf2rom(0x2000, 0x00);
  mf2rom[0x0066] = 0x18;  // JR -2: loop at 0x0066 in MF2 ROM space
  mf2rom[0x0067] = 0xFE;

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  m.attach_mf2_rom(mf2rom.data(), mf2rom.size());
  m.set_mf2(true);
  for (int i = 0; i < 120; ++i) m.run_frame();  // to the Ready screen

  m.mf2_stop_button();
  for (int i = 0; i < 3; ++i) m.run_frame();
  Mf2Regs r{};
  mf2_peek(m.mf2(), &r);
  EXPECT_EQ(r.active, 1);
  const uint16_t pc = m.regs().pc;
  EXPECT_GE(pc, 0x0066);
  EXPECT_LE(pc, 0x0068) << "the CPU took the NMI into the MF2 ROM's loop";
}
