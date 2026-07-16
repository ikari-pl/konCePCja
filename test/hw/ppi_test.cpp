/* ppi_test.cpp — the CPC PPI 8255 Device: port directions, keyboard row select,
 * the AY-bus control lines, Port B status inputs, and the cassette motor. See
 * docs/hardware/ppi-device.md. */

#include "hw/ppi.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/board.h"

namespace {

struct PpiRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(ppi_state_size());
  Board board;
  Device dev;
};

void make_ppi(PpiRig& rig) {
  rig.dev = ppi_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
}

// One PPI I/O cycle. port = 0..3 (A/B/C/control); addr high byte 0xF4|port.
void io_write(PpiRig& rig, uint8_t port, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = static_cast<uint16_t>((0xF400) | (port << 8));
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
}
uint8_t io_read(PpiRig& rig, uint8_t port, bool vsync = false,
                uint8_t ay_da = 0xFF) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.addr = static_cast<uint16_t>((0xF400) | (port << 8));
  rig.board.bus.vid.vsync = vsync;
  rig.board.bus.ay.da = ay_da;
  board_tick(&rig.board);
  return rig.board.bus.cpu.data;
}
// Advance one idle tick so the PPI republishes its AY-bus outputs; return them.
AyBus ay_after_idle(PpiRig& rig, uint8_t ay_da_in = 0xFF) {
  rig.board.bus = bus_resting();
  rig.board.bus.ay.da = ay_da_in;
  board_tick(&rig.board);
  return rig.board.bus.ay;
}

constexpr uint8_t kCfg = 0x82;  // Port A output, Port B input, Port C output

}  // namespace

TEST(Ppi, ControlConfiguresAndClearsLatches) {
  PpiRig rig;
  make_ppi(rig);
  io_write(rig, 2, 0x3F);  // stuff Port C
  io_write(rig, 3, kCfg);  // mode-set → clears all latches
  PpiRegs r{};
  ppi_peek(&rig.dev, &r);
  EXPECT_EQ(r.control, kCfg);
  EXPECT_EQ(r.portC, 0x00) << "mode-set clears the output latches (real 8255)";
}

TEST(Ppi, PortCSelectsKeyboardRowAndDrivesAyControl) {
  PpiRig rig;
  make_ppi(rig);
  io_write(rig, 3, kCfg);  // Port C output
  io_write(rig, 2, 0xC5);  // BDIR+BC1 (0xC0) + row 5
  AyBus ay = ay_after_idle(rig);
  EXPECT_EQ(ay.kbd_row, 5) << "Port C low nibble → keyboard row";
  EXPECT_TRUE(ay.bdir) << "Port C bit 7 → BDIR";
  EXPECT_TRUE(ay.bc1) << "Port C bit 6 → BC1";

  io_write(rig, 2, 0x09);  // row 9, BDIR/BC1 clear
  ay = ay_after_idle(rig);
  EXPECT_EQ(ay.kbd_row, 9);
  EXPECT_FALSE(ay.bdir);
  EXPECT_FALSE(ay.bc1);
}

TEST(Ppi, PortAOutputPresentsDataToAy) {
  PpiRig rig;
  make_ppi(rig);
  io_write(rig, 3, kCfg);  // Port A output
  io_write(rig, 0, 0x0E);  // e.g. select AY register 14
  AyBus ay = ay_after_idle(rig);
  EXPECT_EQ(ay.da, 0x0E) << "Port A output drives the AY data bus";
}

TEST(Ppi, PortAInputReadsAyDataBus) {
  PpiRig rig;
  make_ppi(rig);
  io_write(rig, 3, 0x92);  // 0x92: Port A INPUT (bit4=1), Port B input
  const uint8_t got = io_read(rig, 0, /*vsync=*/false, /*ay_da=*/0x5A);
  EXPECT_EQ(got, 0x5A) << "Port A input returns what the AY drove on the bus";
}

TEST(Ppi, PortBReportsVsyncAndJumpers) {
  PpiRig rig;
  make_ppi(rig);
  io_write(rig, 3, kCfg);  // Port B is input under 0x82
  const uint8_t lo = io_read(rig, 1, /*vsync=*/false);
  const uint8_t hi = io_read(rig, 1, /*vsync=*/true);
  EXPECT_EQ(lo & 0x01, 0x00) << "VSYNC low → bit 0 clear";
  EXPECT_EQ(hi & 0x01, 0x01) << "VSYNC high → bit 0 set";
  EXPECT_EQ(lo & 0x1E, 0x1E) << "default jumpers: id 7 + 50 Hz";
  EXPECT_EQ(lo & 0x20, 0x20)
      << "/EXP (bit 5) reads 1 with no expansion pulling it low "
         "(cpcwiki/cpctech 8255: a device signals presence by driving /EXP "
         "low)";
  EXPECT_EQ(lo & 0x40, 0x40) << "BUSY (bit 6) reads 1 when no printer is ready";
}

TEST(Ppi, ResetStateIsAllInput0x9B) {
  PpiRig rig;
  make_ppi(rig);  // board_reset() runs
  PpiRegs r{};
  ppi_peek(&rig.dev, &r);
  EXPECT_EQ(r.control, 0x9B)
      << "8255 RESET: mode 0, all ports input; a regression to 0 (all-output) "
         "would make firmware read Port B / VSYNC as a dead latch";
  EXPECT_EQ(io_read(rig, 1, /*vsync=*/true) & 0x01, 0x01)
      << "VSYNC visible on Port B at reset (defaults to input, no mode-set "
         "yet)";
}

TEST(Ppi, PortCBitSetResetTogglesTapeMotor) {
  PpiRig rig;
  make_ppi(rig);
  io_write(rig, 3, kCfg);
  io_write(rig, 3, 0x09);  // BSR: set bit 4 (motor)  (0000 1001: bit#=4,set=1)
  PpiRegs r{};
  ppi_peek(&rig.dev, &r);
  EXPECT_EQ(r.tape_motor, 1) << "BSR set of Port C bit 4 turns the motor on";
  io_write(rig, 3, 0x08);  // BSR: clear bit 4
  ppi_peek(&rig.dev, &r);
  EXPECT_EQ(r.tape_motor, 0);
}
