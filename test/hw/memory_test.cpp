/* memory_test.cpp — the CPC memory map: ROM overlays, banking I/O, and a boot from
 * lower ROM. See docs/hardware/memory-device.md. */

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "hw/board.h"
#include "hw/memory.h"
#include "hw/z80.h"

namespace {

struct MemRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(mem_state_size());
  Board board;
  Device dev;
};

void make_mem(MemRig& rig) {
  rig.dev = mem_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
}

uint8_t rd(MemRig& rig, uint16_t addr) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.mreq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.addr = addr;
  board_tick(&rig.board);
  return rig.board.bus.cpu.data;
}
void wr(MemRig& rig, uint16_t addr, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.mreq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = addr;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
}
void io(MemRig& rig, uint8_t data) {  // GA banking write
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = 0x7F00;
  rig.board.bus.cpu.data = data;
  board_tick(&rig.board);
}

// Always-on clock for the boot test.
void mclk_tick(void*, const Bus*, Bus* out) { out->clk.cpu = true; }
size_t mclk_size(const void*) { return 1; }
void mclk_save(const void*, void* b) { *static_cast<uint8_t*>(b) = 0; }
void mclk_load(void*, const void*) {}
Device mclk_device() {
  static uint8_t d = 0;
  return Device{&d, "clk", mclk_tick, [](void*){}, mclk_size, mclk_save, mclk_load};
}

}  // namespace

TEST(Memory, RomOverlaysAndBankingIO) {
  MemRig rig;
  make_mem(rig);
  uint8_t lower[0x4000] = {0}; lower[0] = 0xAA; lower[1] = 0xBB;
  uint8_t upper[0x4000] = {0}; upper[0] = 0xCC;
  mem_load_lower_rom(&rig.dev, lower, sizeof(lower));
  mem_load_upper_rom(&rig.dev, upper, sizeof(upper));

  EXPECT_EQ(rd(rig, 0x0000), 0xAA) << "lower ROM overlays 0x0000 by default";
  EXPECT_EQ(rd(rig, 0xC000), 0xCC) << "upper ROM overlays 0xC000 by default";
  EXPECT_EQ(rd(rig, 0x8000), 0x00) << "middle is RAM";

  wr(rig, 0x0000, 0x11);                      // write-through: lands in RAM under the ROM
  EXPECT_EQ(rd(rig, 0x0000), 0xAA) << "ROM still visible after the RAM write";
  io(rig, 0x80 | 0x04);                            // mode reg, bit2 = lower-ROM disable
  EXPECT_EQ(rd(rig, 0x0000), 0x11) << "RAM revealed when the lower ROM is disabled";
  io(rig, 0x80);                                   // re-enable lower ROM
  EXPECT_EQ(rd(rig, 0x0000), 0xAA);

  io(rig, 0x80 | 0x08);                            // bit3 = upper-ROM disable
  EXPECT_EQ(rd(rig, 0xC000), 0x00) << "RAM revealed when the upper ROM is disabled";

  MemRegs mr{};
  mem_peek(&rig.dev, &mr);
  EXPECT_EQ(mr.rom_config & 0x08, 0x08);
}

TEST(Memory, Z80BootsFromLowerRom) {
  // Program in lower ROM: LD A,0x42 ; LD (0x8000),A ; HALT — proving the Z80 fetches
  // and runs code from ROM at reset (PC=0).
  const uint8_t boot[] = {0x3E, 0x42, 0x32, 0x00, 0x80, 0x76};

  std::vector<uint8_t> mmem(mem_state_size());  Device mdev = mem_init(mmem.data());
  std::vector<uint8_t> zmem(z80_state_size());  Device zdev = z80_init(zmem.data());
  Board board;
  board_init(&board);
  board_add(&board, mclk_device());
  board_add(&board, mdev);
  board_add(&board, zdev);
  board_reset(&board);
  mem_load_lower_rom(&mdev, boot, sizeof(boot));  // after reset (reset keeps ROM anyway)

  Z80Regs r{};
  for (int tick = 0; tick < 4000; ++tick) {
    board_tick(&board);
    z80_peek(&zdev, &r);
    if (r.halted) break;
  }
  EXPECT_EQ(r.halted, 1) << "the Z80 ran the ROM program to HALT";
  EXPECT_EQ(static_cast<uint8_t>(r.af >> 8), 0x42);
  EXPECT_EQ(mem_read_ram(&mdev, 0x8000), 0x42) << "ROM code wrote 0x42 to RAM at 0x8000";
}
