/* plus_rmr2_mode_test.cpp — the 6128+ RMR2 vs screen-mode disambiguation in the
 * Gate Array. On a Plus with the ASIC register page unlocked, a Gate-Array
 * function-2 write (A15=0/A14=1, data>>6==2) with bit5 (0x20) SET is the RMR2
 * low-ROM bank-remap register, NOT a screen-mode/ROM/interrupt write — the mode,
 * ROM config and the raster counter must all be left untouched. Only the memory
 * Device acts on it (remapping the low-ROM cartridge bank). This mirrors the
 * legacy z80_OUT_handler (case 2: `if (!asic.locked && (val & 0x20)) …RMR2…`).
 *
 * Regression: Burnin' Rubber's raster handler builds a mode-1 fence band, then
 * emits RMR2 writes (data 0xB8/0xB0, bit5 set) for its music/ROM paging. Without
 * this gate the Gate Array mis-read those as mode-0 writes, clobbering the fence
 * back to mode 0 and — via their bit4 — resetting the raster-interrupt counter,
 * corrupting the whole per-band pipeline. */

#include "hw/gate_array.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/asic.h"
#include "hw/board.h"

namespace {

struct Rig {
  std::vector<uint8_t> gmem = std::vector<uint8_t>(ga_state_size());
  std::vector<uint8_t> amem = std::vector<uint8_t>(asic_state_size());
  Board board;
  Device ga;
  Device asic;
};

void make_rig(Rig& rig) {
  rig.ga = ga_init(rig.gmem.data());
  rig.asic = asic_init(rig.amem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.ga);
  board_add(&rig.board, rig.asic);
  ga_attach_asic(&rig.ga, &rig.asic);
  board_reset(&rig.board);
}

void tick(Rig& rig, const Bus& in) {
  rig.board.bus = in;
  board_tick(&rig.board);
}

// A CRTC register-select write (&BC00) — the port the ASIC unlock knock rides.
void knock(Rig& rig, uint8_t val) {
  Bus in = bus_resting();
  in.cpu.iorq = true;
  in.cpu.wr = true;
  in.cpu.addr = 0xBC00;
  in.cpu.data = val;
  tick(rig, in);
  tick(rig, bus_resting());  // release (edge-triggered snoop)
}

void unlock_asic(Rig& rig) {
  asic_set_plugged(&rig.asic, 1);
  const uint8_t seq[17] = {0xFF, 0x00, 0xFF, 0x77, 0xB3, 0x51, 0xA8, 0xD4, 0x62,
                           0x39, 0x9C, 0x46, 0x2B, 0x15, 0x8A, 0xCD, 0x01};
  for (uint8_t b : seq) knock(rig, b);
  AsicRegs a{};
  asic_peek(&rig.asic, &a);
  ASSERT_EQ(a.locked, 0) << "the knock should have unlocked the ASIC";
}

// A Gate-Array function-2 write (A15=0/A14=1 → &7Fxx), then release.
void ga_fn2_write(Rig& rig, uint8_t data) {
  Bus in = bus_resting();
  in.cpu.iorq = true;
  in.cpu.wr = true;
  in.cpu.addr = 0x7F00;
  in.cpu.data = data;
  tick(rig, in);
  tick(rig, bus_resting());
}

// One HSYNC rising edge — latches req_mode into the visible mode.
void hsync_pulse(Rig& rig) {
  Bus in = bus_resting();
  in.vid.hsync = true;
  tick(rig, in);
  tick(rig, bus_resting());
}

GateArrayRegs peek(Rig& rig) {
  GateArrayRegs g{};
  ga_peek(&rig.ga, &g);
  return g;
}

}  // namespace

// A bit5-clear fn-2 write is a normal mode/ROM write on a Plus, even unlocked.
TEST(PlusRmr2Mode, ModeWriteStillWorksWhenUnlocked) {
  Rig rig;
  make_rig(rig);
  unlock_asic(rig);
  ga_fn2_write(rig, 0x8D);  // bit5 clear, mode bits = 01
  hsync_pulse(rig);
  EXPECT_EQ(peek(rig).mode, 1) << "0x8D must set mode 1 (bit5 clear = classic MRER)";
  ga_fn2_write(rig, 0x8C);  // bit5 clear, mode bits = 00
  hsync_pulse(rig);
  EXPECT_EQ(peek(rig).mode, 0) << "0x8C must set mode 0";
}

// THE REGRESSION: with the register page unlocked, a bit5-SET fn-2 write is RMR2
// and must NOT change the screen mode.
TEST(PlusRmr2Mode, Rmr2WriteDoesNotClobberMode) {
  Rig rig;
  make_rig(rig);
  unlock_asic(rig);
  ga_fn2_write(rig, 0x8D);  // establish mode 1 (the fence band)
  hsync_pulse(rig);
  ASSERT_EQ(peek(rig).mode, 1);

  ga_fn2_write(rig, 0xB8);  // bit5 set → RMR2, mode bits = 00, must be ignored
  hsync_pulse(rig);
  EXPECT_EQ(peek(rig).mode, 1)
      << "RMR2 write (0xB8, bit5 set) must NOT change the screen mode";

  ga_fn2_write(rig, 0xB0);  // another RMR2 flavour
  hsync_pulse(rig);
  EXPECT_EQ(peek(rig).mode, 1)
      << "RMR2 write (0xB0, bit5 set) must NOT change the screen mode";
}

// RMR2 writes carry bit4 (0x10); on the classic path that rearms the raster
// interrupt (resets sl_count). As RMR2 they must leave the counter alone.
TEST(PlusRmr2Mode, Rmr2WriteDoesNotRearmRasterCounter) {
  Rig rig;
  make_rig(rig);
  unlock_asic(rig);
  // Advance the GA raster counter with a few HSYNCs.
  for (int i = 0; i < 5; ++i) hsync_pulse(rig);
  const uint8_t before = peek(rig).sl_count;
  ASSERT_GT(before, 0u);

  ga_fn2_write(rig, 0xB8);  // bit5 set (RMR2) + bit4 set (would rearm if classic)
  EXPECT_EQ(peek(rig).sl_count, before)
      << "an RMR2 write must not reset the raster-interrupt counter";
}

// When the ASIC is LOCKED (or absent) a bit5-set fn-2 write falls back to the
// classic MRER behaviour and DOES set the mode (legacy: the RMR2 branch is
// gated on !asic.locked).
TEST(PlusRmr2Mode, LockedAsicTreatsBit5WriteAsClassicMode) {
  Rig rig;
  make_rig(rig);
  asic_set_plugged(&rig.asic, 1);  // plugged but still LOCKED (no knock)
  ga_fn2_write(rig, 0x8D);
  hsync_pulse(rig);
  ASSERT_EQ(peek(rig).mode, 1);
  ga_fn2_write(rig, 0xB0);  // bit5 set, but locked → classic → mode bits = 00
  hsync_pulse(rig);
  EXPECT_EQ(peek(rig).mode, 0)
      << "while locked, a bit5-set write is a classic mode write (mode 0)";
}
