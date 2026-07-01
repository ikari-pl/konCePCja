/* video_test.cpp — the Gate Array pixel path: address mapping, per-mode decode,
 * palette, and an active-line render. Golden data. See docs/hardware/video-device.md. */

#include <cstdint>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "hw/video.h"

namespace {
// A mode-0 byte whose two pixels are both `pen` (4-bit): px0/px1 bits interleave as
// b7b6=p3, b5b4=p2, b3b2=p1, b1b0=p0.
uint8_t mode0_solid(uint8_t pen) {
  const uint8_t p3 = (pen >> 3) & 1, p2 = (pen >> 2) & 1, p1 = (pen >> 1) & 1, p0 = pen & 1;
  return static_cast<uint8_t>((p3 << 7) | (p3 << 6) | (p2 << 5) | (p2 << 4) |
                              (p1 << 3) | (p1 << 2) | (p0 << 1) | p0);
}
}  // namespace

TEST(Video, AddressMapping) {
  EXPECT_EQ(vid_byte_addr(0x3000, 0, 0), 0xC000) << "screen base MA 0x3000 → RAM 0xC000";
  EXPECT_EQ(vid_byte_addr(0x3000, 0, 1), 0xC001) << "byte 1 of the character";
  EXPECT_EQ(vid_byte_addr(0x3000, 1, 0), 0xC800) << "RA=1 → +0x800 (2K scanline stride)";
  EXPECT_EQ(vid_byte_addr(0x3000, 7, 0), 0xF800) << "RA=7 → +7*0x800";
  EXPECT_EQ(vid_byte_addr(0x3001, 0, 0), 0xC002) << "MA+1 → +2 bytes";
}

TEST(Video, ModeDecode) {
  uint8_t pens[8];
  // Mode 0: 0xAA = 10101010 → px0 = (1,1,1,1)=15, px1 = (0,0,0,0)=0.
  EXPECT_EQ(vid_decode(0, 0xAA, pens), 2);
  EXPECT_EQ(pens[0], 15);
  EXPECT_EQ(pens[1], 0);
  // Mode 1: 0xA7 → 2,1,3,1 (from the reference).
  EXPECT_EQ(vid_decode(1, 0xA7, pens), 4);
  EXPECT_EQ(pens[0], 2); EXPECT_EQ(pens[1], 1); EXPECT_EQ(pens[2], 3); EXPECT_EQ(pens[3], 1);
  // Mode 2: 0x81 = 10000001 → 1,0,0,0,0,0,0,1 (MSB first).
  EXPECT_EQ(vid_decode(2, 0x81, pens), 8);
  EXPECT_EQ(pens[0], 1); EXPECT_EQ(pens[7], 1);
  for (int i = 1; i < 7; ++i) EXPECT_EQ(pens[i], 0);
}

TEST(Video, Palette) {
  uint8_t r, g, b;
  vid_hw_rgb(20, &r, &g, &b); EXPECT_EQ(r, 0);   EXPECT_EQ(g, 0);   EXPECT_EQ(b, 0);    // black
  vid_hw_rgb(11, &r, &g, &b); EXPECT_EQ(r, 255); EXPECT_EQ(g, 255); EXPECT_EQ(b, 255);  // white
  vid_hw_rgb(18, &r, &g, &b); EXPECT_EQ(r, 0);   EXPECT_EQ(g, 255); EXPECT_EQ(b, 0);    // bright green
  vid_hw_rgb(12, &r, &g, &b); EXPECT_EQ(r, 255); EXPECT_EQ(g, 0);   EXPECT_EQ(b, 0);    // bright red
  vid_hw_rgb(21, &r, &g, &b); EXPECT_EQ(r, 0);   EXPECT_EQ(g, 0);   EXPECT_EQ(b, 255);  // bright blue
}

TEST(Video, RenderActiveLine) {
  std::vector<uint8_t> ram(0x10000, 0);
  ram[vid_byte_addr(0x3000, 0, 0)] = 0xA7;  // mode-1 pens 2,1,3,1
  ram[vid_byte_addr(0x3000, 0, 1)] = 0x00;  // pens 0,0,0,0
  // ink[pen] = hardware colour: 0→black, 1→red, 2→green, 3→white.
  const uint8_t ink[17] = {20, 12, 18, 11};
  uint8_t out[8 * 3] = {0};
  const int px = vid_render_line(ram.data(), 1, ink, 0x3000, 0, /*chars=*/1, out);
  EXPECT_EQ(px, 8) << "one char in mode 1 = 2 bytes * 4 px";
  // First 4 pixels (byte 0 = 0xA7 → pens 2,1,3,1 → green,red,white,red):
  EXPECT_EQ(out[0], 0);   EXPECT_EQ(out[1], 255); EXPECT_EQ(out[2], 0);    // px0 green
  EXPECT_EQ(out[3], 255); EXPECT_EQ(out[4], 0);   EXPECT_EQ(out[5], 0);    // px1 red
  EXPECT_EQ(out[6], 255); EXPECT_EQ(out[7], 255); EXPECT_EQ(out[8], 255);  // px2 white
  EXPECT_EQ(out[9], 255); EXPECT_EQ(out[10], 0);  EXPECT_EQ(out[11], 0);   // px3 red
  // Next 4 (byte 1 = 0x00 → pens 0,0,0,0 → black):
  EXPECT_EQ(out[12], 0); EXPECT_EQ(out[13], 0); EXPECT_EQ(out[14], 0);     // px4 black
}

TEST(Video, RenderFrameColourBands) {
  // Mode 0 (16 colours, 160x200). 40 chars/row; column c shows pen (c % 16), so the
  // frame is 16 vertical colour bands. Distinct inks for the 16 pens.
  const uint8_t ink[17] = {20, 4, 21, 6, 22, 18, 19, 11, 12, 24, 13, 14, 15, 10, 30, 2, 0};
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
  // Pixel 0 = pen 0 = ink 20 = black; pixels 8..9 (char 1) = pen 1 = ink 4 = {0,0,128}.
  EXPECT_EQ(fb[0], 0); EXPECT_EQ(fb[1], 0); EXPECT_EQ(fb[2], 0);
  const int px_ch1 = 4;  // char 1 starts at pixel 4 (4 px/char in mode 0)
  EXPECT_EQ(fb[px_ch1 * 3 + 2], 128) << "char 1 = pen 1 = ink 4 = blue";

  // Write a PPM artifact so the frame is viewable.
  if (FILE* f = std::fopen("/tmp/cpc_frame.ppm", "wb")) {
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    std::fwrite(fb.data(), 1, fb.size(), f);
    std::fclose(f);
  }
}
