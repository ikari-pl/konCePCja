/* psg_test.cpp — the AY-3-8912 PSG Device: register file + masks, the AY-bus
 * latch/write/read protocol, keyboard read-back on register 14, tone toggle
 * rate, and the 10 envelope shapes. See docs/hardware/psg-device.md. */

#include "hw/psg.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/board.h"

namespace {

struct PsgRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(psg_state_size());
  Board board;
  Device dev;
};

void make_psg(PsgRig& rig) {
  rig.dev = psg_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
}

// Drive the AY bus for one master cycle (no psg clock → no sound step).
void ay_cycle(PsgRig& rig, bool bdir, bool bc1, uint8_t da, uint8_t row = 0) {
  rig.board.bus = bus_resting();
  rig.board.bus.ay.bdir = bdir;
  rig.board.bus.ay.bc1 = bc1;
  rig.board.bus.ay.da = da;
  rig.board.bus.ay.kbd_row = row;
  board_tick(&rig.board);
}
// Read a register through the bus: BC1 asserted → PSG drives ay.da.
uint8_t ay_read(PsgRig& rig, uint8_t reg, uint8_t row = 0) {
  ay_cycle(rig, true, true, reg);  // latch address
  rig.board.bus = bus_resting();
  rig.board.bus.ay.bc1 = true;  // read
  rig.board.bus.ay.kbd_row = row;
  board_tick(&rig.board);
  return rig.board.bus.ay.da;
}
void ay_write(PsgRig& rig, uint8_t reg, uint8_t val) {
  ay_cycle(rig, true, true, reg);   // latch address
  ay_cycle(rig, true, false, val);  // write value
}
// Advance the PSG sound engine by n 1 MHz steps.
void psg_clocks(PsgRig& rig, int n) {
  for (int i = 0; i < n; ++i) {
    rig.board.bus = bus_resting();
    rig.board.bus.clk.psg = true;
    board_tick(&rig.board);
  }
}

}  // namespace

TEST(Psg, RegisterFileWriteReadWithMasks) {
  PsgRig rig;
  make_psg(rig);
  ay_write(rig, 0, 0xFF);
  ay_write(rig, 1, 0xFF);  // coarse tone A: masked to 0x0F
  ay_write(rig, 6, 0xFF);  // noise: masked to 0x1F
  ay_write(rig, 8, 0xFF);  // amplitude A: masked to 0x1F
  EXPECT_EQ(ay_read(rig, 0), 0xFF);
  EXPECT_EQ(ay_read(rig, 1), 0x0F) << "coarse tone period is 4-bit";
  EXPECT_EQ(ay_read(rig, 6), 0x1F) << "noise period is 5-bit";
  EXPECT_EQ(ay_read(rig, 8), 0x1F) << "amplitude is 5-bit";
}

TEST(Psg, LatchWriteReadRoundTrip) {
  PsgRig rig;
  make_psg(rig);
  ay_write(rig, 2, 0xAB);
  ay_write(rig, 4, 0xCD);
  EXPECT_EQ(ay_read(rig, 2), 0xAB);
  EXPECT_EQ(ay_read(rig, 4), 0xCD);
  PsgRegs r{};
  psg_peek(&rig.dev, &r);
  EXPECT_EQ(r.sel, 4) << "selected register persists after a read";
}

TEST(Psg, KeyboardReadOnRegister14) {
  PsgRig rig;
  make_psg(rig);
  // reg 7 mixer: Port A input (bit 6 = 0) — default 0xFF has bit6 set, so clear
  // it.
  ay_write(rig, 7, 0x3F);              // bits 6,7 clear → both ports input
  psg_set_key_row(&rig.dev, 5, 0xFB);  // row 5: one key pressed (bit 2 = 0)
  psg_set_key_row(&rig.dev, 9, 0x7F);  // row 9: bit 7 pressed
  EXPECT_EQ(ay_read(rig, 14, /*row=*/5), 0xFB)
      << "reg14 read returns row 5 columns";
  EXPECT_EQ(ay_read(rig, 14, /*row=*/9), 0x7F)
      << "row select picks the matrix row";
}

TEST(Psg, ToneTogglesAtExpectedRate) {
  PsgRig rig;
  make_psg(rig);
  ay_write(rig, 0, 0x01);  // tone A period = 1 → toggle every 8 psg clocks (÷8)
  ay_write(rig, 1, 0x00);
  PsgRegs r{};
  psg_peek(&rig.dev, &r);
  const uint8_t before = r.tone_out & 1;
  psg_clocks(rig, 7);  // not yet — one shy of a ÷8 period
  psg_peek(&rig.dev, &r);
  EXPECT_EQ(r.tone_out & 1, before) << "no toggle before 8 clocks";
  psg_clocks(rig, 1);  // completes the ÷8 period → f = 62500/period
  psg_peek(&rig.dev, &r);
  EXPECT_NE(r.tone_out & 1, before)
      << "channel A square wave toggled after 8 clocks";
}

TEST(Psg, NoiseLfsrAdvances) {
  // Independent ground truth: the AY 17-bit noise LFSR with the MAME/hardware
  // taps 0 and 3 (feedback = bit0 ^ bit3) seeded to 1 emits a FIXED output
  // sequence. At period 1 the LFSR steps once per 16 PSG clocks. With seed 1
  // the lone set bit needs 16 shifts to reach the output, so the first 16 taps
  // read 0, then a 1 at step 16 and again at step 30 (16 + the tap distance).
  // A wrong seed, tap position, or shift direction would move those 1s.
  static const uint8_t kNoiseRef[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                        0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
                                        0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
  PsgRig rig;
  make_psg(rig);
  ay_write(rig, 6, 0x01);  // noise period 1 → one LFSR step per 16 clocks
  for (int i = 0; i < 32; ++i) {
    psg_clocks(rig, 16);  // advance the LFSR exactly one step
    PsgRegs r{};
    psg_peek(&rig.dev, &r);
    EXPECT_EQ(r.noise_out, kNoiseRef[i])
        << "noise output bit " << i << " of the seed-1 LFSR sequence";
  }
}

// Sample an envelope shape: write period 0 (fastest) + shape, then read the
// level once per envelope step (256 psg clocks). Returns the first `count`
// levels.
std::vector<int> sample_env(PsgRig& rig, uint8_t shape, int count) {
  make_psg(rig);
  ay_write(rig, 11, 0x00);
  ay_write(rig, 12, 0x00);   // envelope period → 1
  ay_write(rig, 13, shape);  // restarts the envelope
  std::vector<int> levels;
  PsgRegs r{};
  psg_peek(&rig.dev, &r);
  levels.push_back(r.env_level);
  for (int i = 0; i < count - 1; ++i) {
    psg_clocks(rig, 256);  // one envelope step
    psg_peek(&rig.dev, &r);
    levels.push_back(r.env_level);
  }
  return levels;
}

TEST(Psg, EnvelopeShapeDecayThenHold) {
  PsgRig rig;
  // Shape 0x09 (CONT+HOLD, no ATT): ramp down 31→0 then hold at 0.
  auto lv = sample_env(rig, 0x09, 40);
  EXPECT_EQ(lv[0], 31) << "starts high";
  EXPECT_GT(lv[0], lv[5]) << "ramps down";
  EXPECT_EQ(lv[33], 0) << "held low after one decay segment";
  EXPECT_EQ(lv[39], 0);
}

TEST(Psg, EnvelopeShapeAttackThenHoldHigh) {
  PsgRig rig;
  // Shape 0x0D (CONT+ATT+HOLD): ramp up 0→31 then hold at 31.
  auto lv = sample_env(rig, 0x0D, 40);
  EXPECT_EQ(lv[0], 0) << "starts low";
  EXPECT_LT(lv[0], lv[5]) << "ramps up";
  EXPECT_EQ(lv[33], 31) << "held high after one attack segment";
  EXPECT_EQ(lv[39], 31);
}

TEST(Psg, EnvelopeShapeRepeatingSawtooth) {
  PsgRig rig;
  // Shape 0x08 (CONT only): ramp down repeating 31→0, 31→0, ...
  auto lv = sample_env(rig, 0x08, 70);
  EXPECT_EQ(lv[0], 31);
  EXPECT_EQ(lv[31], 0) << "reaches 0 at the end of the first segment";
  EXPECT_EQ(lv[32], 31) << "wraps back to 31 (repeating)";
}

TEST(Psg, EnvelopeShapeTriangle) {
  PsgRig rig;
  // Shape 0x0E (CONT+ATT+ALT): triangle up 0→31, then down 31→0, repeating. The
  // peak (31) holds for one extra sample at the alternate boundary — accepted
  // AY behavior.
  auto lv = sample_env(rig, 0x0E, 70);
  EXPECT_EQ(lv[0], 0);
  EXPECT_EQ(lv[31], 31) << "peak at the segment boundary";
  EXPECT_LT(lv[40], lv[31]) << "descending after the peak";
  EXPECT_EQ(lv[63], 0)
      << "returns to the trough at the end of the falling segment";
}

TEST(Psg, AmplitudeScalesToFiveBits) {
  // Independent ground truth (NOT a restatement of the impl's `2V+1`): on the
  // AY-3-8910/8912 the 16 fixed 4-bit amplitudes select the ODD steps of the
  // 32-level (5-bit) DAC — levels 1,3,…,31 — the same 16 rungs the log volume
  // table Amplitudes_AY[0..15] spans. These are literal hardware values; a
  // impl that used 2V, 2V+2, or a lookup typo would miss the vector.
  static const uint8_t kFixedLevel[16] = {1,  3,  5,  7,  9,  11, 13, 15,
                                          17, 19, 21, 23, 25, 27, 29, 31};
  PsgRig rig;
  make_psg(rig);
  ay_write(rig, 7, 0x3F);  // mixer: tone + noise disabled → steady DC level
  for (uint8_t v = 0; v < 16; ++v) {
    ay_write(rig, 8, v);  // channel A fixed amplitude v (bit 4 clear)
    psg_clocks(rig, 1);
    PsgRegs r{};
    psg_peek(&rig.dev, &r);
    EXPECT_EQ(r.chan_level[0], kFixedLevel[v])
        << "amp " << int(v) << " → AY fixed-DAC step (odd rung " << int(v)
        << ")";
  }
}
