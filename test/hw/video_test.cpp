/* video_test.cpp — the Gate Array pixel path: address mapping, per-mode decode,
 * palette, and an active-line render. Golden data. See
 * docs/hardware/video-device.md. */

#include "hw/video.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {
// A mode-0 byte whose two pixels are both `pen` (4-bit). Real GA plane layout
// (matches the legacy M0Map): pen bit 0 (LSB) is carried on byte bits 7/6, pen
// bit 1 on 3/2, pen bit 2 on 5/4, pen bit 3 (MSB) on 1/0.
uint8_t mode0_solid(uint8_t pen) {
  const uint8_t p3 = (pen >> 3) & 1, p2 = (pen >> 2) & 1, p1 = (pen >> 1) & 1,
                p0 = pen & 1;
  return static_cast<uint8_t>((p0 << 7) | (p0 << 6) | (p2 << 5) | (p2 << 4) |
                              (p1 << 3) | (p1 << 2) | (p3 << 1) | p3);
}
}  // namespace

TEST(Video, AddressMapping) {
  EXPECT_EQ(vid_byte_addr(0x3000, 0, 0), 0xC000)
      << "screen base MA 0x3000 → RAM 0xC000";
  EXPECT_EQ(vid_byte_addr(0x3000, 0, 1), 0xC001) << "byte 1 of the character";
  EXPECT_EQ(vid_byte_addr(0x3000, 1, 0), 0xC800)
      << "RA=1 → +0x800 (2K scanline stride)";
  EXPECT_EQ(vid_byte_addr(0x3000, 7, 0), 0xF800) << "RA=7 → +7*0x800";
  EXPECT_EQ(vid_byte_addr(0x3001, 0, 0), 0xC002) << "MA+1 → +2 bytes";
}

TEST(Video, ModeDecode) {
  uint8_t pens[8];
  // Mode 0: 0xAA = 10101010 → px0 = (1,1,1,1)=15, px1 = (0,0,0,0)=0.
  EXPECT_EQ(vid_decode(0, 0xAA, pens), 2);
  EXPECT_EQ(pens[0], 15);
  EXPECT_EQ(pens[1], 0);
  // Mode 1: 0xA7 → 1,2,3,2 (real GA plane order: high plane = bit3-k, low =
  // bit7-k).
  EXPECT_EQ(vid_decode(1, 0xA7, pens), 4);
  EXPECT_EQ(pens[0], 1);
  EXPECT_EQ(pens[1], 2);
  EXPECT_EQ(pens[2], 3);
  EXPECT_EQ(pens[3], 2);
  // Mode 2: 0x81 = 10000001 → 1,0,0,0,0,0,0,1 (MSB first).
  EXPECT_EQ(vid_decode(2, 0x81, pens), 8);
  EXPECT_EQ(pens[0], 1);
  EXPECT_EQ(pens[7], 1);
  for (int i = 1; i < 7; ++i) EXPECT_EQ(pens[i], 0);
}

// Hardware-exact mode-0 pen bit assignment (matches the legacy M0Map / real
// Gate Array): for pixel 0 the pen bits are byte bit7 → P0, bit3 → P1, bit5 →
// P2, bit1 → P3 (pixel 1 uses bits 6/2/4/0). A regression swapped bit3↔bit5
// (pen bits 1↔2), which decoded e.g. 0xF3 to pen 11 instead of pen 13 — turning
// Burnin' Rubber's grey mode-0 fence fill black. These expected values come
// straight from src/crtc.cpp's M0Map lookup table (the oracle).
TEST(Video, ModeDecode0Hardware) {
  uint8_t p[8];
  auto pens0 = [&](uint8_t b) {
    vid_decode(0, b, p);
    return std::pair<int, int>(p[0], p[1]);
  };
  EXPECT_EQ(pens0(0xF3), (std::pair<int, int>(13, 13)));  // fence fill = grey
  EXPECT_EQ(pens0(0x9F), (std::pair<int, int>(11, 14)));
  EXPECT_EQ(pens0(0x08), (std::pair<int, int>(2, 0)));    // bit3 → pen bit1
  EXPECT_EQ(pens0(0x20), (std::pair<int, int>(4, 0)));    // bit5 → pen bit2
  EXPECT_EQ(pens0(0x80), (std::pair<int, int>(1, 0)));
  EXPECT_EQ(pens0(0xAA), (std::pair<int, int>(15, 0)));
}

// Mode 3 (undocumented, gate-array.md §"Mode 3"): 2 px/byte at mode 0's width,
// but only pens 0-3 — mode 1's P0/P1 bits, with mode 0's P2/P3 planes IGNORED.
// Literal-byte ground truth (not a re-derivation of the decode).
TEST(Video, ModeDecode3Hardware) {
  uint8_t p[8];
  auto pens3 = [&](uint8_t b) {
    const int n = vid_decode(3, b, p);
    EXPECT_EQ(n, 2) << "mode 3 emits 2 pixels per byte";
    return std::pair<int, int>(p[0], p[1]);
  };
  EXPECT_EQ(pens3(0x00), (std::pair<int, int>(0, 0)));
  EXPECT_EQ(pens3(0x88), (std::pair<int, int>(3, 0)));  // bit7|bit3 → px0 = 3
  EXPECT_EQ(pens3(0x44), (std::pair<int, int>(0, 3)));  // bit6|bit2 → px1 = 3
  EXPECT_EQ(pens3(0x80), (std::pair<int, int>(1, 0)));
  EXPECT_EQ(pens3(0x08), (std::pair<int, int>(2, 0)));
  EXPECT_EQ(pens3(0xCC), (std::pair<int, int>(3, 3)));
  // The discriminator: bits 5 and 1 are mode 0's P2/P3 — mode 3 must DROP them,
  // so a byte that would light high pens in mode 0 decodes to pen 0 here.
  EXPECT_EQ(pens3(0x22), (std::pair<int, int>(0, 0)))
      << "mode 3 has no P2/P3: inks stay in 0-3";
  EXPECT_EQ(vid_px_per_char(3), 4) << "mode 3 is 160 px wide, like mode 0";
}

// vid_decode_lut must be byte-identical to vid_decode over the WHOLE input
// domain — it is the Fast-tier (Gate B7) decode, and the plan requires the fast
// path to reproduce the reference's pixels exactly. Modes 0-3 are the real
// 2-bit register values; 4-7 exercise the "mode > 3 folds to mode 2" arm both
// functions share (guards the accessor's indexing/count/fold, since the table
// is generated from vid_decode and so equal by construction otherwise).
TEST(VideoDecodeLut, MatchesScalarOverWholeDomain) {
  for (unsigned mode = 0; mode < 8; ++mode) {
    for (int byte = 0; byte < 256; ++byte) {
      uint8_t ref[8] = {0}, lut[8] = {0};
      const int n_ref =
          vid_decode(static_cast<uint8_t>(mode), static_cast<uint8_t>(byte), ref);
      const int n_lut = vid_decode_lut(static_cast<uint8_t>(mode),
                                       static_cast<uint8_t>(byte), lut);
      ASSERT_EQ(n_lut, n_ref) << "count differs at mode " << mode << " byte "
                              << byte;
      for (int p = 0; p < n_ref; ++p)
        ASSERT_EQ(lut[p], ref[p])
            << "pen differs at mode " << mode << " byte " << byte << " px " << p;
    }
  }
}

// The 32-entry hardware colour table: 27 distinct colours with 5 documented
// duplicate index pairs, and the 0.5-level (128) rung really present. Anchored
// to literal RGB values, independent of the palette packer.
TEST(Video, PaletteHardwareColoursAndDuplicates) {
  auto rgb = [](uint8_t idx) {
    uint8_t r, g, b;
    vid_hw_rgb(idx, &r, &g, &b);
    return std::array<uint8_t, 3>{r, g, b};
  };
  // The 5 duplicate pairs (same RGB, different index):
  EXPECT_EQ(rgb(0), rgb(1));    // 0x00,0x01 → grey (128,128,128)
  EXPECT_EQ(rgb(2), rgb(17));   // 0x02,0x11 → sea green (0,255,128)
  EXPECT_EQ(rgb(3), rgb(9));    // 0x03,0x09 → pastel yellow (255,255,128)
  EXPECT_EQ(rgb(4), rgb(16));   // 0x04,0x10 → blue (0,0,128)
  EXPECT_EQ(rgb(5), rgb(8));    // 0x05,0x08 → magenta (255,0,128)
  EXPECT_EQ(rgb(0), (std::array<uint8_t, 3>{128, 128, 128}));
  EXPECT_EQ(rgb(11), (std::array<uint8_t, 3>{255, 255, 255})) << "bright white";
  EXPECT_EQ(rgb(20), (std::array<uint8_t, 3>{0, 0, 0})) << "black";
  EXPECT_EQ(rgb(6), (std::array<uint8_t, 3>{0, 128, 128}))
      << "half-level (0.5 = 128) channels are real";
  // Exactly 27 distinct colours across the 32 indices (three levels per RGB).
  std::set<std::array<uint8_t, 3>> distinct;
  for (int i = 0; i < 32; ++i) distinct.insert(rgb(static_cast<uint8_t>(i)));
  EXPECT_EQ(distinct.size(), 27u) << "3×3×3 = 27 hardware colours";
}

TEST(Video, Palette) {
  uint8_t r, g, b;
  vid_hw_rgb(20, &r, &g, &b);
  EXPECT_EQ(r, 0);
  EXPECT_EQ(g, 0);
  EXPECT_EQ(b, 0);  // black
  vid_hw_rgb(11, &r, &g, &b);
  EXPECT_EQ(r, 255);
  EXPECT_EQ(g, 255);
  EXPECT_EQ(b, 255);  // white
  vid_hw_rgb(18, &r, &g, &b);
  EXPECT_EQ(r, 0);
  EXPECT_EQ(g, 255);
  EXPECT_EQ(b, 0);  // bright green
  vid_hw_rgb(12, &r, &g, &b);
  EXPECT_EQ(r, 255);
  EXPECT_EQ(g, 0);
  EXPECT_EQ(b, 0);  // bright red
  vid_hw_rgb(21, &r, &g, &b);
  EXPECT_EQ(r, 0);
  EXPECT_EQ(g, 0);
  EXPECT_EQ(b, 255);  // bright blue
}

TEST(Video, RenderActiveLine) {
  std::vector<uint8_t> ram(0x10000, 0);
  ram[vid_byte_addr(0x3000, 0, 0)] = 0xA7;  // mode-1 pens 2,1,3,1
  ram[vid_byte_addr(0x3000, 0, 1)] = 0x00;  // pens 0,0,0,0
  // ink[pen] = hardware colour: 0→black, 1→red, 2→green, 3→white.
  const uint8_t ink[17] = {20, 12, 18, 11};
  uint8_t out[8 * 3] = {0};
  const int px =
      vid_render_line(ram.data(), 1, ink, 0x3000, 0, /*chars=*/1, out);
  EXPECT_EQ(px, 8) << "one char in mode 1 = 2 bytes * 4 px";
  // First 4 pixels (byte 0 = 0xA7 → pens 1,2,3,2 → red,green,white,green):
  EXPECT_EQ(out[0], 255);
  EXPECT_EQ(out[1], 0);
  EXPECT_EQ(out[2], 0);  // px0 red
  EXPECT_EQ(out[3], 0);
  EXPECT_EQ(out[4], 255);
  EXPECT_EQ(out[5], 0);  // px1 green
  EXPECT_EQ(out[6], 255);
  EXPECT_EQ(out[7], 255);
  EXPECT_EQ(out[8], 255);  // px2 white
  EXPECT_EQ(out[9], 0);
  EXPECT_EQ(out[10], 255);
  EXPECT_EQ(out[11], 0);  // px3 green
  // Next 4 (byte 1 = 0x00 → pens 0,0,0,0 → black):
  EXPECT_EQ(out[12], 0);
  EXPECT_EQ(out[13], 0);
  EXPECT_EQ(out[14], 0);  // px4 black
}

TEST(Video, RenderFrameColourBands) {
  // GEOMETRY / pipeline test: band layout, frame dimensions, and MA addressing.
  // The bands are encoded with mode0_solid and read back with vid_render, which
  // share the pen-decode convention — so this is NOT a decode oracle (see
  // Video.ModeDecode0Hardware, which feeds literal bytes). The raw-byte block
  // at the end guards the render path's decode with independent ground truth.
  //
  // Mode 0 (16 colours, 160x200). 40 chars/row; column c shows pen (c % 16), so
  // the frame is 16 vertical colour bands. Distinct inks for the 16 pens.
  const uint8_t ink[17] = {20, 4,  21, 6,  22, 18, 19, 11, 12,
                           24, 13, 14, 15, 10, 30, 2,  0};
  std::vector<uint8_t> ram(0x10000, 0);
  const uint8_t r0mode = 0, r1 = 40, r6 = 25, r9 = 7;
  const uint16_t ma_start = 0x3000;
  for (uint8_t row = 0; row < r6; ++row) {
    const uint16_t ma_base = static_cast<uint16_t>(ma_start + row * r1);
    for (uint8_t ra = 0; ra <= r9; ++ra)
      for (uint8_t ch = 0; ch < r1; ++ch) {
        const uint8_t byte = mode0_solid(static_cast<uint8_t>(ch % 16));
        ram[vid_byte_addr(static_cast<uint16_t>(ma_base + ch), ra, 0)] = byte;
        ram[vid_byte_addr(static_cast<uint16_t>(ma_base + ch), ra, 1)] = byte;
      }
  }
  const int width = r1 * vid_px_per_char(r0mode);  // 40*4 = 160
  const int height = r6 * (r9 + 1);                // 25*8 = 200
  std::vector<uint8_t> fb(static_cast<size_t>(width) * height * 3, 0);
  vid_render_frame(ram.data(), r0mode, ink, ma_start, r1, r6, r9, fb.data());

  EXPECT_EQ(width, 160);
  EXPECT_EQ(height, 200);
  // Pixel 0 = pen 0 = ink 20 = black; pixels 8..9 (char 1) = pen 1 = ink 4 =
  // {0,0,128}.
  EXPECT_EQ(fb[0], 0);
  EXPECT_EQ(fb[1], 0);
  EXPECT_EQ(fb[2], 0);
  const int px_ch1 = 4;  // char 1 starts at pixel 4 (4 px/char in mode 0)
  EXPECT_EQ(fb[px_ch1 * 3 + 2], 128) << "char 1 = pen 1 = ink 4 = blue";

  // Independent decode guard: overwrite char 0 with the RAW literal byte 0xAA
  // and char 1 with 0x00 — bypassing mode0_solid entirely. Ground truth from
  // Video.ModeDecode0Hardware: 0xAA → pens (15, 0), 0x00 → pens (0, 0). So the
  // rendered pixels must satisfy pen equalities without knowing any RGB value:
  // char0.px1, char1.px0, char1.px1 are all pen 0 (equal colour), and
  // char0.px0 (pen 15) differs from them.
  for (uint8_t ra = 0; ra <= r9; ++ra) {
    ram[vid_byte_addr(ma_start + 0, ra, 0)] = 0xAA;
    ram[vid_byte_addr(ma_start + 0, ra, 1)] = 0xAA;
    ram[vid_byte_addr(ma_start + 1, ra, 0)] = 0x00;
    ram[vid_byte_addr(ma_start + 1, ra, 1)] = 0x00;
  }
  vid_render_frame(ram.data(), r0mode, ink, ma_start, r1, r6, r9, fb.data());
  auto px = [&](int c, int k) {  // RGB triple of char c, sub-pixel k, top row
    const int i = (c * vid_px_per_char(r0mode) + k) * 3;
    return std::array<uint8_t, 3>{fb[i], fb[i + 1], fb[i + 2]};
  };
  EXPECT_EQ(px(1, 0), px(1, 1)) << "0x00 → pens (0,0): both sub-pixels equal";
  EXPECT_EQ(px(0, 1), px(1, 0)) << "char0.px1 and char1.px0 are both pen 0";
  EXPECT_NE(px(0, 0), px(0, 1)) << "0xAA → pen 15 (px0) ≠ pen 0 (px1)";

  // Write a PPM artifact so the frame is viewable.
  if (FILE* f = std::fopen("/tmp/cpc_frame.ppm", "wb")) {
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    std::fwrite(fb.data(), 1, fb.size(), f);
    std::fclose(f);
  }
}

// ---- Live video Device: GA + CRTC + memory + video on a board ----

#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"

namespace {
// Injects a scripted sequence of Gate Array register writes over the first
// ticks, then idles — sets mode/palette on a running board without disturbing
// the clock.
struct Injector {
  const uint8_t* seq;
  int len;
  int tick;
};
void inj_tick(void* self, const Bus*, Bus* out) {
  Injector* j = static_cast<Injector*>(self);
  if (j->tick < j->len) {
    out->cpu.iorq = true;
    out->cpu.wr = true;
    out->cpu.addr = 0x7F00;
    out->cpu.data = j->seq[j->tick];
  }
  j->tick++;
}
size_t inj_size(const void*) { return sizeof(Injector); }
void inj_sl(const void*, void*) {}
Device inj_device(Injector* j) {
  return Device{j,
                "inj",
                inj_tick,
                [](void*) {},
                inj_size,
                [](const void*, void*) {},
                [](void*, const void*) {}};
}
}  // namespace

TEST(Video, LiveDeviceRendersActiveAndBorder) {
  // GA setup: mode 1; pen 0 → ink 20 (the whole active area, since RAM = 0 →
  // pen 0); the border pen (16) → ink 6. Bytes are raw GA-port writes: 0x81 =
  // mode 1; 0x00/0x54 = pen 0 ink 20; 0x10/0x46 = border pen ink 6.
  const uint8_t seq[] = {0x81, 0x00, 0x54, 0x10, 0x46};

  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> mmem(mem_state_size());
  Device mdev = mem_init(mmem.data());
  std::vector<uint8_t> vmem(video_state_size());
  Device vdev = video_init(vmem.data());
  Injector inj{seq, static_cast<int>(sizeof(seq)), 0};

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, inj_device(&inj));
  board_add(&board, cdev);
  board_add(&board, mdev);
  board_add(&board, vdev);
  board_reset(&board);

  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&cdev, i, std_regs[i]);
  crtc_poke_reg(&cdev, 12, 0x30);
  // Screen RAM left at 0 → every active pixel decodes to pen 0.

  const int w = 768, h = 272;  // the full monitor window
  std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 3, 0);
  video_attach(&vdev, &gdev, fb.data(), w, h);

  VideoRegs vr{};
  for (int tick = 0; tick < 1400000 && vr.frames < 3; ++tick) {  // a few frames
    board_tick(&board);
    video_peek(&vdev, &vr);
  }
  ASSERT_GE(vr.frames, 2u) << "the live renderer completed frames";

  auto rgb = [](uint8_t colour) {
    uint8_t r, g, b;
    vid_hw_rgb(colour, &r, &g, &b);
    return std::vector<uint8_t>{r, g, b};
  };
  auto at = [&](int x, int y) {
    const size_t o = (static_cast<size_t>(y) * w + x) * 3;
    return std::vector<uint8_t>{fb[o], fb[o + 1], fb[o + 2]};
  };

  // Corners are border (pen 16 → ink 6); the centre is active display (pen 0 →
  // ink 20).
  EXPECT_EQ(at(2, 2), rgb(6)) << "top-left is border colour";
  EXPECT_EQ(at(w - 3, h - 3), rgb(6)) << "bottom-right is border colour";
  EXPECT_EQ(at(w / 2, h / 2), rgb(20)) << "centre is active display (pen 0)";
}

// ---- Plus mode: 12-bit palette + per-beam sprite compositing ----

#include "hw/asic.h"

namespace {
void bus_idle(Board* bd, int n = 1) {
  for (int i = 0; i < n; ++i) {
    bd->bus = bus_resting();
    board_tick(bd);
  }
}
// The ASIC unlock knock is sent to the CRTC register-select port (&BCxx).
void bus_knock(Board* bd, uint8_t val) {
  bd->bus = bus_resting();
  bd->bus.cpu.iorq = true;
  bd->bus.cpu.wr = true;
  bd->bus.cpu.addr = 0xBC00;
  bd->bus.cpu.data = val;
  board_tick(bd);
  bus_idle(bd, 2);
}
void bus_unlock(Board* bd) {
  const uint8_t seq[17] = {0xFF, 0x00, 0xFF, 0x77, 0xB3, 0x51, 0xA8, 0xD4, 0x62,
                           0x39, 0x9C, 0x46, 0x2B, 0x15, 0x8A, 0xCD, 0x01};
  for (uint8_t b : seq) bus_knock(bd, b);
  // RMR2: page the register page into &4000-&7FFF (Gate-Array fn-2, port &7Fxx,
  // data 10 11 1000 → membank field 3). The knock only enables RMR2; the CPU
  // must page the register page in before bus_pgwrite() reaches the ASIC.
  bd->bus = bus_resting();
  bd->bus.cpu.iorq = true;
  bd->bus.cpu.wr = true;
  bd->bus.cpu.addr = 0x7F00;
  bd->bus.cpu.data = 0xB8;
  board_tick(bd);
  bus_idle(bd, 1);
}
// A register-page memory write (edge-triggered decode; idle resets the edge).
void bus_pgwrite(Board* bd, uint16_t addr, uint8_t val) {
  bd->bus = bus_resting();
  bd->bus.cpu.mreq = true;
  bd->bus.cpu.wr = true;
  bd->bus.cpu.addr = addr;
  bd->bus.cpu.data = val;
  board_tick(bd);
  bus_idle(bd, 1);
}
// Program a 12-bit palette entry: even byte RRRR_BBBB, odd byte 0000_GGGG.
void bus_set_palette(Board* bd, int entry, uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t base = static_cast<uint16_t>(0x6400 + entry * 2);
  bus_pgwrite(bd, base, static_cast<uint8_t>((r << 4) | b));
  bus_pgwrite(bd, base + 1, g);
}
}  // namespace

TEST(Video, PlusPaletteAndSpriteCompositing) {
  const uint8_t seq[] = {0x81};  // GA: mode 1 (only the mode matters in Plus)

  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> mmem(mem_state_size());
  Device mdev = mem_init(mmem.data());
  std::vector<uint8_t> vmem(video_state_size());
  Device vdev = video_init(vmem.data());
  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());
  // Start the injector spent (silent) so it does not stomp the programming bus;
  // rewind it to 0 just before the render loop to replay the GA mode-set.
  Injector inj{seq, static_cast<int>(sizeof(seq)),
               static_cast<int>(sizeof(seq))};

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, inj_device(&inj));
  board_add(&board, cdev);
  board_add(&board, mdev);
  board_add(&board, adev);
  board_add(&board, vdev);
  board_reset(&board);
  asic_set_plugged(&adev, 1);  // model 3: the ASIC is on the board

  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&cdev, i, std_regs[i]);
  crtc_poke_reg(&cdev, 12, 0x30);

  // Program the ASIC (register page persists after re-lock): background pen 0 →
  // magenta, border pen 16 → cyan, sprite colour 1 → yellow.
  bus_unlock(&board);
  bus_set_palette(&board, 0, 0xF, 0x0, 0xF);   // entry 0  magenta (255,0,255)
  bus_set_palette(&board, 16, 0x0, 0xF, 0xF);  // entry 16 cyan    (0,255,255)
  bus_set_palette(&board, 17, 0xF, 0xF, 0x0);  // entry 17 yellow  (255,255,0)
  // Sprite 0: fill its 16×16 with colour index 1, at active (0,0), mag 1×1.
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x)
      bus_pgwrite(&board, static_cast<uint16_t>(0x4000 | (y << 4) | x), 1);
  bus_pgwrite(&board, 0x6000 + 0, 0x00);  // sprite 0 X low
  bus_pgwrite(&board, 0x6000 + 1, 0x00);  // sprite 0 X high
  bus_pgwrite(&board, 0x6000 + 2, 0x00);  // sprite 0 Y low
  bus_pgwrite(&board, 0x6000 + 3, 0x00);  // sprite 0 Y high
  bus_pgwrite(&board, 0x6000 + 4, 0x05);  // magnification x=1, y=1

  const int w = 768, h = 272;
  std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 3, 0);
  video_attach(&vdev, &gdev, fb.data(), w, h);
  video_attach_asic(&vdev, &adev);
  inj.tick = 0;  // replay the GA mode-set on the running board

  VideoRegs vr{};
  for (int tick = 0; tick < 1400000 && vr.frames < 3; ++tick) {
    board_tick(&board);
    video_peek(&vdev, &vr);
  }
  ASSERT_GE(vr.frames, 2u) << "the Plus renderer completed frames";

  auto at = [&](int x, int y) {
    const size_t o = (static_cast<size_t>(y) * w + x) * 3;
    return std::vector<uint8_t>{fb[o], fb[o + 1], fb[o + 2]};
  };
  const std::vector<uint8_t> magenta{255, 0, 255}, cyan{0, 255, 255},
      yellow{255, 255, 0};

  // Background is the 12-bit palette entry 0; the border is entry 16.
  EXPECT_EQ(at(w / 2, h / 2), magenta) << "centre = 12-bit palette pen 0";
  EXPECT_EQ(at(2, 2), cyan) << "corner = 12-bit border (entry 16)";

  // The sprite composited over the background: yellow pixels exist somewhere.
  int yellows = 0;
  for (int i = 0; i < w * h; ++i)
    if (fb[i * 3] == 255 && fb[i * 3 + 1] == 255 && fb[i * 3 + 2] == 0)
      yellows++;
  EXPECT_GT(yellows, 0) << "sprite 0 composited (entry 17 = yellow)";
  EXPECT_LT(yellows, 16 * 16 + 32) << "sprite is ~256 px (16x16 at mag 1)";
}

// Two overlapping sprites straddling a 16px cell boundary exercise the per-cell
// sprite cull's every subtle path: priority order (sprite 0 wins where opaque),
// transparency fall-through (sprite 1 shows through sprite 0's holes), a live
// lower-priority candidate that must NOT be culled away, and a sprite collected
// as a candidate in BOTH cells it spans. A cull bug in any of these changes the
// composited colours below.
TEST(Video, PlusSpriteOverlapPriorityCull) {
  const uint8_t seq[] = {0x81};  // GA mode 1 (only the mode matters in Plus)
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> mmem(mem_state_size());
  Device mdev = mem_init(mmem.data());
  std::vector<uint8_t> vmem(video_state_size());
  Device vdev = video_init(vmem.data());
  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());
  Injector inj{seq, static_cast<int>(sizeof(seq)),
               static_cast<int>(sizeof(seq))};

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, inj_device(&inj));
  board_add(&board, cdev);
  board_add(&board, mdev);
  board_add(&board, adev);
  board_add(&board, vdev);
  board_reset(&board);
  asic_set_plugged(&adev, 1);

  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&cdev, i, std_regs[i]);
  crtc_poke_reg(&cdev, 12, 0x30);

  bus_unlock(&board);
  bus_set_palette(&board, 0, 0xF, 0x0, 0xF);   // background pen 0 → magenta
  bus_set_palette(&board, 16, 0x0, 0xF, 0xF);  // border → cyan
  bus_set_palette(&board, 17, 0xF, 0xF, 0x0);  // sprite colour 1 → yellow
  bus_set_palette(&board, 18, 0x0, 0xF, 0x0);  // sprite colour 2 → green

  // Sprite 0 (highest priority): left 8 columns opaque colour 1 (yellow), right
  // 8 columns transparent (colour 0). Placed at active (10,10) so it spans the
  // cell boundary at active x=16.
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x)
      bus_pgwrite(&board, static_cast<uint16_t>(0x4000 | (y << 4) | x),
                  x < 8 ? 1 : 0);
  bus_pgwrite(&board, 0x6000 + 0, 10);    // sprite 0 X = 10 (crosses x=16)
  bus_pgwrite(&board, 0x6000 + 1, 0x00);
  bus_pgwrite(&board, 0x6000 + 2, 10);    // sprite 0 Y = 10
  bus_pgwrite(&board, 0x6000 + 3, 0x00);
  bus_pgwrite(&board, 0x6000 + 4, 0x05);  // magnification 1×1

  // Sprite 1 (lower priority): fully opaque colour 2 (green), exactly under 0.
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x)
      bus_pgwrite(&board, static_cast<uint16_t>(0x4100 | (y << 4) | x), 2);
  bus_pgwrite(&board, 0x6008 + 0, 10);
  bus_pgwrite(&board, 0x6008 + 1, 0x00);
  bus_pgwrite(&board, 0x6008 + 2, 10);
  bus_pgwrite(&board, 0x6008 + 3, 0x00);
  bus_pgwrite(&board, 0x6008 + 4, 0x05);  // magnification 1×1

  const int w = 768, h = 272;
  std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 3, 0);
  video_attach(&vdev, &gdev, fb.data(), w, h);
  video_attach_asic(&vdev, &adev);
  inj.tick = 0;

  VideoRegs vr{};
  for (int tick = 0; tick < 1400000 && vr.frames < 3; ++tick) {
    board_tick(&board);
    video_peek(&vdev, &vr);
  }
  ASSERT_GE(vr.frames, 2u) << "the Plus renderer completed frames";

  int yellow = 0, green = 0;
  for (int i = 0; i < w * h; ++i) {
    const uint8_t r = fb[i * 3], g = fb[i * 3 + 1], b = fb[i * 3 + 2];
    if (r == 255 && g == 255 && b == 0)
      yellow++;  // sprite 0 opaque, won priority
    else if (r == 0 && g == 255 && b == 0)
      green++;  // sprite 1 shown through sprite 0's hole
  }
  // Sprite 0's opaque left half beats sprite 1: yellow present.
  EXPECT_GT(yellow, 0) << "sprite 0 (higher priority) wins where it is opaque";
  // Sprite 1 shows through sprite 0's transparent right half: if the cull had
  // dropped the lower-priority candidate, this would be 0.
  EXPECT_GT(green, 0) << "lower-priority sprite 1 falls through, not culled away";
  // Each half is 8×16 = 128 px at mag 1; a cell-boundary clip would shrink them.
  EXPECT_NEAR(yellow, 128, 40) << "opaque half rendered across the cell edge";
  EXPECT_NEAR(green, 128, 40) << "transparent half filled by the lower sprite";
}

TEST(Video, PlusSplitScreenSwapsDisplayBase) {
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> mmem(mem_state_size());
  Device mdev = mem_init(mmem.data());
  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);  // the GA generates the 1 MHz CRTC clock
  board_add(&board, cdev);
  board_add(&board, mdev);
  board_add(&board, adev);
  board_reset(&board);
  crtc_attach_asic(&cdev, &adev);
  asic_set_plugged(&adev, 1);

  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&cdev, i, std_regs[i]);
  crtc_poke_reg(&cdev, 12, 0x30);  // display base 0x3000
  crtc_poke_reg(&cdev, 13, 0x00);

  // Plus split: at frame scanline 100 the display base swaps to 0x2000.
  bus_unlock(&board);
  bus_pgwrite(&board, 0x6801, 100);   // split_line
  bus_pgwrite(&board, 0x6802, 0x20);  // split_addr high → 0x2000
  bus_pgwrite(&board, 0x6803, 0x00);  // split_addr low

  CrtcRegs cr{};
  int frame_idx = 0;
  uint16_t prev_sl = 0;
  int before_min = 0xFFFF, before_max = 0, after_min = 0xFFFF, after_max = 0;
  bool before_any = false, after_any = false;
  for (int tick = 0; tick < 1200000; ++tick) {
    board_tick(&board);
    crtc_peek(&cdev, &cr);
    if (cr.scanline < prev_sl) frame_idx++;  // frame top → counter wrapped
    prev_sl = cr.scanline;
    if (frame_idx < 1 || !cr.dispen)
      continue;              // skip the pre-programming frame
    if (cr.scanline < 90) {  // rows above the split
      before_any = true;
      before_min = cr.ma < before_min ? cr.ma : before_min;
      before_max = cr.ma > before_max ? cr.ma : before_max;
    } else if (cr.scanline >= 110 && cr.scanline < 200) {  // rows below it
      after_any = true;
      after_min = cr.ma < after_min ? cr.ma : after_min;
      after_max = cr.ma > after_max ? cr.ma : after_max;
    }
  }
  ASSERT_TRUE(before_any) << "sampled active cells above the split";
  ASSERT_TRUE(after_any) << "sampled active cells below the split";
  EXPECT_GE(before_min, 0x3000) << "above the split: base is R12/R13 = 0x3000";
  EXPECT_LT(before_max, 0x3400);
  EXPECT_GE(after_min, 0x2000) << "below the split: base swapped to split_addr";
  EXPECT_LT(after_max, 0x3000) << "below the split: no 0x3000-region addresses";
}

// Plus vertical soft scroll: the CRTC drives a fetch RA offset by vscroll.
TEST(Video, PlusVscrollOffsetsFetchRa) {
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);  // the GA generates the CRTC clock
  board_add(&board, cdev);
  board_add(&board, adev);
  board_reset(&board);
  crtc_attach_asic(&cdev, &adev);
  asic_set_plugged(&adev, 1);

  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&cdev, i, std_regs[i]);
  crtc_poke_reg(&cdev, 12, 0x30);

  bus_unlock(&board);
  bus_pgwrite(&board, 0x6804, 2 << 4);  // vscroll = 2 (hscroll = 0)

  bool checked = false;
  CrtcRegs cr{};
  for (int t = 0; t < 400000 && !checked; ++t) {
    board_tick(&board);
    crtc_peek(&cdev, &cr);
    if (cr.ra == 0 && board.bus.vid.dispen && !board.bus.vid.hsync) {
      EXPECT_EQ(board.bus.vid.ra, 2u) << "vscroll offsets the fetched RA";
      checked = true;
    }
  }
  EXPECT_TRUE(checked) << "reached an active RA=0 cell";
}

namespace {
// Video RAM that answers the GA's video fetch (RamBus), pre-filled so every
// fetched byte is 0xFF → mode-1 pen 3 across the whole active display.
struct Vram {
  uint8_t cells[0x10000];
};
void vram_tick(void* self, const Bus* in, Bus* out) {
  Vram* r = static_cast<Vram*>(self);
  if (in->ram.fetch) out->ram.data = r->cells[in->ram.addr];
}
size_t vram_size(const void*) { return sizeof(Vram); }
Device vram_device(Vram* s) {
  return Device{s,
                "vram",
                vram_tick,
                [](void*) {},
                vram_size,
                [](const void*, void*) {},
                [](void*, const void*) {}};
}
}  // namespace

// Plus horizontal soft scroll: it shifts the BACKGROUND right by hscroll,
// pulling the reset carry (palette entry 0) in at the active left edge, while
// leaving sprites alone.
TEST(Video, PlusHscrollShiftsBackgroundCarry) {
  const uint8_t seq[] = {0x81};  // GA: mode 1

  auto vram = std::make_unique<Vram>();
  std::memset(vram->cells, 0xFF, sizeof(vram->cells));  // every pen = 3

  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> vmem(video_state_size());
  Device vdev = video_init(vmem.data());
  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());
  Injector inj{seq, static_cast<int>(sizeof(seq)),
               static_cast<int>(sizeof(seq))};

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, inj_device(&inj));
  board_add(&board, cdev);
  board_add(&board, vram_device(vram.get()));
  board_add(&board, adev);
  board_add(&board, vdev);
  board_reset(&board);
  asic_set_plugged(&adev, 1);
  crtc_attach_asic(&cdev, &adev);

  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&cdev, i, std_regs[i]);
  crtc_poke_reg(&cdev, 12, 0x30);

  bus_unlock(&board);
  bus_set_palette(&board, 0, 0xF, 0x0, 0xF);  // carry entry 0 = magenta
  bus_set_palette(&board, 3, 0xF, 0xF, 0x0);  // background pen 3 = yellow
  bus_pgwrite(&board, 0x6804, 4);             // hscroll = 4, vscroll = 0

  const int w = 768, h = 272;
  std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 3, 0);
  video_attach(&vdev, &gdev, fb.data(), w, h);
  video_attach_asic(&vdev, &adev);
  inj.tick = 0;

  VideoRegs vr{};
  for (int tick = 0; tick < 1400000 && vr.frames < 3; ++tick) {
    board_tick(&board);
    video_peek(&vdev, &vr);
  }
  ASSERT_GE(vr.frames, 2u);

  int magenta = 0, yellow = 0;
  for (int i = 0; i < w * h; ++i) {
    if (fb[i * 3] == 255 && fb[i * 3 + 1] == 0 && fb[i * 3 + 2] == 255)
      magenta++;
    if (fb[i * 3] == 255 && fb[i * 3 + 1] == 255 && fb[i * 3 + 2] == 0)
      yellow++;
  }
  EXPECT_GT(yellow, 0) << "the active background is pen 3 (yellow)";
  EXPECT_GT(magenta, 0)
      << "hscroll pulled the reset carry (entry 0 = magenta) in at the edge";
  EXPECT_LT(magenta, yellow) << "the carry is only the left-edge strip";
}
