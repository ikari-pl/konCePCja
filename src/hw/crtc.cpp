/* crtc.cpp — the CPC CRTC (6845) character-timing engine.
 * See docs/hardware/crtc-device.md. Type 0/1 timing; rendering lives elsewhere. */

#include "crtc.h"

#include <cstring>
#include <new>

namespace {

struct crtc_state {
  uint8_t reg[18] = {0};
  uint8_t reg_select = 0;
  uint16_t hcc = 0;      // horizontal char counter (0..R0)
  uint8_t ra = 0;        // raster within char row (0..R9)
  uint8_t vcc = 0;       // char-row counter (0..R4)
  uint8_t hsw = 0;       // HSYNC width counter (chars)
  uint8_t vsw = 0;       // VSYNC width counter (scanlines)
  uint8_t vta = 0;       // vertical-total-adjust scanline counter (R5)
  bool in_hsync = false;
  bool in_vsync = false;
  bool in_vta = false;
  bool dispen = false;
  uint16_t ma_row = 0;   // char-row base address
  uint16_t ma = 0;       // current memory address
};

crtc_state* self_of(void* self) { return static_cast<crtc_state*>(self); }

void crtc_newframe(crtc_state* c) {
  c->vcc = 0;
  c->ra = 0;
  c->in_vta = false;
  c->vta = 0;
  c->ma_row = static_cast<uint16_t>(((c->reg[12] << 8) | c->reg[13]) & 0x3FFF);
  if (c->vcc == c->reg[7]) { c->in_vsync = true; c->vsw = 0; }  // R7==0 edge case
}

void crtc_newrow(crtc_state* c) {
  c->ma_row = static_cast<uint16_t>((c->ma_row + c->reg[1]) & 0x3FFF);  // += R1 per row
  if (c->vcc == c->reg[4]) {              // end of vertical total
    if (c->reg[5] != 0) { c->in_vta = true; c->vta = 0; }  // run R5 adjust scanlines
    else crtc_newframe(c);
    return;
  }
  c->vcc = static_cast<uint8_t>(c->vcc + 1);
  if (c->vcc == c->reg[7]) { c->in_vsync = true; c->vsw = 0; }  // VSYNC starts at row R7
}

void crtc_newscanline(crtc_state* c) {
  if (c->in_vsync) {  // VSYNC width is measured in scanlines
    uint8_t vwidth = static_cast<uint8_t>(c->reg[3] >> 4);
    if (vwidth == 0) vwidth = 16;
    if (++c->vsw >= vwidth) c->in_vsync = false;
  }
  if (c->in_vta) {  // vertical total adjust: R5 extra scanlines, then restart
    if (++c->vta >= c->reg[5]) crtc_newframe(c);
    return;
  }
  if (c->ra == c->reg[9]) {  // last scanline of the char row
    c->ra = 0;
    crtc_newrow(c);
  } else {
    c->ra = static_cast<uint8_t>(c->ra + 1);
  }
}

// One 1 MHz character cycle.
void crtc_char(crtc_state* c) {
  const uint8_t hwidth = static_cast<uint8_t>(c->reg[3] & 0x0F);
  if (c->hcc == c->reg[2] && hwidth != 0) {  // HSYNC starts at char R2
    c->in_hsync = true;
    c->hsw = 0;
  } else if (c->in_hsync) {
    if (++c->hsw >= hwidth) c->in_hsync = false;
  }

  c->ma = static_cast<uint16_t>((c->ma_row + c->hcc) & 0x3FFF);
  c->dispen = (c->hcc < c->reg[1]) && (c->vcc < c->reg[6]);

  if (c->hcc == c->reg[0]) {  // end of scanline
    c->hcc = 0;
    crtc_newscanline(c);
  } else {
    c->hcc = static_cast<uint16_t>(c->hcc + 1);
  }
}

void crtc_tick(void* self, const Bus* in, Bus* out) {
  crtc_state* c = self_of(self);

  // Register I/O: select at &BCxx, write at &BDxx (upper address byte decodes).
  if (in->cpu.iorq && in->cpu.wr) {
    const uint8_t hi = static_cast<uint8_t>(in->cpu.addr >> 8);
    if (hi == 0xBC) c->reg_select = static_cast<uint8_t>(in->cpu.data & 0x1F);
    else if (hi == 0xBD && c->reg_select < 18) c->reg[c->reg_select] = in->cpu.data;
  }

  if (in->clk.crtc) crtc_char(c);  // advance one character per 1 MHz tick

  out->vid.hsync = c->in_hsync;
  out->vid.vsync = c->in_vsync;
  out->vid.dispen = c->dispen;
  out->vid.ma = c->ma;
  out->vid.ra = c->ra;
}

void crtc_reset(void* self) {
  crtc_state* c = self_of(self);
  std::memset(c->reg, 0, sizeof(c->reg));
  c->reg_select = 0;
  c->hcc = c->vcc = c->ra = 0;
  c->hsw = c->vsw = c->vta = 0;
  c->in_hsync = c->in_vsync = c->in_vta = c->dispen = false;
  c->ma_row = c->ma = 0;
}

size_t crtc_dev_state_size(const void*) { return sizeof(crtc_state); }
void crtc_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;  // format version
  std::memcpy(b + 1, self, sizeof(crtc_state));
}
void crtc_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, sizeof(crtc_state));
}

}  // namespace

extern "C" {

size_t crtc_state_size(void) { return sizeof(crtc_state) + 1; }

Device crtc_init(void* storage) {
  crtc_state* c = new (storage) crtc_state();
  crtc_reset(c);
  return Device{c,          "crtc",     crtc_tick, crtc_reset,
                crtc_dev_state_size, crtc_save, crtc_load};
}

void crtc_peek(const Device* dev, CrtcRegs* out) {
  const crtc_state* c = static_cast<const crtc_state*>(dev->self);
  out->hcc = c->hcc;
  out->ra = c->ra;
  out->vcc = c->vcc;
  out->ma = c->ma;
  out->hsync = c->in_hsync ? 1 : 0;
  out->vsync = c->in_vsync ? 1 : 0;
  out->dispen = c->dispen ? 1 : 0;
  out->reg_select = c->reg_select;
  std::memcpy(out->reg, c->reg, sizeof(out->reg));
}

void crtc_poke_reg(const Device* dev, uint8_t idx, uint8_t val) {
  crtc_state* c = static_cast<crtc_state*>(dev->self);
  if (idx < 18) c->reg[idx] = val;
}

}  // extern "C"
