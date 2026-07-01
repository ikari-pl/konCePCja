/* video.cpp — Gate Array video pixel path. See docs/hardware/video-device.md.
 * Decode formulas + palette taken from the legacy renderer; pixel-exact. */

#include "video.h"

#include <new>

#include "gate_array.h"
#include "memory.h"

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
        vid_hw_rgb(ink[pens[p]], &out[px * 3], &out[(px * 3) + 1], &out[(px * 3) + 2]);
        px++;
      }
    }
  }
  return px;
}

int vid_px_per_char(uint8_t mode) { return mode == 0 ? 4 : mode == 1 ? 8 : 16; }

void vid_render_frame(const uint8_t* ram, uint8_t mode, const uint8_t* ink,
                      uint16_t ma_start, uint8_t r1, uint8_t r6, uint8_t r9,
                      uint8_t* fb) {
  const int width = r1 * vid_px_per_char(mode);
  int y = 0;
  for (uint8_t row = 0; row < r6; ++row) {
    const uint16_t ma_base = static_cast<uint16_t>((ma_start + row * r1) & 0x3FFF);
    for (uint8_t ra = 0; ra <= r9; ++ra) {
      vid_render_line(ram, mode, ink, ma_base, ra, r1, fb + (static_cast<size_t>(y) * width * 3));
      y++;
    }
  }
}

}  // extern "C"

// --- Live video Device ---------------------------------------------------------

namespace {

struct video_state {
  const Device* gate_array = nullptr;
  const Device* mem = nullptr;
  uint8_t* fb = nullptr;
  int fb_w = 0, fb_h = 0;
  int cur_row = 0;      // active display line being filled
  int cur_col = 0;      // pixel x within the line
  bool in_line = false; // emitted active pixels on the current line?
  bool vsync_prev = false;
  uint8_t dispen_prev = 0;
  uint32_t frames = 0;
};

video_state* vself(void* self) { return static_cast<video_state*>(self); }

void video_tick(void* self, const Bus* in, Bus* out) {
  (void)out;
  video_state* v = vself(self);
  if (!v->gate_array || !v->mem || !v->fb) return;

  const bool vsync = in->vid.vsync;
  if (vsync && !v->vsync_prev) {  // frame boundary: rewind to the top
    v->frames++;
    v->cur_row = 0;
    v->cur_col = 0;
    v->in_line = false;
  }
  v->vsync_prev = vsync;

  if (!in->clk.crtc) return;  // one character per 1 MHz tick
  const uint8_t dispen = in->vid.dispen ? 1 : 0;

  if (dispen && v->cur_row < v->fb_h) {
    GateArrayRegs g{};
    ga_peek(v->gate_array, &g);
    const uint16_t ma = in->vid.ma;
    const uint8_t ra = in->vid.ra;
    for (uint8_t k = 0; k < 2; ++k) {  // two bytes per character
      const uint8_t byte = mem_read_ram(v->mem, vid_byte_addr(ma, ra, k));
      uint8_t pens[8];
      const int n = vid_decode(g.mode, byte, pens);
      for (int p = 0; p < n && v->cur_col < v->fb_w; ++p) {
        uint8_t* px = v->fb + ((static_cast<size_t>(v->cur_row) * v->fb_w + v->cur_col) * 3);
        vid_hw_rgb(g.ink[pens[p]], px, px + 1, px + 2);
        v->cur_col++;
      }
    }
    v->in_line = true;
  }

  if (!dispen && v->dispen_prev && v->in_line) {  // active portion of the line ended
    v->cur_row++;
    v->cur_col = 0;
    v->in_line = false;
  }
  v->dispen_prev = dispen;
}

void video_reset(void* self) {
  video_state* v = vself(self);
  v->cur_row = v->cur_col = 0;
  v->in_line = v->vsync_prev = false;
  v->dispen_prev = 0;
  v->frames = 0;
  // fb / device pointers persist (set by video_attach).
}

size_t video_dev_state_size(const void*) { return 1; }  // logical state only
void video_save(const void*, void* buf) { *static_cast<uint8_t*>(buf) = 1; }
void video_load(void*, const void*) {}

}  // namespace

extern "C" {

size_t video_state_size(void) { return sizeof(video_state); }

Device video_init(void* storage) {
  video_state* v = new (storage) video_state();
  return Device{v,          "video",    video_tick, video_reset,
                video_dev_state_size, video_save, video_load};
}

void video_attach(const Device* vid, const Device* gate_array, const Device* mem,
                  uint8_t* fb, int w, int h) {
  video_state* v = static_cast<video_state*>(vid->self);
  v->gate_array = gate_array;
  v->mem = mem;
  v->fb = fb;
  v->fb_w = w;
  v->fb_h = h;
}

void video_peek(const Device* vid, VideoRegs* out) {
  const video_state* v = static_cast<const video_state*>(vid->self);
  out->mode = 0;
  out->frames = v->frames;
  out->cur_row = v->cur_row;
}

}  // extern "C"
