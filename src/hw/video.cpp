/* video.cpp — Gate Array video pixel path. See docs/hardware/video-device.md.
 * Decode formulas + palette taken from the legacy renderer; pixel-exact. */

#include "video.h"

namespace {

// CPC hardware colours 0..31 → 8-bit RGB. Channel levels 0.0/0.5/1.0 → 0/128/255.
// 27 distinct colours; 5 indices duplicate (see gate-array hardware ref).
constexpr uint8_t kPalette[32][3] = {
    {128, 128, 128}, {128, 128, 128}, {0, 255, 128}, {255, 255, 128},
    {0, 0, 128},     {255, 0, 128},   {0, 128, 128}, {255, 128, 128},
    {255, 0, 128},   {255, 255, 128}, {255, 255, 0}, {255, 255, 255},
    {255, 0, 0},     {255, 0, 255},   {255, 128, 0}, {255, 128, 255},
    {0, 0, 128},     {0, 255, 128},   {0, 255, 0},   {0, 255, 255},
    {0, 0, 0},       {0, 0, 255},     {0, 128, 0},   {0, 128, 255},
    {128, 0, 128},   {128, 255, 128}, {128, 255, 0}, {128, 255, 255},
    {128, 0, 0},     {128, 0, 255},   {128, 128, 0}, {128, 128, 255},
};

uint8_t bit(uint8_t byte, int n) { return static_cast<uint8_t>((byte >> n) & 1); }

}  // namespace

extern "C" {

uint16_t vid_byte_addr(uint16_t ma, uint8_t ra, uint8_t k) {
  return static_cast<uint16_t>(((ma & 0x3000) << 2) | ((ra & 0x07) << 11) |
                               ((ma & 0x03FF) << 1) | (k & 1));
}

int vid_decode(uint8_t mode, uint8_t b, uint8_t pens_out[8]) {
  switch (mode) {
    case 0:  // 2 px/byte, 4-bit pen
      pens_out[0] = static_cast<uint8_t>((bit(b, 7) << 3) | (bit(b, 5) << 2) |
                                         (bit(b, 3) << 1) | bit(b, 1));
      pens_out[1] = static_cast<uint8_t>((bit(b, 6) << 3) | (bit(b, 4) << 2) |
                                         (bit(b, 2) << 1) | bit(b, 0));
      return 2;
    case 1:  // 4 px/byte, 2-bit pen
      for (int k = 0; k < 4; ++k)
        pens_out[k] = static_cast<uint8_t>((bit(b, 7 - k) << 1) | bit(b, 3 - k));
      return 4;
    case 2:  // 8 px/byte, 1-bit pen, MSB first
    default:
      for (int k = 0; k < 8; ++k) pens_out[k] = bit(b, 7 - k);
      return 8;
  }
}

void vid_hw_rgb(uint8_t colour, uint8_t* r, uint8_t* g, uint8_t* b) {
  const uint8_t* c = kPalette[colour & 0x1F];
  *r = c[0];
  *g = c[1];
  *b = c[2];
}

int vid_render_line(const uint8_t* ram, uint8_t mode, const uint8_t* ink,
                    uint16_t ma_base, uint8_t ra, uint8_t chars, uint8_t* out) {
  int px = 0;
  for (uint8_t ch = 0; ch < chars; ++ch) {
    const uint16_t ma = static_cast<uint16_t>((ma_base + ch) & 0x3FFF);
    for (uint8_t k = 0; k < 2; ++k) {  // 2 bytes per character
      uint8_t pens[8];
      const int n = vid_decode(mode, ram[vid_byte_addr(ma, ra, k)], pens);
      for (int p = 0; p < n; ++p) {
        vid_hw_rgb(ink[pens[p]], &out[px * 3], &out[px * 3 + 1], &out[px * 3 + 2]);
        px++;
      }
    }
  }
  return px;
}

}  // extern "C"
