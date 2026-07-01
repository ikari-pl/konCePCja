/* gate_array.cpp — the CPC Gate Array Device (clock + raster interrupt slice).
 * See docs/hardware/gate-array-device.md. */

#include "gate_array.h"

#include <new>

namespace {

struct ga_state {
  uint8_t phase = 0;       // 0..15 master-cycle phase within the 1 us window
  uint8_t sl_count = 0;    // 6-bit HSYNC line counter (raster interrupt)
  uint8_t hs_count = 0;    // VSYNC-resync HSYNC countdown
  bool hsync_prev = false; // HSYNC level last cycle (rising-edge detect)
  bool vsync_prev = false; // VSYNC level last cycle
  bool irq_line = false;   // asserting the Z80 INT line?
};

ga_state* self_of(void* self) { return static_cast<ga_state*>(self); }

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

  // --- I/O write: Gate Array select is A15=0, A14=1; function = data>>6. Only the
  //     mode register's bit 4 (interrupt rearm) is handled in this slice. ---
  if (in->cpu.iorq && in->cpu.wr && (in->cpu.addr & 0xC000) == 0x4000) {
    const uint8_t d = in->cpu.data;
    if ((d >> 6) == 2 && (d & 0x10)) {  // mode/ROM register, interrupt-delay bit
      g->irq_line = false;
      g->sl_count = 0;
    }
  }

  if (g->irq_line) out->cpu.irq = true;  // wired-OR onto the shared INT line
}

void ga_reset(void* self) {
  ga_state* g = self_of(self);
  g->phase = 0;
  g->sl_count = 0;
  g->hs_count = 0;
  g->hsync_prev = false;
  g->vsync_prev = false;
  g->irq_line = false;
}

size_t ga_dev_state_size(const void*) { return sizeof(ga_state); }
void ga_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  const ga_state* g = static_cast<const ga_state*>(self);
  b[0] = 1;  // format version
  b[1] = g->phase;
  b[2] = g->sl_count;
  b[3] = g->hs_count;
  b[4] = static_cast<uint8_t>((g->hsync_prev ? 1 : 0) | (g->vsync_prev ? 2 : 0) |
                              (g->irq_line ? 4 : 0));
}
void ga_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  ga_state* g = self_of(self);
  if (b[0] != 1) return;
  g->phase = b[1];
  g->sl_count = b[2];
  g->hs_count = b[3];
  g->hsync_prev = (b[4] & 1) != 0;
  g->vsync_prev = (b[4] & 2) != 0;
  g->irq_line = (b[4] & 4) != 0;
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
}

}  // extern "C"
