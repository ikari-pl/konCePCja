/* crtc.h — the CPC CRTC (6845) as a Device. See docs/hardware/crtc-device.md.
 *
 * First slice: the character-timing engine — the H/V counters generating HSYNC,
 * VSYNC, DISPEN, and the MA/RA video address. Rendering is the video backend's
 * job.
 *
 * Caller-owned, no heap: allocate crtc_state_size() bytes, hand them to
 * crtc_init(). */
#ifndef KONCPC_HW_CRTC_H
#define KONCPC_HW_CRTC_H

#include <stddef.h>
#include <cstdint>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Introspection snapshot (tests / debugging). */
typedef struct CrtcRegs {
  uint16_t hcc;   /* horizontal char counter (0..R0) */
  uint8_t ra;     /* raster/scanline in the char row (0..R9) */
  uint8_t vcc;    /* char-row counter (0..R4) */
  uint16_t ma;    /* current 14-bit memory address */
  uint8_t hsync;  /* HSYNC asserted */
  uint8_t vsync;  /* VSYNC asserted */
  uint8_t dispen; /* display enable (active area) */
  uint8_t reg_select;
  uint8_t type; /* 0=HD6845S, 1=UM6845R, 2=MC6845, 3=AMS40489 */
  uint8_t reg[18];
  uint16_t scanline; /* frame scanline counter (0 at frame top) */
  uint8_t hsw;       /* HSYNC width counter (chars into the sync) */
  uint8_t vsw;       /* VSYNC width counter (scanlines into the sync) */
  uint8_t vta;       /* vertical-total-adjust scanline counter (R5) */
} CrtcRegs;

size_t crtc_state_size(void);
Device crtc_init(void* storage);
void crtc_peek(const Device* dev, CrtcRegs* out);

/* Plus mode: give the CRTC a reference to the ASIC Device so it can apply the
 * hardware split screen (the display base swaps to split_addr at split_line).
 * Self-gated on the ASIC being plugged, so it is a no-op on models 0-2. */
void crtc_attach_asic(const Device* dev, const Device* asic);
/* Set a register directly (test convenience; bypasses the &BC/&BD I/O path). */
void crtc_poke_reg(const Device* dev, uint8_t idx, uint8_t val);

/* Restore internal beam counters (SNA v3). flags: bit0 vsync active, bit1 hsync
 * active, bit7 vertical-total-adjust. */
void crtc_restore_v3(const Device* dev, uint16_t ma, uint16_t scanline,
                     uint8_t hcc, uint8_t vcc, uint8_t ra, uint8_t hsw,
                     uint8_t vsw, uint8_t flags);

/* --- Fast-tier batch engine (crtc-device.md §batch, plan §4.5) ---
 *
 * The CRTC's evolution is independent of the CPU between register writes, so
 * the Fast scheduler advances it in whole 1 MHz characters — the same
 * crtc_char step the per-cycle tick runs, minus the bus — collecting the sync
 * edges the Gate Array consumes, each timestamped with its character index.
 * Time mapping (the two-phase bus's one-hop latency, derived in
 * fast_video_chain_test.cpp): char k of the run executes at master 16k+1; a
 * GA irq change caused by its edges is CPU-visible at T-state 4k+1; an I/O
 * write whose T1 is T-state tau applies after char floor(tau/4). */
typedef enum CrtcEdgeKind : std::uint8_t {
  CRTC_EDGE_HSYNC_RISE = 0,
  CRTC_EDGE_HSYNC_FALL = 1,
  CRTC_EDGE_VSYNC_RISE = 2,
  CRTC_EDGE_VSYNC_FALL = 3,
} CrtcEdgeKind;

typedef struct CrtcEdge {
  uint32_t at;  /* char index within this advance call (0 = first char) */
  uint8_t kind; /* CrtcEdgeKind */
} CrtcEdge;

/* Advance `chars` characters, appending sync edges to out[0..max_out).
 * Returns the TOTAL number of edges produced (may exceed max_out — the state
 * still advances; size max_out at >= 4 * scanlines to never truncate). */
int crtc_advance_chars(const Device* dev, uint32_t chars, CrtcEdge* out,
                       int max_out);

/* Per-char view for the catch-up video renderer (F5): everything the bus
 * would carry to the video device for one character — the (vscroll-adjusted)
 * fetch address, the post-char sync/DISPEN levels, and this char's edges.
 * `mode` is NOT the CRTC's to fill: the chain scheduler stamps the GA's
 * latched screen mode into it while applying the edge stream (the latch
 * moves at HSYNC — video-device.md §batch). */
typedef struct CrtcCharView {
  uint16_t ma;
  uint8_t ra;
  uint8_t levels;      /* bit0 hsync, bit1 vsync, bit2 dispen (post-char) */
  uint8_t edges;       /* CrtcEdgeKind bitmask of this char's transitions */
  uint8_t mode;        /* chain-stamped GA mode (left 0 by crtc_advance_view) */
  uint16_t frame_line; /* post-char frame scanline (the ASIC PRI/split ref) */
} CrtcCharView;
enum : std::uint8_t {
  CRTC_LVL_HSYNC = 1,
  CRTC_LVL_VSYNC = 2,
  CRTC_LVL_DISPEN = 4,
};

/* Advance `chars` characters filling one view each — the superset of
 * crtc_advance_chars for consumers that also render. out must hold `chars`
 * entries. Returns the total edge count (for the caller's edge accounting). */
int crtc_advance_view(const Device* dev, uint32_t chars, CrtcCharView* out);

/* IRQ horizon (Fast scheduler): the number of characters h >= 1 such that
 * advancing h-1 chars from the CURRENT state produces no INT-path event — no
 * HSYNC fall (the GA's 52-line counter step) and no scanline end (frame_line
 * advance: the ASIC PRI edge-detect reference, and the char VSYNC rises on).
 * Char h-1 (the h-th) is the next candidate event char, inclusive. Any CRTC
 * register write invalidates a previously returned horizon (R0/R2/R7 moves
 * the geometry) — recompute after every applied I/O event. */
uint32_t crtc_irq_horizon_chars(const Device* dev);

/* Apply one I/O access as an event — the identical &BC/&BD/&BE/&BF decode
 * crtc_tick snoops from the bus. The caller must have caught the CRTC up to
 * the access time first (catch-up-then-apply). The write form returns a
 * CrtcEdgeKind bitmask of edges the write itself caused (an R7 hit can start
 * VSYNC immediately); the read form returns nonzero and fills *out only when
 * the CRTC drives the bus (type-dependent readable windows). */
uint8_t crtc_fast_io_write(const Device* dev, uint16_t port, uint8_t val);
int crtc_fast_io_read(const Device* dev, uint16_t port, uint8_t* out);

/* Apply one LPEN strobe sample at the current character — the batch twin of
 * crtc_tick's pen.strobe handler. `level` is the strobe line as of THIS char
 * (the caller ticks the light gun per char and passes its out->pen.strobe);
 * on the rising edge (false→true) the current ma latches into R16/R17,
 * exactly mirroring the per-cycle latch. Edge-detected: holding level high
 * does not re-latch, and a true→false re-arms the next rising edge. Call
 * AFTER advancing the CRTC to this char (crtc_advance_view et al.) so c->ma
 * is the char's fetch address, matching crtc_tick's post-crtc_char strobe
 * check. */
void crtc_fast_lpen_strobe(const Device* dev, bool level);
/* Select which 6845 variant is soldered in (0..3, default 0). A hardware strap:
 * persists across reset. Program-visible differences: register readability, the
 * type-1 status register, R3 sync widths, R8 DISPTMG skew (see the spec §5). */
void crtc_set_type(const Device* dev, uint8_t type);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_CRTC_H */
