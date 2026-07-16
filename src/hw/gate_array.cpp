/* gate_array.cpp — the CPC Gate Array Device (clock + raster interrupt slice).
 * See docs/hardware/gate-array-device.md. */

#include "gate_array.h"

#include <cstddef>
#include <cstring>
#include <new>

#include "asic.h"  // Plus PRI deference (asic_vid_pri_active)
#include "video.h"  // vid_byte_addr — the GA owns the MA/RA → RAM address shuffle

namespace {

struct ga_state {
  uint8_t phase = 0;        // 0..15 master-cycle phase within the 1 us window
  uint8_t sl_count = 0;     // 6-bit HSYNC line counter (raster interrupt)
  uint8_t hs_count = 0;     // VSYNC-resync HSYNC countdown
  bool hsync_prev = false;  // HSYNC level last cycle (rising-edge detect)
  bool vsync_prev = false;  // VSYNC level last cycle
  bool irq_line = false;    // asserting the Z80 INT line?
  const Device* asic = nullptr;  // Plus: defer the 52-line int to the ASIC PRI

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
    case 2:  // screen mode (bits 1:0, latched at HSYNC) + ROM enables + INT
             // rearm
      // 6128+ RMR2: on a Plus with the register page unlocked, a function-2
      // write with bit5 (0x20) set is the low-ROM bank-remap register, NOT a
      // screen-mode/ROM/interrupt write. The memory Device acts on it
      // (remapping the low-ROM cartridge bank); the Gate Array leaves mode,
      // rom_config and the raster counter untouched. Mirrors the legacy
      // z80_OUT_handler case 2
      // (`if (!asic.locked && (val & 0x20)) …RMR2… else …MRER…`) and the memory
      // Device's own RMR2 gate. Without this, Plus raster handlers that page
      // ROM via RMR2 (data 0xB8/0xB0) would clobber the current mode and reset
      // the interrupt cadence, breaking the per-band mode/split pipeline.
      if (g->asic && asic_unlocked(g->asic) && (data & 0x20)) break;
      g->req_mode = static_cast<uint8_t>(data & 0x03);
      g->rom_config = data;
      if (data & 0x10) {
        g->irq_line = false;
        g->sl_count = 0;
      }  // interrupt rearm
      break;
    case 3:  // 6128 RAM banking
      g->ram_config = data;
      break;
    default:
      break;
  }
}

// Advance the 52-line raster-interrupt counter on an HSYNC edge, firing (unless
// the ASIC's PRI owns the interrupt) at line 52 and at the VSYNC-resync save
// margin. See gate-array-device.md §6.
void ga_raster_count(ga_state* g, bool ga_owns_int) {
  g->sl_count = static_cast<uint8_t>((g->sl_count + 1) & 0x3F);
  if (g->sl_count == 52) {  // ~300 Hz
    if (ga_owns_int) g->irq_line = true;
    g->sl_count = 0;
  }
  if (g->hs_count != 0 && --g->hs_count == 0) {  // 2 HSYNCs after VSYNC
    if (g->sl_count >= 32 && ga_owns_int)
      g->irq_line = true;  // save-margin: don't skip a due INT
    g->sl_count = 0;
  }
}

void ga_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  ga_state* g = self_of(self);

  // Clock generation, /WAIT quantiser, video-fetch slots — the pure-function-
  // of-phase outputs, shared with the wake scheduler's synthesized sleep
  // cycles (ga_clock_out in gate_array.h holds the documentation).
  ga_clock_out(g->phase, in->vid.ma, in->vid.ra, out);

  g->phase = static_cast<uint8_t>((g->phase + 1) & 0x0F);

  // --- raster interrupt: count HSYNCs, fire at 52, resync on VSYNC ---
  const bool hs_rise = in->vid.hsync && !g->hsync_prev;
  const bool hs_fall = !in->vid.hsync && g->hsync_prev;
  const bool vs_rise = in->vid.vsync && !g->vsync_prev;
  g->hsync_prev = in->vid.hsync;
  g->vsync_prev = in->vid.vsync;

  if (hs_rise)
    g->mode = g->req_mode;  // screen mode latches at the start of each line
  if (vs_rise) g->hs_count = 2;  // arm: resync 2 HSYNCs after VSYNC

  // In Plus mode with a programmable raster interrupt set, the GA's fixed
  // 52-line interrupt defers to the ASIC's (asic-device.md §5). The counter
  // still runs; only the firing is suppressed.
  if (hs_fall) ga_raster_count(g, !(g->asic && asic_vid_pri_active(g->asic)));

  // --- interrupt acknowledge: the CPU's M1+IORQ ack cycle clears bit 5 and the
  // line.
  //     (The Z80's real IORQ-ack cycle lands with the next slice; this path is
  //     exercised
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
  const Device* keep = g->asic;  // the ASIC attachment is board wiring, not
                                 // machine state — it survives a cold reset
                                 // (matches crtc_reset / ga_load). Losing it
                                 // here would silently disable Plus RMR2 gating
                                 // and PRI deference after any runtime reset.
  *g = ga_state{};  // all counters/edges cleared; mode defaults to 1 (per the
                    // struct)
  g->asic = keep;
}

size_t ga_dev_state_size(const void* /*unused*/) {
  return sizeof(ga_state) + 1;
}
void ga_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;  // format version
  std::memcpy(b + 1, self, sizeof(ga_state));
  // The ASIC attachment is board wiring, not logical state. Zero it in the
  // blob so two identically-driven machines serialise byte-identically (a
  // live heap pointer differs per allocation); load() keeps the live pointer,
  // so this is its exact inverse. Mirrors crtc_save.
  std::memset(b + 1 + offsetof(ga_state, asic), 0, sizeof(const Device*));
}
void ga_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] != 1) return;
  ga_state* g = self_of(self);
  const Device* keep = g->asic;  // board wiring survives a state load
  std::memcpy(self, b + 1, sizeof(ga_state));
  g->asic = keep;
}

}  // namespace

extern "C" {

size_t ga_state_size(void) { return sizeof(ga_state); }

Device ga_init(void* storage) {
  ga_state* g = new (storage) ga_state();
  ga_reset(g);
  return Device{g,       "gate-array", ga_tick, ga_reset, ga_dev_state_size,
                ga_save, ga_load};
}

void ga_attach_asic(const Device* dev, const Device* asic) {
  static_cast<ga_state*>(dev->self)->asic = asic;
}

void ga_advance(const Device* dev, uint64_t skipped_cycles) {
  // Wake-scheduler catch-up (gate_array.h): each skipped tick would have done
  // exactly `phase = (phase + 1) & 0x0F` — no sync edge (the CRTC was asleep),
  // no CPU I/O, no ack, so no counter/mode/irq change was possible.
  ga_state* g = static_cast<ga_state*>(dev->self);
  g->phase = static_cast<uint8_t>((g->phase + skipped_cycles) & 0x0F);
}

void ga_batch_hsync_rise(const Device* dev) {
  ga_state* g = static_cast<ga_state*>(dev->self);
  g->mode = g->req_mode;  // screen mode latches at the start of each line
}

void ga_batch_hsync_fall(const Device* dev) {
  ga_state* g = static_cast<ga_state*>(dev->self);
  ga_raster_count(g, !(g->asic && asic_vid_pri_active(g->asic)));
}

void ga_batch_vsync_rise(const Device* dev) {
  static_cast<ga_state*>(dev->self)->hs_count = 2;
}

void ga_batch_int_ack(const Device* dev) {
  ga_state* g = static_cast<ga_state*>(dev->self);
  g->sl_count &= 0x1F;
  g->irq_line = false;
}

void ga_fast_io_write(const Device* dev, uint16_t port, uint8_t val) {
  if ((port & 0xC000) == 0x4000)
    ga_write(static_cast<ga_state*>(dev->self), val);
}

int ga_irq_asserted(const Device* dev) {
  return static_cast<const ga_state*>(dev->self)->irq_line ? 1 : 0;
}

void ga_batch_set_sync(const Device* dev, int hsync, int vsync) {
  ga_state* g = static_cast<ga_state*>(dev->self);
  g->hsync_prev = hsync != 0;
  g->vsync_prev = vsync != 0;
}

int ga_predict_irq_hsyncs(const Device* dev) {
  const ga_state* g = static_cast<const ga_state*>(dev->self);
  if (g->irq_line) return 0;
  if (g->asic && asic_vid_pri_active(g->asic)) return -1;  // PRI owns INT
  // Dry-run the counter rules on a copy — ga_raster_count is the ONE
  // definition; a shadow ga_state keeps this prediction incapable of drifting
  // from the committing path.
  ga_state shadow = *g;
  const int bound = 52 + 32 + 2;  // one full period + the resync margin
  for (int n = 1; n <= bound; ++n) {
    ga_raster_count(&shadow, true);
    if (shadow.irq_line) return n;
  }
  return -1;  // unreachable for a GA that owns its interrupt
}

uint8_t ga_current_mode(const Device* dev) {
  return static_cast<const ga_state*>(dev->self)->mode;
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

void ga_poke_counters(const Device* dev, uint8_t hs_count, uint8_t sl_count,
                      uint8_t irq_line) {
  ga_state* g = static_cast<ga_state*>(dev->self);
  g->hs_count = static_cast<uint8_t>(hs_count & 3);
  g->sl_count = sl_count;
  g->irq_line = irq_line != 0;
}

}  // extern "C"
