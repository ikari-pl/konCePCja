/* crtc_quirks_test.cpp — the CRTC's deep counter behaviours (quirk slice 1):
 * 8-bit HCC wrap (the mid-line R0-shrink stretch demos use for rupture), 5-bit
 * RA / 7-bit VCC wraps, per-register write masks, the R7-write immediate VSYNC
 * edge, and the type-1 row-0 start-address re-read. See
 * docs/hardware/crtc-device.md §7b. */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "hw/board.h"
#include "hw/crtc.h"

namespace {

void qclk_tick(void*, const Bus*, Bus* out) { out->clk.crtc = true; }
size_t qone_size(const void*) { return 1; }
Device qclk_device() {
  static uint8_t d = 0;
  return Device{&d,
                "clk",
                qclk_tick,
                [](void*) {},
                qone_size,
                [](const void*, void*) {},
                [](void*, const void*) {}};
}

struct Rig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(crtc_state_size());
  Board board;
  Device dev;
};

void make_crtc(Rig& rig, uint8_t type = 0) {
  rig.dev = crtc_init(rig.mem.data());
  crtc_set_type(&rig.dev, type);
  board_init(&rig.board);
  board_add(&rig.board, qclk_device());
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
  const uint8_t regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&rig.dev, i, regs[i]);
  crtc_poke_reg(&rig.dev, 12, 0x30);
}

void out_port(Rig& rig, uint16_t port,
              uint8_t val) {  // NOTE: each call ticks once
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = port;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
}
CrtcRegs peek(Rig& rig) {
  CrtcRegs r{};
  crtc_peek(&rig.dev, &r);
  return r;
}
// Tick until a predicate on the snapshot holds (bounded).
template <typename Pred>
bool run_until(Rig& rig, int max_ticks, Pred pred) {
  for (int i = 0; i < max_ticks; ++i) {
    board_tick(&rig.board);
    if (pred(peek(rig))) return true;
  }
  return false;
}

}  // namespace

TEST(CrtcQuirks, WriteMasksClampRegisterWidths) {
  Rig rig;
  make_crtc(rig);
  out_port(rig, 0xBC00, 4);
  out_port(rig, 0xBD00, 0xFF);
  out_port(rig, 0xBC00, 9);
  out_port(rig, 0xBD00, 0xFF);
  out_port(rig, 0xBC00, 12);
  out_port(rig, 0xBD00, 0xFF);
  const CrtcRegs r = peek(rig);
  EXPECT_EQ(r.reg[4], 0x7F) << "R4 is a 7-bit register";
  EXPECT_EQ(r.reg[9], 0x1F) << "R9 is a 5-bit register";
  EXPECT_EQ(r.reg[12], 0x3F) << "R12 is a 6-bit register";
}

TEST(CrtcQuirks, R0ShrinkMidLineStretchesToThe8BitWrap) {
  Rig rig;
  make_crtc(rig);
  ASSERT_TRUE(
      run_until(rig, 200, [](const CrtcRegs& r) { return r.hcc == 30; }));
  const CrtcRegs at = peek(rig);
  // Shrink R0 below the current count: the 8-bit counter must run 30→255, wrap,
  // and only end the line when it comes around to the new R0.
  crtc_poke_reg(&rig.dev, 0, 20);
  int ticks = 0;
  CrtcRegs r{};
  do {
    board_tick(&rig.board);
    ticks++;
    r = peek(rig);
    ASSERT_LE(ticks, 300) << "line must end at the wrap-around, not run away";
  } while (r.ra == at.ra && r.vcc == at.vcc);
  // From hcc=30: 226 ticks to wrap to 0, 20 more to reach hcc==20(==R0), then
  // the matching tick advances the scanline: 247 total.
  EXPECT_EQ(ticks, 247) << "stretched line = (256-30) + 21 characters";
  // Subsequent lines are the short R0=20 length (21 chars).
  const uint8_t ra_now = r.ra;
  ticks = 0;
  do {
    board_tick(&rig.board);
    ticks++;
    r = peek(rig);
  } while (r.ra == ra_now && ticks < 100);
  EXPECT_EQ(ticks, 21) << "following lines run at the new R0";
}

TEST(CrtcQuirks, R7WriteEqualToCurrentRowStartsVsyncImmediately) {
  Rig rig;
  make_crtc(rig);
  // Get well inside the frame, far from R7=30, VSYNC off.
  ASSERT_TRUE(run_until(rig, 20000, [](const CrtcRegs& r) {
    return r.vcc == 5 && r.ra == 2 && !r.vsync;
  }));
  out_port(rig, 0xBC00, 7);
  out_port(rig, 0xBD00, 5);  // R7 = the row being displayed RIGHT NOW
  EXPECT_EQ(peek(rig).vsync, 1) << "the write itself triggers the VSYNC edge";
}

TEST(CrtcQuirks, RaRunsToThe5BitWrapWhenR9ShrinksBelowIt) {
  Rig rig;
  make_crtc(rig);
  ASSERT_TRUE(run_until(
      rig, 20000, [](const CrtcRegs& r) { return r.vcc == 2 && r.ra == 5; }));
  crtc_poke_reg(&rig.dev, 9, 2);  // below the current raster count
  uint8_t max_ra = 0;
  for (int i = 0; i < 64 * 40; ++i) {  // up to 40 scanlines
    board_tick(&rig.board);
    max_ra = std::max(max_ra, peek(rig).ra);
    if (peek(rig).vcc != 2) break;  // row finally advanced
  }
  EXPECT_GE(max_ra, 31) << "the 5-bit raster counter ran through its wrap";
  EXPECT_EQ(peek(rig).vcc, 3)
      << "...and the row advanced after coming around to R9";
}

TEST(CrtcQuirks, VccRunsToThe7BitWrapWhenR4ShrinksBelowIt) {
  Rig rig;
  make_crtc(rig);
  ASSERT_TRUE(
      run_until(rig, 40000, [](const CrtcRegs& r) { return r.vcc == 10; }));
  crtc_poke_reg(&rig.dev, 4, 5);  // below the current row count
  uint8_t max_vcc = 0;
  for (int i = 0; i < 64 * 8 * 130; ++i) {  // up to ~130 char rows
    board_tick(&rig.board);
    const CrtcRegs r = peek(rig);
    max_vcc = std::max(max_vcc, r.vcc);
    if (max_vcc >= 100 && r.vcc < 3) break;  // wrapped and restarted
  }
  EXPECT_GE(max_vcc, 127) << "the 7-bit row counter ran through its wrap";
}

TEST(CrtcQuirks, Type1ReReadsStartAddressDuringRow0) {
  for (uint8_t type : {uint8_t{0}, uint8_t{1}}) {
    Rig rig;
    make_crtc(rig, type);
    // Let the first frame complete (so crtc_newframe has latched R12/R13 =
    // 0x3000), then land inside the NEXT frame's char row 0, a couple of
    // scanlines in.
    ASSERT_TRUE(
        run_until(rig, 40000, [](const CrtcRegs& r) { return r.vcc == 2; }));
    ASSERT_TRUE(run_until(
        rig, 90000, [](const CrtcRegs& r) { return r.vcc == 0 && r.ra == 1; }))
        << "type " << int(type);
    crtc_poke_reg(&rig.dev, 12, 0x10);  // move the start address mid-row-0
    // Two scanlines later, still in row 0:
    ASSERT_TRUE(run_until(
        rig, 300, [](const CrtcRegs& r) { return r.vcc == 0 && r.ra == 3; }))
        << "type " << int(type);
    const uint16_t page = static_cast<uint16_t>(peek(rig).ma & 0x3000);
    if (type == 1)
      EXPECT_EQ(page, 0x1000)
          << "type 1 re-reads R12/R13 every scanline of row 0";
    else
      EXPECT_EQ(page, 0x3000)
          << "type 0 latched the start address at frame start";
  }
}
