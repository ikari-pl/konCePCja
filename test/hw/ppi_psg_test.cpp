/* ppi_psg_test.cpp — PPI + PSG on one board, exercising the internal AY bus end
 * to end: the firmware keyboard-scan handshake (select row → latch reg 14 →
 * read) and a register write reaching the PSG through Port A / Port C control.
 * See docs/hardware/ppi-device.md §6 and psg-device.md §3. */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/board.h"
#include "hw/ppi.h"
#include "hw/psg.h"

namespace {

// A stimulus device: drives the CPU lines from a settable CpuBus each tick, so
// the board can thread the AY bus between the PPI and PSG across settle cycles
// (the way a real multi-T-state Z80 I/O cycle holds its address/control lines).
struct Stim {
  CpuBus cpu{};
};
void stim_tick(void* self, const Bus*, Bus* out) {
  out->cpu = static_cast<Stim*>(self)->cpu;
}
size_t stim_size(const void*) { return sizeof(Stim); }
Device stim_device(Stim* s) {
  return Device{s,
                "stim",
                stim_tick,
                [](void*) {},
                stim_size,
                [](const void*, void*) {},
                [](void*, const void*) {}};
}

struct Rig {
  std::vector<uint8_t> pmem = std::vector<uint8_t>(ppi_state_size());
  std::vector<uint8_t> smem = std::vector<uint8_t>(psg_state_size());
  Board board;
  Device ppi, psg;
  Stim stim;
};

void make_rig(Rig& rig) {
  rig.ppi = ppi_init(rig.pmem.data());
  rig.psg = psg_init(rig.smem.data());
  board_init(&rig.board);
  board_add(&rig.board, stim_device(&rig.stim));
  board_add(&rig.board, rig.ppi);
  board_add(&rig.board, rig.psg);
  board_reset(&rig.board);
}

void settle(Rig& rig, int ticks = 4) {
  rig.stim.cpu = CpuBus{};  // release the CPU lines
  for (int i = 0; i < ticks; ++i) board_tick(&rig.board);
}

// A held Z80 I/O write: drive iorq+wr+addr+data for several master cycles, then
// idle.
void out(Rig& rig, uint8_t port, uint8_t val) {
  rig.stim.cpu = CpuBus{};
  rig.stim.cpu.iorq = true;
  rig.stim.cpu.wr = true;
  rig.stim.cpu.addr = static_cast<uint16_t>(0xF400 | (port << 8));
  rig.stim.cpu.data = val;
  for (int i = 0; i < 4; ++i) board_tick(&rig.board);
  settle(rig);
}

// A held Z80 I/O read: drive iorq+rd for several cycles; return the sampled
// data.
uint8_t in(Rig& rig, uint8_t port) {
  rig.stim.cpu = CpuBus{};
  rig.stim.cpu.iorq = true;
  rig.stim.cpu.rd = true;
  rig.stim.cpu.addr = static_cast<uint16_t>(0xF400 | (port << 8));
  uint8_t data = 0xFF;
  for (int i = 0; i < 4; ++i) {
    board_tick(&rig.board);
    data = rig.board.bus.cpu.data;  // PPI drives it; settled by the last cycle
  }
  settle(rig);
  return data;
}

constexpr uint8_t kCfg = 0x82;  // Port A output, Port B input, Port C output

}  // namespace

TEST(PpiPsg, KeyboardScanHandshake) {
  Rig rig;
  make_rig(rig);

  // Put keys on two rows: row 5 has key at column 2 pressed (bit 2 = 0).
  psg_set_key_row(&rig.psg, 5, 0xFB);
  psg_set_key_row(&rig.psg, 9, 0x7F);  // row 9: bit 7 pressed

  // Firmware scan — the real CPC toggles the 8255 between two configs: Port A
  // OUTPUT to send the register number to the AY, then Port A INPUT to read the
  // data bus.
  out(rig, 3, 0x82);      // Port A output
  out(rig, 0, 14);        // Port A → AY data bus = 14 (Port A register)
  out(rig, 2, 0xC0);      // Port C: BDIR+BC1 → AY latches address 14
  out(rig, 2, 0x00);      // inactive
  out(rig, 3, 0x92);      // Port A INPUT (mode-set clears Port C latch)
  out(rig, 2, 0x40 | 5);  // Port C: BC1 (read) + row 5
  EXPECT_EQ(in(rig, 0), 0xFB) << "reg-14 read returns row 5 key columns";

  out(rig, 2, 0x40 | 9);  // reselect row 9 (BC1 still read)
  EXPECT_EQ(in(rig, 0), 0x7F)
      << "changing the row nibble reads a different row";
}

TEST(PpiPsg, RegisterWriteReachesPsgThroughPpi) {
  Rig rig;
  make_rig(rig);
  out(rig, 3, kCfg);

  // Write AY register 8 (amplitude A) = 0x0C via the PPI: select, latch, then
  // write. Control returns to inactive between steps so changing Port A never
  // re-latches.
  out(rig, 0, 8);     // Port A = register number
  out(rig, 2, 0xC0);  // latch address 8
  out(rig, 2, 0x00);  // inactive
  out(rig, 0, 0x0C);  // Port A = value
  out(rig, 2, 0x80);  // BDIR only → AY write
  out(rig, 2, 0x00);  // inactive

  PsgRegs r{};
  psg_peek(&rig.psg, &r);
  EXPECT_EQ(r.reg[8], 0x0C)
      << "the value reached the PSG register file through the PPI";
}
