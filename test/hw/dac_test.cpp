/* dac_test.cpp — the printer port latch and the AmDrum DAC Devices:
 * partial decode, edge semantics, strobe-clocked byte capture, the plugged
 * gate, and the Machine-level crafted-write path + Digiblaster mix. See
 * docs/hardware/printer-device.md and docs/hardware/amdrum-device.md. */

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "hw/amdrum.h"
#include "hw/board.h"
#include "hw/printer.h"
#include "subcycle/machine.h"

namespace {

struct DacRig {
  std::vector<uint8_t> mem;
  Board board;
  Device dev;
};

void make_rig(DacRig& rig, size_t state_size, Device (*init)(void*)) {
  rig.mem.assign(state_size, 0);
  rig.dev = init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
}

void io_write(DacRig& rig, uint16_t addr, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = addr;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
  rig.board.bus = bus_resting();  // deassert: the next access has an edge
  board_tick(&rig.board);
}

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

}  // namespace

TEST(PrinterLatch, DecodesA12LowAndInvertsStrobe) {
  DacRig rig;
  make_rig(rig, printer_state_size(), printer_init);
  PrinterRegs pr{};
  printer_peek(&rig.dev, &pr);
  EXPECT_EQ(pr.latch, 0xFF) << "reset value per the golden master";

  io_write(rig, 0xEF7E, 0x41);  // A12 low: selected
  printer_peek(&rig.dev, &pr);
  EXPECT_EQ(pr.latch, 0x41 ^ 0x80) << "bit 7 (/STROBE) is inverted";

  io_write(rig, 0xFF7E, 0x00);  // A12 high: not the printer
  printer_peek(&rig.dev, &pr);
  EXPECT_EQ(pr.latch, 0x41 ^ 0x80) << "unselected write left the latch alone";
}

TEST(PrinterLatch, StrobeFallingEdgeClocksBytesIntoTheRing) {
  DacRig rig;
  make_rig(rig, printer_state_size(), printer_init);

  // The firmware's byte handshake: present data with /STROBE high (latched
  // bit 7 set means val bit 7 CLEAR pre-inversion), then pulse it low.
  io_write(rig, 0xEF7E, 0x41);         // 'A' + strobe idle (latch bit 7 = 1)
  io_write(rig, 0xEF7E, 0x41 | 0x80);  // strobe asserted: falling edge
  io_write(rig, 0xEF7E, 0x41);         // strobe released
  io_write(rig, 0xEF7E, 0x42);         // 'B' presented
  io_write(rig, 0xEF7E, 0x42 | 0x80);  // clocked
  io_write(rig, 0xEF7E, 0x42);

  PrinterEvent ev[8];
  const int n = printer_drain_events(&rig.dev, ev, 8);
  ASSERT_EQ(n, 2) << "one byte per strobe pulse";
  EXPECT_EQ(ev[0].byte, 0x41);
  EXPECT_EQ(ev[1].byte, 0x42);
  EXPECT_LT(ev[0].cycle, ev[1].cycle) << "timestamps follow the clock";
  EXPECT_EQ(printer_drain_events(&rig.dev, ev, 8), 0) << "ring drained";
}

TEST(Amdrum, LatchesOnlyWhenPluggedOnTheUncontestedSpace) {
  DacRig rig;
  make_rig(rig, amdrum_state_size(), amdrum_init);
  AmdrumRegs ar{};
  amdrum_peek(&rig.dev, &ar);
  EXPECT_EQ(ar.dac, 128) << "mid-scale silence at reset";
  EXPECT_EQ(ar.plugged, 0);

  io_write(rig, 0xFF00, 0x20);
  amdrum_peek(&rig.dev, &ar);
  EXPECT_EQ(ar.dac, 128) << "unplugged: the space is empty";

  amdrum_set_plugged(&rig.dev, 1);
  io_write(rig, 0xFF12, 0x20);  // any &FFxx address decodes
  amdrum_peek(&rig.dev, &ar);
  EXPECT_EQ(ar.dac, 0x20);

  io_write(rig, 0xFE00, 0x99);  // upper bits not all high: not decoded
  amdrum_peek(&rig.dev, &ar);
  EXPECT_EQ(ar.dac, 0x20);

  // A CPC reset silences the DAC but does not eject the expansion.
  board_reset(&rig.board);
  amdrum_peek(&rig.dev, &ar);
  EXPECT_EQ(ar.dac, 128);
  EXPECT_EQ(ar.plugged, 1);
}

TEST(MachineDacs, CraftedWritesReachTheLatchesAndTheMixHearsThem) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  for (int i = 0; i < 3; ++i) m.run_frame();

  m.io_write(0xEF7E, 0x55);  // printer latch via the crafted-cycle path
  PrinterRegs pr{};
  printer_peek(m.printer(), &pr);
  EXPECT_EQ(pr.latch, 0x55 ^ 0x80);

  m.set_amdrum(true);
  m.io_write(0xFF00, 0xF0);
  AmdrumRegs ar{};
  amdrum_peek(m.amdrum(), &ar);
  EXPECT_EQ(ar.dac, 0xF0);

  // The analog mix: a hard-off DAC level must shift the DC-blocked output
  // audibly for a moment. Compare the first frame's peak with/without.
  auto peak_frame = [&m] {
    m.run_frame();
    int peak = 0;
    for (int16_t s : m.audio()) peak = std::max<int>(peak, s < 0 ? -s : s);
    return peak;
  };
  m.set_amdrum(false);
  m.set_digiblaster(false);
  for (int i = 0; i < 5; ++i) m.run_frame();  // settle the DC filter
  const int quiet = peak_frame();
  m.set_digiblaster(true);
  m.io_write(0xEF7E, 0x7F);  // latch 0xFF: full-scale DAC step
  const int loud = peak_frame();
  EXPECT_GT(loud, quiet) << "the Digiblaster step reached the audio output";

  // Independent ground truth: the Digiblaster/AmDrum are LINEAR 8-bit DACs
  // (real R-2R ladders; the legacy Level_PP[]/Level_AmDrum[] curves are linear,
  // and the clean-room mixes (latch-128)*31/128 — printer-device.md §3). A
  // linear DAC means the transient a step produces through the DC-blocking
  // filter is PROPORTIONAL to the step size. Verify the ratio rather than
  // restating the formula: a step of 127 vs 64 (from the 128 mid-point) must
  // land near 127/64 ≈ 1.98× — a log or squared curve would miss the window.
  auto step_peak = [&](uint8_t latch_target) {
    m.io_write(0xEF7E, 0x00);              // latch 0x80 = 128: DAC silence
    for (int i = 0; i < 8; ++i) m.run_frame();  // let the DC filter settle
    m.io_write(0xEF7E, latch_target ^ 0x80);    // the step under test
    return peak_frame();
  };
  const int p64 = step_peak(192);   // Δ = 64 from mid-scale
  const int p127 = step_peak(255);  // Δ = 127 from mid-scale
  ASSERT_GT(p64, 0) << "a mid-size step is audible";
  EXPECT_GT(p127 * 2, p64 * 3) << "ratio > 1.5: bigger step, bigger transient";
  EXPECT_LT(p127 * 2, p64 * 5) << "ratio < 2.5: the DAC is linear, not a curve";
  EXPECT_GT(step_peak(255), step_peak(128) + 4)
      << "a step to mid-scale (no swing) stays quiet; a full step does not";
}
