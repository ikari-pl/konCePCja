/* rs232_test.cpp — the Amstrad Serial Interface card Device: DART register
 * semantics, RX FIFO depth + overrun, 8253 divisor readback, and the
 * bit-serial wire (exact framing, loopback). See
 * docs/hardware/rs232-device.md. */

#include "hw/rs232.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/board.h"

namespace {

constexpr uint16_t DART_DATA = 0xFADC;
constexpr uint16_t DART_CTRL = 0xFADE;
constexpr uint16_t PIT_CTR0 = 0xFBDC;
constexpr uint16_t PIT_MODE = 0xFBDF;

struct SerialRig {
  std::vector<uint8_t> mem;
  Board board;
  Device dev;
  bool loopback = false;  // txd wired back into rxd

  void tick(bool rxd_level = true) {
    board.bus.serial.rxd = loopback ? board.bus.serial.txd : rxd_level;
    board_tick(&board);
  }
};

void make_rig(SerialRig& rig) {
  rig.mem.assign(rs232_state_size(), 0);
  rig.dev = rs232_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
  rs232_set_plugged(&rig.dev, 1);
}

void io_write(SerialRig& rig, uint16_t addr, uint8_t val) {
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = addr;
  rig.board.bus.cpu.data = val;
  rig.tick();
  rig.tick();  // deassert: the next access has an edge
}

uint8_t io_read(SerialRig& rig, uint16_t addr) {
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.addr = addr;
  rig.tick();
  const uint8_t val = rig.board.bus.cpu.data;
  rig.tick();  // deassert
  return val;
}

// Program the 8253: counter 0, LSB/MSB access, mode 3 square wave.
void set_divisor(SerialRig& rig, uint16_t divisor) {
  io_write(rig, PIT_MODE, 0x36);
  io_write(rig, PIT_CTR0, static_cast<uint8_t>(divisor & 0xFF));
  io_write(rig, PIT_CTR0, static_cast<uint8_t>(divisor >> 8));
}

// Clock one 8N1 frame into the card's rxd at the given bit time.
void drive_frame(SerialRig& rig, uint8_t byte, uint32_t bit_time) {
  for (uint32_t i = 0; i < bit_time; i++) rig.tick(false);  // start bit
  for (int bit = 0; bit < 8; bit++) {
    const bool level = (byte >> bit) & 1;
    for (uint32_t i = 0; i < bit_time; i++) rig.tick(level);
  }
  for (uint32_t i = 0; i < bit_time; i++) rig.tick(true);  // stop bit
}

}  // namespace

TEST(Rs232, DartRegisterPointerWritesLandAndSnapBack) {
  SerialRig rig;
  make_rig(rig);
  io_write(rig, DART_CTRL, 0x03);  // WR0: pointer -> 3
  io_write(rig, DART_CTRL, 0xC1);  // lands in WR3
  io_write(rig, DART_CTRL, 0x05);  // pointer -> 5
  io_write(rig, DART_CTRL, 0xEA);  // lands in WR5
  Rs232Regs regs{};
  rs232_peek(&rig.dev, &regs);
  EXPECT_EQ(regs.wr[3], 0xC1);
  EXPECT_EQ(regs.wr[5], 0xEA);
  // Channel reset (WR0 command 011) clears the write registers.
  io_write(rig, DART_CTRL, 0x18);
  rs232_peek(&rig.dev, &regs);
  EXPECT_EQ(regs.wr[3], 0x00);
  EXPECT_EQ(regs.wr[5], 0x00);
}

TEST(Rs232, StatusReadsReadyWhenIdle) {
  SerialRig rig;
  make_rig(rig);
  const uint8_t rr0 = io_read(rig, DART_CTRL);
  EXPECT_TRUE(rr0 & 0x04) << "TX empty when nothing queued";
  EXPECT_TRUE(rr0 & 0x08) << "TX buffer empty when nothing queued";
  EXPECT_TRUE(rr0 & 0x20) << "CTS asserted (spec V1)";
  EXPECT_FALSE(rr0 & 0x01) << "no RX data yet";
}

TEST(Rs232, PitDivisorRoundTripsThroughLatch) {
  SerialRig rig;
  make_rig(rig);
  set_divisor(rig, 13);           // 9615 baud, the CP/M SIO default shape
  io_write(rig, PIT_MODE, 0x00);  // counter-latch command for counter 0
  EXPECT_EQ(io_read(rig, PIT_CTR0), 13);  // LSB
  EXPECT_EQ(io_read(rig, PIT_CTR0), 0);   // MSB
  Rs232Regs regs{};
  rs232_peek(&rig.dev, &regs);
  EXPECT_EQ(regs.divisor, 13);
}

TEST(Rs232, TxFramesExactBitTimesOnTheWire) {
  SerialRig rig;
  make_rig(rig);
  set_divisor(rig, 1);  // bit_time = 128 master cycles
  const uint32_t bit_time = 128;
  io_write(rig, DART_DATA, 0x55);  // 01010101

  // Hunt for the start edge, then sample every bit at its center.
  uint32_t waited = 0;
  while (rig.board.bus.serial.txd && waited < 4 * bit_time) {
    rig.tick();
    waited++;
  }
  ASSERT_LT(waited, 4 * bit_time) << "start bit never appeared";

  // Expected 8N1 frame for 0x55, LSB first: start 0, 1,0,1,0,1,0,1,0, stop 1.
  const bool expect[10] = {false, true,  false, true,  false,
                           true,  false, true,  false, true};
  for (int bit = 0; bit < 10; bit++) {
    for (uint32_t i = 0; i < bit_time / 2; i++) rig.tick();
    EXPECT_EQ(rig.board.bus.serial.txd, expect[bit]) << "bit " << bit;
    for (uint32_t i = 0; i < bit_time - bit_time / 2; i++) rig.tick();
  }
  EXPECT_TRUE(rig.board.bus.serial.txd) << "line returns to mark";
  const uint8_t rr1 = [&] {
    io_write(rig, DART_CTRL, 0x01);  // pointer -> RR1
    return io_read(rig, DART_CTRL);
  }();
  EXPECT_TRUE(rr1 & 0x01) << "all-sent after the stop bit";
}

TEST(Rs232, RxAssemblesFramesIntoTheFifo) {
  SerialRig rig;
  make_rig(rig);
  set_divisor(rig, 1);
  drive_frame(rig, 0xA7, 128);
  for (int i = 0; i < 64; i++) rig.tick();  // settle past the stop bit
  EXPECT_TRUE(io_read(rig, DART_CTRL) & 0x01) << "RX available";
  EXPECT_EQ(io_read(rig, DART_DATA), 0xA7);
  EXPECT_FALSE(io_read(rig, DART_CTRL) & 0x01) << "FIFO drained";
}

TEST(Rs232, RxFifoHoldsThreeThenOverruns) {
  SerialRig rig;
  make_rig(rig);
  set_divisor(rig, 1);
  for (uint8_t i = 1; i <= 4; i++) drive_frame(rig, i, 128);
  for (int i = 0; i < 64; i++) rig.tick();
  Rs232Regs regs{};
  rs232_peek(&rig.dev, &regs);
  EXPECT_EQ(regs.fifo_depth, 3);
  EXPECT_TRUE(regs.rr1 & 0x10) << "overrun flagged, newest byte lost";
  EXPECT_EQ(io_read(rig, DART_DATA), 1);
  EXPECT_EQ(io_read(rig, DART_DATA), 2);
  EXPECT_EQ(io_read(rig, DART_DATA), 3);
}

TEST(Rs232, LoopbackRoundTripsAByte) {
  SerialRig rig;
  make_rig(rig);
  set_divisor(rig, 1);
  rig.loopback = true;
  io_write(rig, DART_DATA, 0x3C);
  for (int i = 0; i < 128 * 12; i++) rig.tick();
  EXPECT_TRUE(io_read(rig, DART_CTRL) & 0x01) << "byte came back";
  EXPECT_EQ(io_read(rig, DART_DATA), 0x3C);
}

TEST(Rs232, UnpluggedNothingDecodesAndWireRests) {
  SerialRig rig;
  make_rig(rig);
  rs232_set_plugged(&rig.dev, 0);
  io_write(rig, DART_DATA, 0x55);
  for (int i = 0; i < 256; i++) rig.tick();
  EXPECT_TRUE(rig.board.bus.serial.txd) << "wire rests at mark";
  EXPECT_EQ(io_read(rig, DART_CTRL), 0xFF) << "data bus floats";
}

TEST(Rs232, SnapshotRoundTripsMidFrame) {
  SerialRig rig;
  make_rig(rig);
  set_divisor(rig, 1);
  io_write(rig, DART_DATA, 0x99);
  for (int i = 0; i < 300; i++) rig.tick();  // mid-frame on the wire

  std::vector<uint8_t> blob(rig.dev.state_size(rig.dev.self));
  rig.dev.save(rig.dev.self, blob.data());

  SerialRig rig2;
  make_rig(rig2);
  rig2.dev.load(rig2.dev.self, blob.data());

  // Both rigs finish the frame identically, edge for edge.
  for (int i = 0; i < 128 * 12; i++) {
    rig.tick();
    rig2.tick();
    ASSERT_EQ(rig.board.bus.serial.txd, rig2.board.bus.serial.txd)
        << "wire diverged at cycle " << i;
  }
}
