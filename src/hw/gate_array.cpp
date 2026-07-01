/* gate_array.cpp — the CPC Gate Array Device (clock + raster interrupt slice).
 * See docs/hardware/gate-array-device.md. */

#include "gate_array.h"

#include <cstring>
#include <new>

namespace {

struct ga_state {
  uint8_t phase = 0;       // 0..15 master-cycle phase within the 1 us window
  uint8_t sl_count = 0;    // 6-bit HSYNC line counter (raster interrupt)
  uint8_t hs_count = 0;    // VSYNC-resync HSYNC countdown
  bool hsync_prev = false; // HSYNC level last cycle (rising-edge detect)
  bool vsync_prev = false; // VSYNC level last cycle
  bool irq_line = false;   // asserting the Z80 INT line?

  // Register side (software-visible; consumed by the video/memory later).
  uint8_t pen = 0;         // selected pen (0..15) or 16 = border
  uint8_t mode = 1;        // active screen mode (latched at HSYNC)
  uint8_t req_mode = 1;    // requested mode
  uint8_t rom_config = 0;  // mode/ROM register value (ROM enable bits 2,3)
  uint8_t ram_config = 0;  // 6128 RAM banking (function 3)
  uint8_t ink[17] = {0};   // 16 pens + border, each a 0..31 hardware colour
};

ga_state* self_of(void* self) { return static_cast<ga_state*>(self); }

// A Gate Array register write (I/O to A15=0/A14=1). Function = data>>6.
void ga_write(ga_state* g, uint8_t data) {
  switch (data >> 6) {
    case 0:  // pen select: 0..15, or the border (bit 4)
      g->pen = (data & 0x10) ? 16 : (data & 0x0F);
      break;
    case 1:  // set ink (hardware colour 0..31) for the selected pen
      g->ink[g->pen] = static_cast<uint8_t>(data & 0x1F);
      break;
    case 2:  // screen mode (bits 1:0, latched at HSYNC) + ROM enables + INT rearm
      g->req_mode = static_cast<uint8_t>(data & 0x03);
      g->rom_config = data;
      if (data & 0x10) { g->irq_line = false; g->sl_count = 0; }  // interrupt rearm
      break;
    case 3:  // 6128 RAM banking
      g->ram_config = data;
      break;
    default:
      break;
  }
}

void ga_tick(void* self, const Bus* in, Bus* out) {
  ga_state* g = self_of(self);

  // --- clock generation: divide the 16 MHz master ---
  out->clk.phase = g->phase;
  out->clk.cpu = (g->phase & 0x03) == 0;  // 4 MHz: 1 in 4
  out->clk.crtc = (g->phase == 0);        // 1 MHz
  out->clk.psg = (g->phase == 0);         // 1 MHz enable (further divided in the PSG)
  g->phase = static_cast<uint8_t>((g->phase + 1) & 0x0F);

  // --- raster interrupt: count HSYNCs, fire at 52, resync on VSYNC ---
  const bool hs_rise = in->vid.hsync && !g->hsync_prev;
  const bool vs_rise = in->vid.vsync && !g->vsync_prev;
  g->hsync_prev = in->vid.hsync;
  g->vsync_prev = in->vid.vsync;

  if (hs_rise) g->mode = g->req_mode;  // screen mode latches at the start of each line
  if (vs_rise) g->hs_count = 2;  // arm: resync 2 HSYNCs after VSYNC

  if (hs_rise) {
    g->sl_count = static_cast<uint8_t>((g->sl_count + 1) & 0x3F);
    if (g->sl_count == 52) {  // ~300 Hz
      g->irq_line = true;
      g->sl_count = 0;
    }
    if (g->hs_count != 0 && --g->hs_count == 0) {  // 2 HSYNCs after VSYNC
      if (g->sl_count >= 32) g->irq_line = true;    // save-margin: don't skip a due INT
      g->sl_count = 0;
    }
  }

  // --- interrupt acknowledge: the CPU's M1+IORQ ack cycle clears bit 5 and the line.
  //     (The Z80's real IORQ-ack cycle lands with the next slice; this path is exercised
  //      by a synthetic m1+iorq in tests until then.) ---
  if (in->cpu.m1 && in->cpu.iorq) {
    g->sl_count &= 0x1F;
    g->irq_line = false;
  }

  // --- I/O write: Gate Array select is A15=0, A14=1; function = data>>6. ---
  if (in->cpu.iorq && in->cpu.wr && (in->cpu.addr & 0xC000) == 0x4000) {
    ga_write(g, in->cpu.data);
  }

  if (g->irq_line) out->cpu.irq = true;  // wired-OR onto the shared INT line
}

void ga_reset(void* self) {
  ga_state* g = self_of(self);
  *g = ga_state{};  // all counters/edges cleared; mode defaults to 1 (per the struct)
}

size_t ga_dev_state_size(const void*) { return sizeof(ga_state) + 1; }
void ga_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;  // format version
  std::memcpy(b + 1, self, sizeof(ga_state));
}
void ga_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, sizeof(ga_state));
}

}  // namespace

extern "C" {

size_t ga_state_size(void) { return sizeof(ga_state); }

Device ga_init(void* storage) {
  ga_state* g = new (storage) ga_state();
  ga_reset(g);
  return Device{g,        "gate-array", ga_tick, ga_reset,
                ga_dev_state_size, ga_save, ga_load};
}

void ga_peek(const Device* dev, GateArrayRegs* out) {
  const ga_state* g = static_cast<const ga_state*>(dev->self);
  out->phase = g->phase;
  out->sl_count = g->sl_count;
  out->hs_count = g->hs_count;
  out->irq = g->irq_line ? 1 : 0;
  out->pen = g->pen;
  out->mode = g->mode;
  out->req_mode = g->req_mode;
  out->rom_config = g->rom_config;
  out->ram_config = g->ram_config;
  std::memcpy(out->ink, g->ink, sizeof(out->ink));
}

}  // extern "C"
