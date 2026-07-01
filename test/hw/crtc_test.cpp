/* crtc_test.cpp — the CRTC character-timing engine against the standard CPC screen.
 * See docs/hardware/crtc-device.md §6. clk.crtc is driven every tick (1 char/tick). */

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/z80.h"

namespace {

struct CrtcRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(crtc_state_size());
  Board board;
  Device dev;
};

void make_crtc(CrtcRig& rig) {
  rig.dev = crtc_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
  // Standard CPC register set (docs/hardware/crtc-device.md §2).
  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&rig.dev, i, std_regs[i]);
  crtc_poke_reg(&rig.dev, 12, 0x30);
  crtc_poke_reg(&rig.dev, 13, 0x00);
}

CrtcRegs crtc_char_tick(CrtcRig& rig) {
  rig.board.bus = bus_resting();
  rig.board.bus.clk.crtc = true;
  board_tick(&rig.board);
  CrtcRegs r{};
  crtc_peek(&rig.dev, &r);
  return r;
}

}  // namespace

TEST(Crtc, StandardScreenTiming) {
  CrtcRig rig;
  make_crtc(rig);

  int hsync_edges = 0, vsync_edges = 0;
  int line_len = 0, chars_since_hs = 0;
  int frame_len = 0, sl_since_vs = 0;  // scanlines between VSYNC rising edges
  int hsync_width = 0, max_hsync_width = 0;
  uint8_t prev_hs = 0, prev_vs = 0;
  bool measured_line = false, measured_frame = false;

  for (int i = 0; i < 60000; ++i) {
    CrtcRegs r = crtc_char_tick(rig);
    chars_since_hs++;
    if (r.hsync) hsync_width++;

    if (r.hsync && !prev_hs) {  // HSYNC rising edge = a scanline boundary
      if (hsync_edges > 0) { line_len = chars_since_hs; measured_line = true; }
      chars_since_hs = 0;
      hsync_edges++;
      sl_since_vs++;
    }
    if (!r.hsync && prev_hs) {  // HSYNC falling: record the pulse width
      max_hsync_width = hsync_width;
      hsync_width = 0;
    }

    if (r.vsync && !prev_vs) {  // VSYNC rising edge = a frame boundary
      if (vsync_edges > 0) { frame_len = sl_since_vs; measured_frame = true; }
      sl_since_vs = 0;
      vsync_edges++;
    }

    prev_hs = r.hsync;
    prev_vs = r.vsync;
  }

  ASSERT_TRUE(measured_line) << "saw at least two HSYNCs";
  ASSERT_TRUE(measured_frame) << "saw at least two VSYNCs";
  EXPECT_EQ(line_len, 64) << "one scanline = R0+1 = 64 chars = 64 µs";
  EXPECT_EQ(frame_len, 312) << "one frame = (R4+1)*(R9+1) = 39*8 = 312 → 50 Hz";
  EXPECT_EQ(max_hsync_width, 14) << "HSYNC width = R3 low nibble = 14 chars";
  EXPECT_GT(vsync_edges, 1);
}

TEST(Crtc, VsyncWidthAndPosition) {
  CrtcRig rig;
  make_crtc(rig);
  // Count the scanlines VSYNC stays asserted (rising to falling), and confirm it
  // is exactly R3>>4 = 8. Sample by counting HSYNC edges while VSYNC is high.
  uint8_t prev_vs = 0, prev_hs = 0;
  int vs_scanlines = 0, measured = -1;
  bool in_vs = false;
  for (int i = 0; i < 60000 && measured < 0; ++i) {
    CrtcRegs r = crtc_char_tick(rig);
    if (r.vsync && !prev_vs) { in_vs = true; vs_scanlines = 0; }
    if (in_vs && r.hsync && !prev_hs) vs_scanlines++;  // HSYNC per scanline
    if (!r.vsync && prev_vs) { if (in_vs) measured = vs_scanlines; in_vs = false; }
    prev_vs = r.vsync;
    prev_hs = r.hsync;
  }
  // VSYNC spans 8 scanlines; the HSYNC-edge count within it is 7 or 8 depending on
  // phase alignment of the first sync — assert it is in that band, not zero.
  EXPECT_GE(measured, 7);
  EXPECT_LE(measured, 8);
}

// ---- CRTC → GA → Z80: real CPC frame timing (50 Hz screen → 300 Hz interrupts) ----

namespace {
struct CRam { uint8_t cells[0x10000]; };
void cram_tick(void* self, const Bus* in, Bus* out) {
  CRam* m = static_cast<CRam*>(self);
  if (in->cpu.mreq && in->cpu.wr) m->cells[in->cpu.addr] = in->cpu.data;
  else if (in->cpu.mreq && in->cpu.rd) out->cpu.data = m->cells[in->cpu.addr];
}
size_t cram_size(const void*) { return sizeof(CRam); }
void cram_save(const void* s, void* b) { std::memcpy(b, s, sizeof(CRam)); }
void cram_load(void* s, const void* b) { std::memcpy(s, b, sizeof(CRam)); }
Device cram_device(CRam* s) {
  return Device{s, "ram", cram_tick, [](void*){}, cram_size, cram_save, cram_load};
}
}  // namespace

TEST(Crtc, RealFrameDrives300HzInterrupts) {
  auto ram = std::make_unique<CRam>();
  std::memset(ram->cells, 0, sizeof(CRam::cells));
  // IM 1 ; EI ; JR -2 (loop) ; handler@0x38: EI ; RET (keeps accepting interrupts)
  const uint8_t prog[] = {0xED, 0x56, 0xFB, 0x18, 0xFE};
  std::memcpy(ram->cells, prog, sizeof(prog));
  ram->cells[0x38] = 0xFB; ram->cells[0x39] = 0xC9;

  std::vector<uint8_t> gmem(ga_state_size());   Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size()); Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> zmem(z80_state_size());  Device zdev = z80_init(zmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);              // clk fabric + INT line
  board_add(&board, cdev);              // consumes clk.crtc, drives vid.hsync/vsync
  board_add(&board, cram_device(ram.get()));
  board_add(&board, zdev);
  board_reset(&board);
  // Program the CRTC AFTER reset (reset zeroes the registers).
  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&cdev, i, std_regs[i]);
  crtc_poke_reg(&cdev, 12, 0x30);

  // Count GA interrupt firings across exactly one CRTC frame (VSYNC to VSYNC).
  GateArrayRegs g{};
  CrtcRegs cr{};
  uint8_t prev_irq = 0, prev_vs = 0;
  int vsync_edges = 0, ga_ints = 0;
  bool counting = false;
  for (int tick = 0; tick < 700000; ++tick) {  // ~2 frames (frame ≈ 320k master cycles)
    board_tick(&board);
    ga_peek(&gdev, &g);
    crtc_peek(&cdev, &cr);
    if (cr.vsync && !prev_vs) {
      vsync_edges++;
      if (vsync_edges == 1) counting = true;    // frame start
      else if (vsync_edges == 2) break;         // one full frame counted
    }
    if (counting && g.irq && !prev_irq) ga_ints++;
    prev_irq = g.irq;
    prev_vs = cr.vsync;
  }
  ASSERT_EQ(vsync_edges, 2) << "observed one full CRTC frame";
  EXPECT_EQ(ga_ints, 6) << "312 scanlines / 52 = 6 raster interrupts per 50 Hz frame = 300 Hz";
}
