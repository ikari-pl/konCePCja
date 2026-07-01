/* video_test.cpp — the Gate Array pixel path: address mapping, per-mode decode,
 * palette, and an active-line render. Golden data. See docs/hardware/video-device.md. */

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "hw/video.h"

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
