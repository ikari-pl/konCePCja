/* gate_array.h — the CPC Gate Array as a Device. See
 * docs/hardware/gate-array-device.md.
 *
 * First slice: the clock generator (÷4 CPU / ÷16 CRTC/PSG from the 16 MHz
 * master) and the 300 Hz raster interrupt (HSYNC line counter, VSYNC resync).
 * The WAIT µs quantiser and the register side (palette / mode / banking) land
 * in later slices.
 *
 * Caller-owned, no heap: allocate ga_state_size() bytes, hand them to
 * ga_init(). */
#ifndef KONCPC_HW_GATE_ARRAY_H
#define KONCPC_HW_GATE_ARRAY_H

#include <stddef.h>
#include <stdint.h>

#include "buses.h"
#include "device.h"
#include "video.h"  // vid_byte_addr — the GA owns the MA/RA → RAM shuffle

#ifdef __cplusplus
extern "C" {
#endif

/* The GA's pure-function-of-phase bus outputs: the clock fabric (÷4 CPU,
 * ÷16 CRTC/PSG), the /WAIT µs quantiser, and the two video-fetch slots (drive
 * phase 12 → byte 0, phase 14 → byte 1 — a page-mode pair off the CRTC's
 * MA/RA, running every µs regardless of DISPEN, which is why CPC timing is
 * uniform). ONE definition shared by ga_tick (every live cycle) and the wake
 * scheduler's synthesized sleep cycles (machine.cpp tick_wake), so the two can
 * never drift. `phase` is the divider value being driven this cycle. */
static inline void ga_clock_out(uint8_t phase, uint16_t ma, uint8_t ra,
                                Bus* out) {
  out->clk.phase = phase;
  out->clk.cpu = (phase & 0x03) == 0; /* 4 MHz: 1 in 4 */
  out->clk.crtc = phase == 0;         /* 1 MHz */
  out->clk.psg = phase == 0;          /* 1 MHz enable (PSG divides further) */
  /* /WAIT released only during T-slot 1 of each µs: memory M-cycles never see
   * it (their held T1 puts T2 exactly in slot 1); I/O cycles stretch in the
   * auto-TW until the free slot — the CPC's uniform I/O timings. */
  out->cpu.wait = (phase >> 2) != 1;
  if (phase == 12 || phase == 14) {
    out->ram.fetch = true;
    out->ram.addr = vid_byte_addr(ma, ra, phase == 14 ? 1 : 0);
  }
}

/* Introspection snapshot (tests / debugging). */
typedef struct GateArrayRegs {
  uint8_t phase;      /* 0..15 master-cycle phase in the 1 us window */
  uint8_t sl_count;   /* 6-bit HSYNC line counter (raster interrupt) */
  uint8_t hs_count;   /* VSYNC-resync HSYNC countdown (2,1,0) */
  uint8_t irq;        /* 1 while the GA is asserting the Z80 INT line */
  uint8_t pen;        /* currently selected pen (0..15, or 16 = border) */
  uint8_t mode;       /* active screen mode 0..3 (latched at HSYNC) */
  uint8_t req_mode;   /* requested mode (takes effect next HSYNC) */
  uint8_t rom_config; /* mode/ROM register: bit2 lower-ROM dis, bit3 upper-ROM
                         dis */
  uint8_t ram_config; /* 6128 RAM banking (function 3) */
  uint8_t ink[17];    /* hardware colour (0..31) per pen 0..15 + border (16) */
} GateArrayRegs;

size_t ga_state_size(void);
Device ga_init(void* storage);
void ga_peek(const Device* dev, GateArrayRegs* out);

/* Restore raster-counter / IRQ latch state (SNA v3). irq_line: pending INT. */
void ga_poke_counters(const Device* dev, uint8_t hs_count, uint8_t sl_count,
                      uint8_t irq_line);

/* The latched screen mode alone — the batch scheduler stamps it on every
 * CRTC char view, where a full ga_peek struct copy is measurable overhead. */
uint8_t ga_current_mode(const Device* dev);

/* Plus mode: point the GA at the ASIC so its fixed 52-line raster interrupt
 * defers to the ASIC's programmable one when PRI is active. No-op (null) on
 * models 0-2. */
void ga_attach_asic(const Device* dev, const Device* asic);

/* Clock-wake contract (Gate B6 wake scheduler): on cycles with no sync
 * movement, no CPU I/O and no INT-ack, ga_tick's only state change is the ÷16
 * phase advance — its bus outputs are pure functions of that phase (clocks,
 * /WAIT, the two video-fetch slots), which the scheduler synthesizes verbatim.
 * A scheduler that skipped such cycles calls this before the next real tick so
 * the divider lands exactly where a per-cycle run would have put it. */
void ga_advance(const Device* dev, uint64_t skipped_cycles);

/* --- Fast-tier batch engine (gate-array-device.md §batch, plan §4.5) ---
 *
 * In the Fast tier the GA is edge-driven: the CRTC batch engine's timestamped
 * sync edges (crtc.h CrtcEdge) replace the per-cycle level sampling, CPU I/O
 * writes arrive as events (catch-up-then-apply), and the Z80 batch driver's
 * int_ack callback stands in for the M1+IORQ acknowledge cycle. The ÷16
 * phase/clock fabric needs no runtime at all — it is closed-form arithmetic
 * in the Z80 batch driver (the quantiser/Tw rules) and pure phase math for
 * anything else. */

/* One HSYNC rising edge: latch the requested screen mode. */
void ga_batch_hsync_rise(const Device* dev);
/* One HSYNC falling edge (HSYNC end): advance the 52-line raster counter,
 * deferring its fixed interrupt to the ASIC PRI when active. The physical
 * Gate Array clocks R52 at HSYNC end, not at HSYNC start. */
void ga_batch_hsync_fall(const Device* dev);
/* One VSYNC rising edge: arm the 2-HSYNC interrupt resync. Apply BEFORE a
 * same-char HSYNC edge (crtc_advance_chars emits VSYNC first). */
void ga_batch_vsync_rise(const Device* dev);
/* The CPU's INT-acknowledge cycle (Z80BatchIO.int_ack): clear counter bit 5
 * and drop the INT line — ga_tick's m1+iorq arm. */
void ga_batch_int_ack(const Device* dev);
/* Apply one I/O WRITE event — the identical A15=0/A14=1 decode ga_tick snoops
 * (pen/ink/mode+rearm/banking, with the Plus RMR2 deference). */
void ga_fast_io_write(const Device* dev, uint16_t port, uint8_t val);
/* Current INT line level (what the Z80 batch driver samples per boundary). */
int ga_irq_asserted(const Device* dev);
/* Tier handover: set the sync-level shadows (the edge detectors' previous
 * values) to the CRTC's live levels, so the first per-cycle tick after a
 * batch run sees no false edge. */
void ga_batch_set_sync(const Device* dev, int hsync, int vsync);
/* Horizon helper: after how many MORE HSYNC falling edges will the GA assert
 * INT, given the current counters? 0 = already asserting; -1 = never (PRI
 * owns the interrupt). Composes with the CRTC edge timestamps to give the
 * scheduler's next_irq_at. Pure — commits nothing. */
int ga_predict_irq_hsyncs(const Device* dev);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_GATE_ARRAY_H */
