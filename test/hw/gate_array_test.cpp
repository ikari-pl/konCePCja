/* gate_array_test.cpp — the Gate Array Device: clock division + raster interrupt.
 * See docs/hardware/gate-array-device.md. HSYNC/VSYNC are driven synthetically
 * (the real CRTC is a later component). */

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "hw/board.h"
#include "hw/gate_array.h"

namespace {

/* A GA-only board. Each step seeds the committed bus (the GA's `in`) with the
 * requested video/CPU lines, ticks once, and returns the GA snapshot. */
struct GaRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(ga_state_size());
  Board board;
  Device dev;
};

struct StepIn {
  bool hsync = false, vsync = false;
  bool m1 = false, iorq = false, wr = false;
  uint16_t addr = 0;
  uint8_t data = 0xFF;
};

GateArrayRegs ga_step(GaRig& rig, const StepIn& s) {
  rig.board.bus = bus_resting();
  rig.board.bus.vid.hsync = s.hsync;
  rig.board.bus.vid.vsync = s.vsync;
  rig.board.bus.cpu.m1 = s.m1;
  rig.board.bus.cpu.iorq = s.iorq;
  rig.board.bus.cpu.wr = s.wr;
  rig.board.bus.cpu.addr = s.addr;
  rig.board.bus.cpu.data = s.data;
  board_tick(&rig.board);
  GateArrayRegs r{};
  ga_peek(&rig.dev, &r);
  return r;
}

void make_rig(GaRig& rig) {
  rig.dev = ga_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
}

// One HSYNC rising edge (then release). Returns the post-rise snapshot.
GateArrayRegs hsync_pulse(GaRig& rig) {
  GateArrayRegs r = ga_step(rig, StepIn{/*hsync=*/true});
  ga_step(rig, StepIn{/*hsync=*/false});
  return r;
}

}  // namespace

TEST(GateArray, ClockDivision) {
  GaRig rig;
  make_rig(rig);
  int cpu = 0, crtc = 0;
  for (int i = 0; i < 16; ++i) {
    rig.board.bus = bus_resting();
    board_tick(&rig.board);
    if (rig.board.bus.clk.cpu) cpu++;
    if (rig.board.bus.clk.crtc) crtc++;
  }
  EXPECT_EQ(cpu, 4) << "clk.cpu is ÷4 (4 MHz): 4 enables per 16-cycle µs window";
  EXPECT_EQ(crtc, 1) << "clk.crtc is ÷16 (1 MHz): 1 enable per window";
}

TEST(GateArray, RasterInterruptFiresAt52) {
  GaRig rig;
  make_rig(rig);
  for (int i = 0; i < 51; ++i) hsync_pulse(rig);
  GateArrayRegs before = ga_step(rig, StepIn{});
  EXPECT_EQ(before.irq, 0) << "no interrupt before the 52nd HSYNC";
  EXPECT_EQ(before.sl_count, 51);
  GateArrayRegs at52 = hsync_pulse(rig);
  EXPECT_EQ(at52.irq, 1) << "INT asserted at HSYNC line 52 (~300 Hz)";
  EXPECT_EQ(at52.sl_count, 0) << "line counter resets after firing";
}

TEST(GateArray, AcknowledgeMasksBit5AndClearsLine) {
  GaRig rig;
  make_rig(rig);
  // Fire the interrupt (52 HSYNCs) so the INT line is asserted.
  for (int i = 0; i < 52; ++i) hsync_pulse(rig);
  EXPECT_EQ(ga_step(rig, StepIn{}).irq, 1);
  // The CPU's M1+IORQ acknowledge cycle drops the line and masks bit 5.
  GateArrayRegs acked = ga_step(rig, StepIn{.m1 = true, .iorq = true});
  EXPECT_EQ(acked.irq, 0) << "acknowledge drops the INT line";
  // A high line counter (bit 5 set) is masked to 5 bits on acknowledge.
  for (int i = 0; i < 40; ++i) hsync_pulse(rig);  // sl_count = 40 (0x28, bit5 set)
  EXPECT_EQ(ga_step(rig, StepIn{}).sl_count, 40);
  GateArrayRegs masked = ga_step(rig, StepIn{.m1 = true, .iorq = true});
  EXPECT_EQ(masked.sl_count, 40 & 0x1F) << "acknowledge clears bit 5 (0x28 → 0x08)";
}

TEST(GateArray, VsyncResyncsCounter) {
  GaRig rig;
  make_rig(rig);
  for (int i = 0; i < 10; ++i) hsync_pulse(rig);
  EXPECT_EQ(ga_step(rig, StepIn{}).sl_count, 10);
  ga_step(rig, StepIn{.vsync = true});   // VSYNC rising edge arms the resync (hs_count=2)
  ga_step(rig, StepIn{.vsync = false});
  hsync_pulse(rig);                      // 1st HSYNC after VSYNC
  GateArrayRegs after2 = hsync_pulse(rig);  // 2nd HSYNC → resync
  EXPECT_EQ(after2.sl_count, 0) << "counter resyncs to 0 two HSYNCs after VSYNC";
}

TEST(GateArray, ModeRegisterBit4Rearms) {
  GaRig rig;
  make_rig(rig);
  for (int i = 0; i < 52; ++i) hsync_pulse(rig);  // fire
  EXPECT_EQ(ga_step(rig, StepIn{}).irq, 1);
  // I/O write to the GA (A15=0,A14=1), mode register (data>>6==2), bit4 set.
  GateArrayRegs r = ga_step(rig, StepIn{.iorq = true, .wr = true, .addr = 0x7F00, .data = 0x90});
  EXPECT_EQ(r.irq, 0) << "mode-register bit 4 rearms/clears the interrupt";
  EXPECT_EQ(r.sl_count, 0);
}
