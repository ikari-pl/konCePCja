/* gate_array_test.cpp — the Gate Array Device: clock division + raster
 * interrupt. See docs/hardware/gate-array-device.md. HSYNC/VSYNC are driven
 * synthetically (the real CRTC is a later component). */

#include "hw/gate_array.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "hw/board.h"
#include "hw/z80.h"

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

// One complete HSYNC pulse. Returns the post-fall (HSYNC-end) snapshot.
GateArrayRegs hsync_pulse(GaRig& rig) {
  ga_step(rig, StepIn{/*hsync=*/true});
  return ga_step(rig, StepIn{/*hsync=*/false});
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
  EXPECT_EQ(cpu, 4)
      << "clk.cpu is ÷4 (4 MHz): 4 enables per 16-cycle µs window";
  EXPECT_EQ(crtc, 1) << "clk.crtc is ÷16 (1 MHz): 1 enable per window";
}

TEST(GateArray, RasterCounterAdvancesOnHsyncFallingEdge) {
  GaRig rig;
  make_rig(rig);

  const GateArrayRegs at_rise = ga_step(rig, StepIn{.hsync = true});
  EXPECT_EQ(at_rise.sl_count, 0)
      << "the hardware R52 counter clocks at HSYNC end, not HSYNC start";

  const GateArrayRegs at_fall = ga_step(rig, StepIn{.hsync = false});
  EXPECT_EQ(at_fall.sl_count, 1)
      << "one completed HSYNC pulse advances the raster counter once";
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
  for (int i = 0; i < 40; ++i)
    hsync_pulse(rig);  // sl_count = 40 (0x28, bit5 set)
  EXPECT_EQ(ga_step(rig, StepIn{}).sl_count, 40);
  GateArrayRegs masked = ga_step(rig, StepIn{.m1 = true, .iorq = true});
  EXPECT_EQ(masked.sl_count, 40 & 0x1F)
      << "acknowledge clears bit 5 (0x28 → 0x08)";
}

TEST(GateArray, VsyncResyncsCounter) {
  GaRig rig;
  make_rig(rig);
  for (int i = 0; i < 10; ++i) hsync_pulse(rig);
  EXPECT_EQ(ga_step(rig, StepIn{}).sl_count, 10);
  ga_step(
      rig,
      StepIn{.vsync = true});  // VSYNC rising edge arms the resync (hs_count=2)
  ga_step(rig, StepIn{.vsync = false});
  hsync_pulse(rig);                         // 1st HSYNC after VSYNC
  GateArrayRegs after2 = hsync_pulse(rig);  // 2nd HSYNC → resync
  EXPECT_EQ(after2.sl_count, 0)
      << "counter resyncs to 0 two HSYNCs after VSYNC";
}

// The VSYNC resync's save-margin: if the interrupt counter has already reached
// >=32 when the 2-HSYNC resync lands, an interrupt fires then too — so a due
// interrupt near the VSYNC boundary is not skipped (gate-array-device.md §4).
TEST(GateArray, VsyncResyncFiresSaveMarginWhenCounterPast32) {
  GaRig rig;
  make_rig(rig);
  for (int i = 0; i < 40; ++i) hsync_pulse(rig);  // counter to 40 (>=32, <52)
  EXPECT_EQ(ga_step(rig, StepIn{}).sl_count, 40) << "counter at 40, no fire yet";
  ga_step(rig, StepIn{.vsync = true});   // VSYNC rising edge arms the resync
  ga_step(rig, StepIn{.vsync = false});
  hsync_pulse(rig);                          // 1st HSYNC after VSYNC
  GateArrayRegs after2 = hsync_pulse(rig);   // 2nd HSYNC → resync + save-margin
  EXPECT_EQ(after2.sl_count, 0) << "counter still resyncs to 0";
  EXPECT_EQ(after2.irq, 1)
      << "save-margin: counter was >=32 at the resync, so an interrupt fires";
}

TEST(GateArray, ModeRegisterBit4Rearms) {
  GaRig rig;
  make_rig(rig);
  for (int i = 0; i < 52; ++i) hsync_pulse(rig);  // fire
  EXPECT_EQ(ga_step(rig, StepIn{}).irq, 1);
  // I/O write to the GA (A15=0,A14=1), mode register (data>>6==2), bit4 set.
  GateArrayRegs r = ga_step(
      rig, StepIn{.iorq = true, .wr = true, .addr = 0x7F00, .data = 0x90});
  EXPECT_EQ(r.irq, 0) << "mode-register bit 4 rearms/clears the interrupt";
  EXPECT_EQ(r.sl_count, 0);
}

TEST(GateArray, RegisterSide) {
  GaRig rig;
  make_rig(rig);
  auto io = [&](uint8_t data) {
    return ga_step(
        rig, StepIn{.iorq = true, .wr = true, .addr = 0x7F00, .data = data});
  };
  // Pen select (fn 0, pen 5), then set ink (fn 1, colour 12) → ink[5] = 12.
  io(0x05);
  GateArrayRegs a = io(0x40 | 12);
  EXPECT_EQ(a.pen, 5);
  EXPECT_EQ(a.ink[5], 12);
  // Border select (fn 0, bit 4) then ink 3 → ink[16] (border) = 3.
  io(0x10);
  GateArrayRegs b = io(0x40 | 3);
  EXPECT_EQ(b.pen, 16);
  EXPECT_EQ(b.ink[16], 3);
  // Mode (fn 2, mode 2) is requested immediately, latched at the next HSYNC.
  GateArrayRegs m = io(0x80 | 2);
  EXPECT_EQ(m.req_mode, 2);
  EXPECT_NE(m.mode, 2) << "mode not applied until HSYNC";
  GateArrayRegs after_hs = hsync_pulse(rig);
  EXPECT_EQ(after_hs.mode, 2) << "mode latched at the start of the line";
  // ROM enable bits ride in the mode register; RAM banking is function 3.
  GateArrayRegs rom = io(0x80 | 0x0C);  // upper+lower ROM disabled (bits 2,3)
  EXPECT_EQ(rom.rom_config & 0x0C, 0x0C);
  GateArrayRegs ram = io(0xC0 | 0x03);  // RAM config
  EXPECT_EQ(ram.ram_config & 0x3F, 0x03);
}

// The raster-split mechanism: the screen mode register is requested immediately
// but the ACTIVE mode only latches at each HSYNC (the start of a scanline). A
// handler that rewrites the mode mid-scanline therefore paints the CURRENT line
// in the old mode and the NEXT line in the new one — exactly how a game builds
// horizontal mode bands (e.g. the Burnin' Rubber title: MODE-0 logo/fence rows,
// MODE-1 "PRESS FIRE" rows). This is the Device-level invariant the old
// cartridge black-box "mode bands" test was really checking.
TEST(GateArray, ModeLatchesPerScanlineForRasterSplit) {
  GaRig rig;
  make_rig(rig);
  auto io = [&](uint8_t data) {
    return ga_step(
        rig, StepIn{.iorq = true, .wr = true, .addr = 0x7F00, .data = data});
  };

  // Band 1: select MODE 0 and latch it in with an HSYNC (top of a scanline).
  io(0x80 | 0);            // fn 2, mode 0
  GateArrayRegs a = hsync_pulse(rig);
  EXPECT_EQ(a.mode, 0) << "MODE 0 latched at the scanline start";

  // Mid-scanline (no HSYNC yet) request MODE 1: it is only REQUESTED — the
  // active mode must stay 0 so THIS scanline finishes painting in mode 0.
  GateArrayRegs b = io(0x80 | 1);  // fn 2, mode 1
  EXPECT_EQ(b.req_mode, 1);
  EXPECT_EQ(b.mode, 0) << "the mode change must not take effect mid-scanline";
  for (int i = 0; i < 8; ++i) {
    GateArrayRegs mid = ga_step(rig, StepIn{});  // more cycles, still no HSYNC
    EXPECT_EQ(mid.mode, 0) << "active mode holds until the next HSYNC";
  }

  // Band 2: the next HSYNC latches MODE 1 — the split takes effect exactly one
  // scanline after the write, not before and not later.
  GateArrayRegs c = hsync_pulse(rig);
  EXPECT_EQ(c.mode, 1) << "MODE 1 latches at the next scanline boundary";

  // And a mid-line switch back to MODE 0 again defers to the following HSYNC.
  GateArrayRegs d = io(0x80 | 0);
  EXPECT_EQ(d.mode, 1) << "still mode 1 until the boundary";
  GateArrayRegs e = hsync_pulse(rig);
  EXPECT_EQ(e.mode, 0) << "back to MODE 0 on the next scanline";
}

// ---- End-to-end: GA fires the raster INT → Z80 accepts → GA sees the IORQ ack
// ----

namespace {
struct Ram2 {
  uint8_t cells[0x10000];
};
void r2_tick(void* self, const Bus* in, Bus* out) {
  Ram2* m = static_cast<Ram2*>(self);
  if (in->cpu.mreq && in->cpu.wr)
    m->cells[in->cpu.addr] = in->cpu.data;
  else if (in->cpu.mreq && in->cpu.rd)
    out->cpu.data = m->cells[in->cpu.addr];
}
size_t r2_size(const void*) { return sizeof(Ram2); }
void r2_save(const void* s, void* b) { std::memcpy(b, s, sizeof(Ram2)); }
void r2_load(void* s, const void* b) { std::memcpy(s, b, sizeof(Ram2)); }
Device r2_device(Ram2* s) {
  return Device{s, "ram", r2_tick, [](void*) {}, r2_size, r2_save, r2_load};
}
// Fast synthetic HSYNC: a rising edge every 40 master cycles (period irrelevant
// to the GA, which counts edges — this just reaches 52 lines quickly).
struct HsyncGen {
  uint32_t ctr;
};
void hg_tick(void* self, const Bus*, Bus* out) {
  HsyncGen* h = static_cast<HsyncGen*>(self);
  out->vid.hsync = (h->ctr % 40) < 4;
  h->ctr++;
}
size_t hg_size(const void*) { return sizeof(HsyncGen); }
void hg_save(const void* s, void* b) { std::memcpy(b, s, sizeof(HsyncGen)); }
void hg_load(void* s, const void* b) { std::memcpy(s, b, sizeof(HsyncGen)); }
Device hg_device(HsyncGen* s) {
  return Device{s, "hsync", hg_tick, [](void*) {}, hg_size, hg_save, hg_load};
}
}  // namespace

TEST(GateArray, EndToEndInterruptAcceptedAndAcknowledged) {
  auto ram = std::make_unique<Ram2>();
  std::memset(ram->cells, 0, sizeof(Ram2::cells));
  // IM 1 ; EI ; JR -2 (loop) ; handler@0x38: LD A,0x42 ; HALT
  const uint8_t prog[] = {0xED, 0x56, 0xFB, 0x18, 0xFE};
  std::memcpy(ram->cells, prog, sizeof(prog));
  ram->cells[0x38] = 0x3E;
  ram->cells[0x39] = 0x42;
  ram->cells[0x3A] = 0x76;

  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  HsyncGen hg{0};
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);            // drives clk + irq
  board_add(&board, hg_device(&hg));  // drives vid.hsync
  board_add(&board, r2_device(ram.get()));
  board_add(&board, zdev);
  board_reset(&board);

  Z80Regs r{};
  GateArrayRegs g{};
  bool ga_fired = false;
  for (int tick = 0; tick < 200000; ++tick) {
    board_tick(&board);
    ga_peek(&gdev, &g);
    if (g.irq) ga_fired = true;
    z80_peek(&zdev, &r);
    if (r.halted) break;
  }
  EXPECT_TRUE(ga_fired) << "the GA raised the raster interrupt";
  EXPECT_EQ(r.halted, 1) << "the Z80 accepted it and reached the handler";
  EXPECT_EQ(static_cast<uint8_t>(r.af >> 8), 0x42) << "handler ran (LD A,0x42)";
  EXPECT_EQ(g.irq, 0)
      << "the GA cleared its INT line on seeing the Z80's IORQ ack";
  EXPECT_EQ(r.iff1, 0) << "acceptance cleared IFF1";
}

// ---- Video fetch slots (bus DMA): the GA owns the RAM address multiplexer
// ----

#include "hw/video.h"  // vid_byte_addr — the GA's MA/RA → RAM address shuffle

namespace {
// Holds MA/RA on the vid bus (stands in for a configured CRTC).
struct MaDriver {
  uint16_t ma;
  uint8_t ra;
};
void ma_tick(void* self, const Bus*, Bus* out) {
  const MaDriver* d = static_cast<const MaDriver*>(self);
  out->vid.ma = d->ma;
  out->vid.ra = d->ra;
}
size_t ma_size(const void*) { return sizeof(MaDriver); }
Device ma_device(MaDriver* d) {
  return Device{d,
                "ma-driver",
                ma_tick,
                [](void*) {},
                ma_size,
                [](const void*, void*) {},
                [](void*, const void*) {}};
}
}  // namespace

TEST(GateArray, VideoFetchSlotsDriveTheRamBus) {
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  MaDriver drv{0x3000, 2};  // standard screen base, scanline 2

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, ma_device(&drv));
  board_reset(&board);

  int fetches = 0;
  for (int tick = 0; tick < 64; ++tick) {  // 4 full µs windows
    board_tick(&board);
    if (!board.bus.ram.fetch) continue;
    fetches++;
    const uint8_t ph = board.bus.clk.phase;
    EXPECT_TRUE(ph == 12 || ph == 14)
        << "fetches happen only in the video slots";
    if (tick < 16) continue;  // first µs: the driver's MA is still propagating
    const uint8_t k = (ph == 14) ? 1 : 0;
    EXPECT_EQ(board.bus.ram.addr, vid_byte_addr(0x3000, 2, k))
        << "the GA drives the MA/RA-shuffled RAM address (byte " << int(k)
        << ")";
  }
  EXPECT_EQ(fetches, 8) << "exactly two video fetches per µs, every µs";
}
