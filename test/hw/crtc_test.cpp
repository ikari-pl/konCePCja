/* crtc_test.cpp — the CRTC character-timing engine against the standard CPC
 * screen. See docs/hardware/crtc-device.md §6. clk.crtc is driven every tick (1
 * char/tick). */

#include "hw/crtc.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "hw/board.h"
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

// Per-cycle I/O WRITE tick: crtc_tick snoops an &BC/&BD write off the bus AND
// advances one char (clk.crtc) in the same tick — the decode runs before
// crtc_char, matching the batch order (crtc_fast_io_write then advance).
CrtcRegs crtc_io_write_tick(CrtcRig& rig, uint16_t port, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.clk.crtc = true;
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = port;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
  CrtcRegs r{};
  crtc_peek(&rig.dev, &r);
  return r;
}

// Per-cycle I/O READ tick: crtc_tick decodes the read and drives cpu.data when
// the type's readable window matches. clk.crtc stays LOW so the read does not
// advance the char — matching crtc_fast_io_read (decode only, no advance).
// Returns the post-tick committed bus data (0xFF when nothing drove it).
uint8_t crtc_io_read_bus(CrtcRig& rig, uint16_t port) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.addr = port;
  board_tick(&rig.board);
  return rig.board.bus.cpu.data;
}

// Full-state equality (every CrtcRegs field the batch and per-cycle paths can
// diverge on). Stronger than the existing oracle's output-only checks: a
// write that leaves an internal counter (hcc/vcc/hsw/vsw/vta/reg_select) in
// the wrong place surfaces here even before it propagates to ma/levels.
void expect_regs_eq(const CrtcRegs& a, const CrtcRegs& b, int at) {
  ASSERT_EQ(a.hcc, b.hcc) << "hcc @ " << at;
  ASSERT_EQ(a.ra, b.ra) << "ra @ " << at;
  ASSERT_EQ(a.vcc, b.vcc) << "vcc @ " << at;
  ASSERT_EQ(a.ma, b.ma) << "ma @ " << at;
  ASSERT_EQ(a.hsync, b.hsync) << "hsync @ " << at;
  ASSERT_EQ(a.vsync, b.vsync) << "vsync @ " << at;
  ASSERT_EQ(a.dispen, b.dispen) << "dispen @ " << at;
  ASSERT_EQ(a.scanline, b.scanline) << "scanline @ " << at;
  ASSERT_EQ(a.hsw, b.hsw) << "hsw @ " << at;
  ASSERT_EQ(a.vsw, b.vsw) << "vsw @ " << at;
  ASSERT_EQ(a.vta, b.vta) << "vta @ " << at;
  ASSERT_EQ(a.reg_select, b.reg_select) << "reg_select @ " << at;
  for (int i = 0; i < 18; ++i)
    ASSERT_EQ(a.reg[i], b.reg[i]) << "R" << i << " @ " << at;
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
      if (hsync_edges > 0) {
        line_len = chars_since_hs;
        measured_line = true;
      }
      chars_since_hs = 0;
      hsync_edges++;
      sl_since_vs++;
    }
    if (!r.hsync && prev_hs) {  // HSYNC falling: record the pulse width
      max_hsync_width = hsync_width;
      hsync_width = 0;
    }

    if (r.vsync && !prev_vs) {  // VSYNC rising edge = a frame boundary
      if (vsync_edges > 0) {
        frame_len = sl_since_vs;
        measured_frame = true;
      }
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
  // Count the scanlines VSYNC stays asserted (rising to falling), and confirm
  // it is exactly R3>>4 = 8. Sample by counting HSYNC edges while VSYNC is
  // high.
  uint8_t prev_vs = 0, prev_hs = 0;
  int vs_scanlines = 0, measured = -1;
  bool in_vs = false;
  for (int i = 0; i < 60000 && measured < 0; ++i) {
    CrtcRegs r = crtc_char_tick(rig);
    if (r.vsync && !prev_vs) {
      in_vs = true;
      vs_scanlines = 0;
    }
    if (in_vs && r.hsync && !prev_hs) vs_scanlines++;  // HSYNC per scanline
    if (!r.vsync && prev_vs) {
      if (in_vs) measured = vs_scanlines;
      in_vs = false;
    }
    prev_vs = r.vsync;
    prev_hs = r.hsync;
  }
  // VSYNC spans 8 scanlines; the HSYNC-edge count within it is 7 or 8 depending
  // on phase alignment of the first sync — assert it is in that band, not zero.
  EXPECT_GE(measured, 7);
  EXPECT_LE(measured, 8);
}

// ---- CRTC → GA → Z80: real CPC frame timing (50 Hz screen → 300 Hz
// interrupts) ----

namespace {
struct CRam {
  uint8_t cells[0x10000];
};
void cram_tick(void* self, const Bus* in, Bus* out) {
  CRam* m = static_cast<CRam*>(self);
  if (in->cpu.mreq && in->cpu.wr)
    m->cells[in->cpu.addr] = in->cpu.data;
  else if (in->cpu.mreq && in->cpu.rd)
    out->cpu.data = m->cells[in->cpu.addr];
}
size_t cram_size(const void*) { return sizeof(CRam); }
void cram_save(const void* s, void* b) { std::memcpy(b, s, sizeof(CRam)); }
void cram_load(void* s, const void* b) { std::memcpy(s, b, sizeof(CRam)); }
Device cram_device(CRam* s) {
  return Device{s,         "ram",     cram_tick, [](void*) {},
                cram_size, cram_save, cram_load};
}
}  // namespace

TEST(Crtc, RealFrameDrives300HzInterrupts) {
  auto ram = std::make_unique<CRam>();
  std::memset(ram->cells, 0, sizeof(CRam::cells));
  // IM 1 ; EI ; JR -2 (loop) ; handler@0x38: EI ; RET (keeps accepting
  // interrupts)
  const uint8_t prog[] = {0xED, 0x56, 0xFB, 0x18, 0xFE};
  std::memcpy(ram->cells, prog, sizeof(prog));
  ram->cells[0x38] = 0xFB;
  ram->cells[0x39] = 0xC9;

  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);  // clk fabric + INT line
  board_add(&board, cdev);  // consumes clk.crtc, drives vid.hsync/vsync
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
  for (int tick = 0; tick < 700000;
       ++tick) {  // ~2 frames (frame ≈ 320k master cycles)
    board_tick(&board);
    ga_peek(&gdev, &g);
    crtc_peek(&cdev, &cr);
    if (cr.vsync && !prev_vs) {
      vsync_edges++;
      if (vsync_edges == 1)
        counting = true;  // frame start
      else if (vsync_edges == 2)
        break;  // one full frame counted
    }
    if (counting && g.irq && !prev_irq) ga_ints++;
    prev_irq = g.irq;
    prev_vs = cr.vsync;
  }
  ASSERT_EQ(vsync_edges, 2) << "observed one full CRTC frame";
  EXPECT_EQ(ga_ints, 6)
      << "312 scanlines / 52 = 6 raster interrupts per 50 Hz frame = 300 Hz";
}

// The Fast scheduler's IRQ horizon contract (crtc.h): from any state, the
// h-1 characters before the returned horizon produce NO INT-path event — no
// HSYNC fall (the GA counter step) and no frame_line advance (the ASIC PRI
// reference and the VSYNC-rise char). The F8 horizon experiment died on a
// fencepost here (an exclusive sync-edge stop left the event char unpolled, and
// ISRs landed µs late — audio-lockstep-visible, framebuffer-blind); this
// replays the scheduler's exact poll pattern across two standard frames.
TEST(Crtc, IrqHorizonNeverSkipsAnIntPathEvent) {
  CrtcRig rig;
  make_crtc(rig);

  CrtcRegs cr{};
  crtc_peek(&rig.dev, &cr);
  uint16_t prev_line = cr.scanline;
  uint32_t remaining = crtc_irq_horizon_chars(&rig.dev);
  ASSERT_GE(remaining, 1u);

  int events = 0, polls = 0;
  for (int i = 0; i < 64 * 39 * 8 * 2; ++i) {  // two 312-line frames
    CrtcCharView view{};
    crtc_advance_view(&rig.dev, 1, &view);
    --remaining;
    const bool event = (view.edges & (1u << CRTC_EDGE_HSYNC_FALL)) != 0 ||
                       view.frame_line != prev_line;
    if (event) ++events;
    if (remaining > 0) {
      EXPECT_FALSE(event) << "INT-path event " << i
                          << " chars in, before the horizon char — the "
                             "scheduler would poll the INT line late";
      if (event) break;
    } else {  // the scheduler's stop point: re-poll, recompute
      EXPECT_TRUE(event) << "horizon stopped at char " << i
                         << " with nothing to poll (standard frame)";
      ++polls;
      remaining = crtc_irq_horizon_chars(&rig.dev);
      ASSERT_GE(remaining, 1u);
    }
    prev_line = view.frame_line;
  }
  // Standard frame: 312 line ends + 312 HSYNC falls, all distinct chars.
  EXPECT_EQ(events, 312 * 2 * 2);
  EXPECT_EQ(polls, events);  // every event char was a scheduler stop
}

// F8 R14 differential oracle: crtc_advance_view's closed-form span fill must
// be char-for-char identical to the per-cycle tick — same levels, same fetch
// address, same raster line, same frame scanline, and edges exactly where the
// per-cycle levels transition. Chunk sizes cycle through awkward strides so
// span starts land on every phase of the line; the register programs cover
// the per-type quirks the span logic reasons about (R3-zero HSYNC widths, R8
// skew including display-off, R2 at/beyond R0, tiny R0 lines, VSYNC extremes).
TEST(Crtc, AdvanceViewMatchesPerCycleTickCharForChar) {
  struct Program {
    uint8_t type;
    uint8_t regs[10];  // R0..R9
  };
  const Program programs[] = {
      {0, {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7}},  // standard CPC
      {1, {63, 40, 46, 0x00, 38, 0, 25, 30, 0, 7}},  // type 1, R3=0: no HSYNC
      {2, {63, 40, 46, 0x00, 38, 0, 25, 30, 0, 7}},  // type 2, R3=0: width 16
      {0, {63, 40, 46, 0x8E, 38, 0, 25, 30, 0x10, 7}},  // skew 1
      {3, {63, 40, 46, 0x8E, 38, 0, 25, 30, 0x20, 7}},  // type 3, skew 2
      {0, {63, 40, 46, 0x8E, 38, 0, 25, 30, 0x30, 7}},  // skew 3: display off
      {0, {40, 32, 46, 0x8E, 38, 0, 25, 30, 0, 7}},     // R2 > R0: HSYNC never
      {0, {63, 40, 0, 0x8E, 38, 0, 25, 30, 0, 7}},      // R2 = 0: rise at HCC 0
      {0, {3, 2, 1, 0x81, 38, 2, 25, 30, 0, 1}},        // tiny line, tight sync
      {0, {63, 40, 46, 0x8E, 38, 0, 25, 0, 0, 7}},      // R7 = 0 VSYNC at top
  };
  for (const Program& prog : programs) {
    CrtcRig a;  // batch: crtc_advance_view in varying chunks
    CrtcRig b;  // reference: the per-cycle tick, one char per board tick
    make_crtc(a);
    make_crtc(b);
    for (auto* rig : {&a, &b}) {
      for (uint8_t i = 0; i < 10; ++i)
        crtc_poke_reg(&rig->dev, i, prog.regs[i]);
      crtc_set_type(&rig->dev, prog.type);  // the hardware strap
    }

    const uint32_t kChunks[] = {1, 3, 17, 64, 313, 40, 5};
    uint32_t chunk_i = 0;
    std::vector<CrtcCharView> views(400);
    uint8_t prev_hs = 0, prev_vs = 0;
    {
      CrtcRegs r0{};
      crtc_peek(&b.dev, &r0);
      prev_hs = r0.hsync;
      prev_vs = r0.vsync;
    }
    uint32_t done = 0;
    const uint32_t total = 50000;  // > two frames of the standard program
    while (done < total) {
      uint32_t n = kChunks[chunk_i++ % (sizeof(kChunks) / sizeof(kChunks[0]))];
      if (n > total - done) n = total - done;
      crtc_advance_view(&a.dev, n, views.data());
      for (uint32_t k = 0; k < n; ++k) {
        const CrtcRegs ref = crtc_char_tick(b);
        const CrtcCharView& view = views[k];
        const uint32_t at = done + k;
        ASSERT_EQ(view.ma, ref.ma)
            << "type " << int(prog.type) << " char " << at;
        ASSERT_EQ(view.ra, ref.ra)
            << "type " << int(prog.type) << " char " << at;
        ASSERT_EQ((view.levels & CRTC_LVL_HSYNC) != 0, ref.hsync != 0)
            << "type " << int(prog.type) << " char " << at;
        ASSERT_EQ((view.levels & CRTC_LVL_VSYNC) != 0, ref.vsync != 0)
            << "type " << int(prog.type) << " char " << at;
        ASSERT_EQ((view.levels & CRTC_LVL_DISPEN) != 0, ref.dispen != 0)
            << "type " << int(prog.type) << " char " << at;
        ASSERT_EQ(view.frame_line, ref.scanline)
            << "type " << int(prog.type) << " char " << at;
        // Edges must be exactly the level transitions the reference shows.
        uint8_t want_edges = 0;
        if (ref.vsync != prev_vs)
          want_edges |= static_cast<uint8_t>(
              1u << (ref.vsync ? CRTC_EDGE_VSYNC_RISE : CRTC_EDGE_VSYNC_FALL));
        if (ref.hsync != prev_hs)
          want_edges |= static_cast<uint8_t>(
              1u << (ref.hsync ? CRTC_EDGE_HSYNC_RISE : CRTC_EDGE_HSYNC_FALL));
        ASSERT_EQ(view.edges, want_edges)
            << "type " << int(prog.type) << " char " << at;
        prev_hs = ref.hsync;
        prev_vs = ref.vsync;
      }
      done += n;
    }
  }
}

// ---------------------------------------------------------------------------
// The I/O-path oracles: crtc_fast_io_write / crtc_fast_io_read /
// crtc_advance_chars / crtc_fast_lpen_strobe must match the per-cycle crtc_tick
// char-for-char. The existing AdvanceView oracle proves the FREE-RUNNING span
// fill; these cover the catch-up-then-apply contract — register writes and
// reads landing at char k, the edge-only stream, and the LPEN latch — the
// paths the Fast scheduler exercises live but the chain tests only check at
// end-state.
// ---------------------------------------------------------------------------

// crtc_fast_io_write must leave the CRTC in the same state a per-cycle
// crtc_tick snooping the same &BC/&BD write at the same char would — including
// the R7-hit VSYNC-rise edge the decode returns, and the geometry-moving
// writes (R2 repositioning, R0-below-HCC mid-line stretch, R9-below-RA raster
// wrap) whose effects the closed-form span hoists as constants. The batch rig
// applies the write before advancing the char (decode-before-crtc_char,
// matching crtc_tick); full CrtcRegs is diffed every char, and a write's
// returned edge mask is checked against the per-cycle vsync transition.
TEST(Crtc, FastIoWriteMatchesPerCycleSnoop) {
  struct W {
    uint32_t at;
    uint16_t port;
    uint8_t val;
  };
  struct Scn {
    const char* name;
    std::vector<W> writes;
  };
  // Standard regs: R0=63 R1=40 R2=46 R3=0x8E R4=38 R6=25 R9=7 → 64 chars/line,
  // 8 scanlines/row, 512 chars/row. vcc=4 at char 2048; hcc=50 at char 50;
  // ra=5 at char 320 (5 lines in, R9=7 so ra cycles 0..7).
  const std::vector<Scn> scenarios = {
      {"R2 reposition mid-line", {{5, 0xBC00, 2}, {6, 0xBD00, 20}}},
      {"R7-hit starts VSYNC", {{2048, 0xBC00, 7}, {2049, 0xBD00, 4}}},
      {"R7-hit no retrigger while in_vsync",
       {{2048, 0xBC00, 7},
        {2049, 0xBD00, 4},
        {2050, 0xBC00, 7},
        {2051, 0xBD00, 4}}},
      {"R0 below HCC mid-line stretch", {{50, 0xBC00, 0}, {51, 0xBD00, 40}}},
      {"R9 below RA raster wrap", {{320, 0xBC00, 9}, {321, 0xBD00, 3}}},
      {"non-CRTC port (A14 high) is a no-op", {{100, 0xC000, 0xFF}}},
      {"select-only write (&BC) leaves reg_select",
       {{10, 0xBC00, 3}, {11, 0xBD00, 0x55}}},
  };
  const uint32_t total = 4096;
  for (const Scn& scn : scenarios) {
    CrtcRig a, b;
    make_crtc(a);
    make_crtc(b);
    size_t wi = 0;
    for (uint32_t k = 0; k < total; ++k) {
      CrtcRegs pre{};
      crtc_peek(&a.dev, &pre);
      const uint8_t pre_vs = pre.vsync;
      uint8_t caused = 0;
      const bool is_write = wi < scn.writes.size() && scn.writes[wi].at == k;
      if (is_write) {
        const W& w = scn.writes[wi++];
        // Batch: decode (pre-char state) then advance one char.
        caused = crtc_fast_io_write(&a.dev, w.port, w.val);
        CrtcCharView view{};
        crtc_advance_view(&a.dev, 1, &view);
        // Per-cycle: the snooped write + char advance in one tick.
        crtc_io_write_tick(b, w.port, w.val);
        // The write's returned edge mask vs the per-cycle vsync transition.
        // A char advance can also move vsync (newscanline), so account for
        // both the write-caused edge and the char's own view edge.
        const uint8_t batch_rise =
            static_cast<uint8_t>((caused >> CRTC_EDGE_VSYNC_RISE) & 1u) |
            static_cast<uint8_t>((view.edges >> CRTC_EDGE_VSYNC_RISE) & 1u);
        const uint8_t batch_fall =
            static_cast<uint8_t>((view.edges >> CRTC_EDGE_VSYNC_FALL) & 1u);
        CrtcRegs post{};
        crtc_peek(&b.dev, &post);
        const uint8_t want_rise = (post.vsync && !pre_vs) ? 1 : 0;
        const uint8_t want_fall = (!post.vsync && pre_vs) ? 1 : 0;
        EXPECT_EQ(batch_rise, want_rise) << scn.name << " VSYNC rise @ " << k;
        EXPECT_EQ(batch_fall, want_fall) << scn.name << " VSYNC fall @ " << k;
      } else {
        CrtcCharView view{};
        crtc_advance_view(&a.dev, 1, &view);
        crtc_char_tick(b);
      }
      CrtcRegs ra{}, rb{};
      crtc_peek(&a.dev, &ra);
      crtc_peek(&b.dev, &rb);
      expect_regs_eq(ra, rb, static_cast<int>(k));
    }
    ASSERT_EQ(wi, scn.writes.size())
        << scn.name << ": not all writes applied (char offset past total?)";
  }
}

// crtc_fast_io_read must return the same byte the per-cycle crtc_tick drives
// onto the bus at that char, for every type's readable window: R12-R17 on
// types 0/3, R14-R17 on types 1/2, the type-1 status register (&BExx, bit 5
// = vcc >= R6), the type-1 R31 0xFF quirk, and the no-drive floats (0xFF) on
// &BC/&BD and A14-high. Reads are taken at chars straddling R6 so the type-1
// status bit flips.
TEST(Crtc, FastIoReadMatchesPerCycleBusDrive) {
  struct Read {
    uint32_t at;
    uint16_t port;
    uint8_t sel;  // reg_select to set first (&BC00 write); 0xFF = leave as-is
  };
  const uint8_t types[4] = {0, 1, 2, 3};
  // vcc=0 at char 0 (status bit 0); vcc=25 (=R6) at char 25*512=12800 (bit 1).
  const std::vector<Read> reads = {
      {0, 0xBF00, 14},  // R14: readable on all types (>=12/>=14)
      {0, 0xBF00, 12},  // R12: readable types 0/3; 0 (write-only) types 1/2
      {0, 0xBF00, 31},  // R31: type 1 → 0xFF; others → 0
      {0, 0xBE00, 14},  // &BExx fn2: type 3 reads reg; type 1 status; 0/2 float
      {0, 0xBC00, 0xFF},    // &BC fn0: select — no drive, floats 0xFF
      {0, 0xBD00, 0xFF},    // &BD fn1: write — no drive, floats 0xFF
      {0, 0xC000, 0xFF},    // A14 high: not a CRTC port, floats 0xFF
      {12800, 0xBE00, 14},  // type 1 status at vcc=25 (=R6) → bit 5 set (0x20)
  };
  for (const uint8_t type : types) {
    for (const Read& rd : reads) {
      CrtcRig a, b;
      make_crtc(a);
      make_crtc(b);
      crtc_set_type(&a.dev, type);
      crtc_set_type(&b.dev, type);
      // Advance both rigs identically to the read char (the AdvanceView oracle
      // proves the states match), then set reg_select via a real write on both
      // so the select-decode path is exercised identically.
      for (uint32_t k = 0; k < rd.at; ++k) {
        CrtcCharView v{};
        crtc_advance_view(&a.dev, 1, &v);
        crtc_char_tick(b);
      }
      if (rd.sel != 0xFF) {
        crtc_fast_io_write(&a.dev, 0xBC00, rd.sel);
        crtc_io_write_tick(b, 0xBC00, rd.sel);
        // The select write tick advanced b by one char; mirror a so the read
        // samples the same char/state.
        CrtcCharView v{};
        crtc_advance_view(&a.dev, 1, &v);
      }
      uint8_t batch_out = 0xAA;  // sentinel: must be overwritten on a drive
      const int batch_ret = crtc_fast_io_read(&a.dev, rd.port, &batch_out);
      const uint8_t pc_bus = crtc_io_read_bus(b, rd.port);
      if (batch_ret != 0) {
        EXPECT_EQ(batch_out, pc_bus)
            << "type " << int(type) << " port " << std::hex << rd.port << " @ "
            << std::dec << rd.at;
      } else {
        // No drive: the bus floats (0xFF). A driven 0xFF (type 1 R31) is
        // reported via batch_ret != 0 above, so this branch is the true float.
        EXPECT_EQ(pc_bus, 0xFF)
            << "type " << int(type) << " port " << std::hex << rd.port << " @ "
            << std::dec << rd.at << " should float";
      }
    }
  }
}

// crtc_advance_chars (the edge-only API) must emit the same edge stream — same
// `at` char indices, same kinds, same VSYNC-before-HSYNC within-char ordering
// — as the per-cycle tick's level transitions, across the per-type/per-reg
// quirks and awkward chunk strides.
TEST(Crtc, AdvanceCharsEdgeStreamMatchesPerCycle) {
  struct Program {
    uint8_t type;
    uint8_t regs[10];
  };
  const Program programs[] = {
      {0, {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7}},
      {1, {63, 40, 46, 0x00, 38, 0, 25, 30, 0, 7}},
      {2, {63, 40, 46, 0x00, 38, 0, 25, 30, 0, 7}},
      {0, {63, 40, 46, 0x8E, 38, 0, 25, 30, 0x10, 7}},
      {3, {63, 40, 46, 0x8E, 38, 0, 25, 30, 0x20, 7}},
      {0, {63, 40, 46, 0x8E, 38, 0, 25, 30, 0x30, 7}},
      {0, {40, 32, 46, 0x8E, 38, 0, 25, 30, 0, 7}},
      {0, {63, 40, 0, 0x8E, 38, 0, 25, 30, 0, 7}},
      {0, {3, 2, 1, 0x81, 38, 2, 25, 30, 0, 1}},
      {0, {63, 40, 46, 0x8E, 38, 0, 25, 0, 0, 7}},
  };
  const uint32_t kChunks[] = {1, 3, 17, 64, 313, 40, 5};
  const uint32_t total = 50000;
  for (const Program& prog : programs) {
    CrtcRig a, b;
    make_crtc(a);
    make_crtc(b);
    for (auto* rig : {&a, &b}) {
      for (uint8_t i = 0; i < 10; ++i)
        crtc_poke_reg(&rig->dev, i, prog.regs[i]);
      crtc_set_type(&rig->dev, prog.type);
    }
    // Reference: per-cycle tick, record (char, kind) for each level transition,
    // VSYNC before HSYNC within a char (matching crtc_advance_chars ordering).
    std::vector<CrtcEdge> ref;
    {
      uint8_t prev_hs = 0, prev_vs = 0;
      {
        CrtcRegs r0{};
        crtc_peek(&b.dev, &r0);
        prev_hs = r0.hsync;
        prev_vs = r0.vsync;
      }
      for (uint32_t k = 0; k < total; ++k) {
        const CrtcRegs r = crtc_char_tick(b);
        if (r.vsync != prev_vs)
          ref.push_back(
              {k, static_cast<uint8_t>(r.vsync ? CRTC_EDGE_VSYNC_RISE
                                               : CRTC_EDGE_VSYNC_FALL)});
        if (r.hsync != prev_hs)
          ref.push_back(
              {k, static_cast<uint8_t>(r.hsync ? CRTC_EDGE_HSYNC_RISE
                                               : CRTC_EDGE_HSYNC_FALL)});
        prev_hs = r.hsync;
        prev_vs = r.vsync;
      }
    }
    // Batch: crtc_advance_chars in varying chunks, collect edges.
    std::vector<CrtcEdge> got;
    got.reserve(ref.size() + 64);
    uint32_t chunk_i = 0, done = 0;
    while (done < total) {
      uint32_t n = kChunks[chunk_i++ % (sizeof(kChunks) / sizeof(kChunks[0]))];
      if (n > total - done) n = total - done;
      // Worst legal density is two edges per char (a same-char VSYNC + HSYNC
      // transition), so n*2+4 can never truncate (crtc.h §batch).
      std::vector<CrtcEdge> buf(static_cast<size_t>(n) * 2 + 4);
      const int produced = crtc_advance_chars(&a.dev, n, buf.data(),
                                              static_cast<int>(buf.size()));
      ASSERT_LE(produced, static_cast<int>(buf.size()))
          << "edge buffer overflow — type " << int(prog.type);
      for (int i = 0; i < produced; ++i) {
        CrtcEdge e = buf[i];
        e.at += done;  // char index within the whole run
        got.push_back(e);
      }
      done += n;
    }
    ASSERT_EQ(got.size(), ref.size())
        << "type " << int(prog.type) << " edge count";
    for (size_t i = 0; i < got.size(); ++i) {
      EXPECT_EQ(got[i].at, ref[i].at)
          << "type " << int(prog.type) << " edge " << i << " at";
      EXPECT_EQ(got[i].kind, ref[i].kind)
          << "type " << int(prog.type) << " edge " << i << " kind";
    }
  }
}

// crtc_fast_lpen_strobe must latch R16/R17 exactly as the per-cycle crtc_tick
// does on the pen.strobe rising edge — same ma, edge-triggered (held high does
// not re-latch), re-armed on falling, and re-latched on the next rise. The
// batch rig advances one char then presents the strobe level (post-advance ma,
// matching crtc_tick's post-crtc_char strobe check); the per-cycle rig sets
// pen.strobe on the bus before each char tick. Both rigs see the same strobe
// level sequence; R16/R17 are diffed every char.
TEST(Crtc, LpenStrobeMatchesPerCycleLatch) {
  CrtcRig a, b;
  make_crtc(a);
  make_crtc(b);
  // A strobe-level sequence exercising rising (latch), held (no re-latch),
  // falling (re-arm), rising (re-latch), and quiet gaps.
  const bool levels[] = {false, false, true,  true, true, false,
                         true,  false, false, true, true, false,
                         false, true,  false, true, true, true};
  const int nlevels = static_cast<int>(sizeof(levels) / sizeof(levels[0]));
  const uint32_t total = 5000;
  for (uint32_t k = 0; k < total; ++k) {
    const bool level = levels[k % nlevels];
    // Batch: advance one char, then present the strobe (post-advance ma).
    CrtcCharView view{};
    crtc_advance_view(&a.dev, 1, &view);
    crtc_fast_lpen_strobe(&a.dev, level);
    // Per-cycle: set pen.strobe on the committed bus, then char-tick. crtc_tick
    // reads in->pen.strobe after crtc_char advances ma — same post-advance ma.
    b.board.bus = bus_resting();
    b.board.bus.clk.crtc = true;
    b.board.bus.pen.strobe = level;
    board_tick(&b.board);
    CrtcRegs ra{}, rb{};
    crtc_peek(&a.dev, &ra);
    crtc_peek(&b.dev, &rb);
    ASSERT_EQ(ra.reg[16], rb.reg[16]) << "R16 @ " << k << " level=" << level;
    ASSERT_EQ(ra.reg[17], rb.reg[17]) << "R17 @ " << k << " level=" << level;
    // ma must match too (the latch source) — guards a future span-path ma
    // divergence the R16/R17 check would miss if both latch a wrong value.
    ASSERT_EQ(ra.ma, rb.ma) << "ma @ " << k;
  }
}
