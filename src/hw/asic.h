/* asic.h — the Amstrad Plus / 6128+ ASIC as a Device. THE SPEC:
 * docs/hardware/asic-device.md. A chipset variant selected on model=3: the
 * classic GA/CRTC/video Devices enter Plus mode and this Device owns the
 * Plus-only state that has no classic analog. INCREMENT 1: the register-page
 * control plane — the unlock knock FSM and the &4000-&7FFF register-page
 * overlay. INCREMENT 1-A: the register decode — sprites (16×16 4-bit pixels +
 * X/Y/magnification attributes), the 32-entry 12-bit palette, and the DMA
 * channel registers (source, prescaler, enable) parsed into structured state.
 * The DMA sequencer, sprite compositing and 12-bit palette rendering consume
 * this state in later increments (video seam, DMA sound).
 *
 * No new bus lines: the register page rides the memory Device's /RAMDIS
 * overlay + one-tick write latch (memory-device.md §4b), like the Multiface. */
#ifndef KONCPC_HW_ASIC_H
#define KONCPC_HW_ASIC_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsicRegs {
  uint8_t plugged;       /* model==3: the ASIC is on the board */
  uint8_t locked;        /* register page hidden until the knock unlocks it */
  uint8_t page_on;       /* RMR2 has paged the register page into &4000-&7FFF */
  uint8_t pri_line;      /* programmable raster interrupt scanline (&6800) */
  uint8_t split_line;    /* split-screen scanline (&6801) */
  uint16_t split_addr;   /* split-screen display address (&6802-3) */
  uint8_t hscroll;       /* soft-scroll H delay (&6804 bits 0-3) */
  uint8_t vscroll;       /* soft-scroll V offset (&6804 bits 4-6) */
  uint8_t extend_border; /* &6804 bit 7 */
  uint8_t int_vector;    /* IM2 interrupt vector (&6805 & 0xF8) */
} AsicRegs;

size_t asic_state_size(void);
Device asic_init(void* storage);

/* model==3 puts the ASIC on the board (gates all decode). A hardware strap set
 * at board build, like crtc_set_type. */
void asic_set_plugged(const Device* dev, int on);

/* --- Intra-chip video interface -----------------------------------------
 * The Plus integrates the Gate Array, CRTC, video pixel path and these ASIC
 * extras onto ONE die, so the video and CRTC Devices read this state directly
 * (via video_attach_asic / crtc_attach_asic) rather than over the board bus —
 * exactly as the video Device already reads the Gate Array's ink/mode through
 * ga_peek. These are internal chip connections, NOT board `Bus` pins and NOT
 * debug peeks. Callers read them at a sync boundary (per frame in the CRTC,
 * per scanline in the video Device) so a same-tick register write settles one
 * boundary later, keeping the read order-independent w.r.t. the ASIC's tick. */

/* Is the video half live (the ASIC is plugged: model 3)? Gates the Plus path.
 */
int asic_vid_active(const Device* dev);

/* Is the register page unlocked? (Plus cartridge RMR2 low-ROM remap is live
 * only once the firmware knocks the ASIC open.) Read by the memory Device. */
int asic_unlocked(const Device* dev);

/* Split screen: the display base swaps to *addr at scanline *line (0 = off). */
void asic_vid_split(const Device* dev, uint8_t* line, uint16_t* addr);

/* Whether the programmable raster interrupt is active (plugged and pri_line
 * set). The Gate Array reads this to defer its fixed 52-line interrupt to the
 * ASIC's programmable one (asic-device.md §5). */
int asic_vid_pri_active(const Device* dev);

/* Soft scroll (&6804): horizontal fine-scroll delay (0-15 pixels) and vertical
 * fine-scroll offset (0-7 scanlines). The CRTC applies vscroll to the fetch
 * address; the video Device applies hscroll to the background pens. Any
 * out-pointer may be null (asic-device.md §3). */
void asic_vid_scroll(const Device* dev, uint8_t* hscroll, uint8_t* vscroll);

/* Extend-border flag (&6804 bit 7): shifts the display window right by one
 * char when active. The CRTC reads this in disp_skew. */
int asic_vid_extend_border(const Device* dev);

/* Palette entry `index` (0..31) as 12-bit RGB packed 0x0RGB (4 bits each). */
uint16_t asic_vid_palette(const Device* dev, int index);

/* Write generation of the per-line video snapshot's inputs (sprite attrs,
 * palette incl. classic-ink snoops, scroll/config) — monotonic within a
 * session, re-keyed past all seen values by load. Equal generations imply a
 * byte-identical snapshot, so the video device may skip its per-line
 * re-read (F8 R17). */
uint32_t asic_vid_gen(const Device* dev);

/* Whether palette entry `index` has been programmed by the Plus. Unset entries
 * read as the classic ink colour in the video Device (one palette RAM, Plus
 * writes override classic — asic_set_palette semantics). */
int asic_vid_palette_set(const Device* dev, int index);

/* Sprite `id` (0..15) pixel at (x,y) (0..15): 4-bit palette index, 0 =
 * transparent. The compositing seam maps a non-zero index n to palette entry
 * 16+n (the sprite palette bank). */
uint8_t asic_vid_sprite_pixel(const Device* dev, int id, int x, int y);

/* Sprite `id` attributes: X (10-bit), Y (9-bit), per-axis magnification
 * (0/1/2/4). Any out-pointer may be null. */
void asic_vid_sprite_attr(const Device* dev, int id, uint16_t* x, uint16_t* y,
                          uint8_t* mag_x, uint8_t* mag_y);

/* --- Debug / DevTools introspection -------------------------------------
 * Read-only windows for tests and the debugger, NOT the render path. */

/* Full control-register snapshot (lock/plugged/split/scroll/PRI/vector). */
void asic_peek(const Device* dev, AsicRegs* out);

/* One raw byte of the register page; offset 0..0x3FFF. */
uint8_t asic_page_peek(const Device* dev, uint16_t offset);

/* Position in the unlock-knock FSM (debug display). */
int asic_lock_pos(const Device* dev);

/* DMA channel debug view beyond asic_dma_regs: loop target, remaining loop
 * iterations, PAUSE countdown, prescaler sub-counter, and the channel's INT
 * flag. Any out-pointer may be null. */
void asic_dma_debug(const Device* dev, int ch, uint16_t* loop_addr,
                    uint16_t* loops, uint16_t* pause_ticks,
                    uint16_t* tick_cycles, uint8_t* irq);

/* DMA channel `ch` (0..2) register view: source address (word-aligned),
 * prescaler, and enable bit. Any out-pointer may be null. */
void asic_dma_regs(const Device* dev, int ch, uint16_t* source,
                   uint8_t* prescaler, uint8_t* enabled);

/* --- Fast-tier batch seam (asic-device.md §batch, plan §4.8) ---
 *
 * F7 scope: the register page, PRI, split/scroll/palette/sprites and the
 * unlock knock run under the Fast tier as events; the DMA sound sequencer
 * does NOT (it steals bus masters from the CPU at M-cycle boundaries the
 * batch driver does not retro-track) — frames with a DMA channel enabled run
 * on the per-cycle tiers, and a mid-frame enable bails the fast frame. */

/* One I/O WRITE event: the unlock-knock snoop (CRTC-select decode, one FSM
 * feed per access), the classic Gate-Array palette snoop, and the RMR2
 * register-page map — asic_tick's three iorq-write arms. */
void asic_fast_io_write(const Device* dev, uint16_t port, uint8_t val);
/* The register-page overlay as a memory event: nonzero = the ASIC claims the
 * access (unlocked, paged in, &4000-&7FFF) — reads answer from the page,
 * writes decode once and veto the RAM underneath (the caller skips the
 * memory seam). */
int asic_fast_mem_read(const Device* dev, uint16_t addr, uint8_t* out);
int asic_fast_mem_write(const Device* dev, uint16_t addr, uint8_t val);
/* Nonzero when the register page would claim &4000-&7FFF accesses right now
 * (plugged, unlocked, paged in) — the cheap pre-test the Fast tier's write
 * filter uses before deciding whether a write needs render catch-up. */
int asic_page_armed(const Device* dev);
/* Apply one CRTC frame-line value (the chain feeds every per-char view):
 * fires the programmable raster interrupt on the reach-edge, mirroring
 * asic_irq's per-cycle detection. */
void asic_batch_frame_line(const Device* dev, uint16_t line);
/* Current INT assertion (PRI or a DMA channel flag). */
int asic_irq_asserted(const Device* dev);
/* The CPU's interrupt acknowledge: returns the IM2 vector the ASIC drives
 * (priority: raster > DMA2 > DMA1 > DMA0) and clears the sources — exactly
 * asic_irq's m1+iorq arm. Call only when plugged. */
uint8_t asic_batch_int_ack(const Device* dev);
/* Nonzero while any DMA channel is enabled — the Fast tier's degrade/bail
 * signal (see the scope note above). */
int asic_dma_active(const Device* dev);
/* Tier handover: land the edge-detector shadows (HSYNC level for the DMA
 * sequencer, the current frame line for the PRI, and the strobe edges clear)
 * so the first per-cycle tick after a batch run sees no false edge. */
void asic_batch_set_sync(const Device* dev, int hsync, uint16_t frame_line);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_ASIC_H */
