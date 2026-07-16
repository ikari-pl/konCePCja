/* video.h — the Gate Array video pixel path (pure functions). See
 * docs/hardware/video-device.md. MA/RA→address, per-mode pixel decode, the CPC
 * 27-colour palette, and an active-line renderer. Pixel-exact vs the real GA.
 */
#ifndef KONCPC_HW_VIDEO_H
#define KONCPC_HW_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Byte k (0/1) of the character at CRTC (ma, ra) → RAM byte address. */
uint16_t vid_byte_addr(uint16_t ma, uint8_t ra, uint8_t k);

/* Decode one fetched video byte in screen `mode` (0/1/2) into pen indices
 * (left→right). Writes to pens_out and returns the pixel count: 2 / 4 / 8. */
int vid_decode(uint8_t mode, uint8_t byte, uint8_t pens_out[8]);

/* Same result as vid_decode(), served from a table built once from it (so it is
 * byte-identical by construction). The Fast-tier batch renderer (Gate B7) reads
 * this instead of re-running the per-pixel bit-scatter; the faithful per-cycle
 * path keeps using vid_decode(). Thread-safe first-call init. */
int vid_decode_lut(uint8_t mode, uint8_t byte, uint8_t pens_out[8]);

/* CPC hardware colour (0..31, what an ink register holds) → 8-bit RGB. */
void vid_hw_rgb(uint8_t colour, uint8_t* r, uint8_t* g, uint8_t* b);

/* Render one active character row's pixels: `chars` characters (2 bytes each)
 * from `ram` starting at CRTC address `ma_base`, scanline `ra`, decoded in
 * `mode`, each pen mapped through ink[0..16] to a hardware colour, written to
 * `out` as RGB triplets (3 bytes/pixel). Returns the pixel count. */
int vid_render_line(const uint8_t* ram, uint8_t mode, const uint8_t* ink,
                    uint16_t ma_base, uint8_t ra, uint8_t chars, uint8_t* out);

/* Displayed pixels per CRTC character (2 bytes): 4 / 8 / 16 for mode 0 / 1 / 2.
 */
int vid_px_per_char(uint8_t mode);

/* Render the full active display into `fb` (RGB, width*height*3 where
 * width = r1 * vid_px_per_char(mode), height = r6 * (r9+1)). Static snapshot:
 * the MA advances by r1 per character row, ra covers 0..r9. No mid-frame
 * changes. */
void vid_render_frame(const uint8_t* ram, uint8_t mode, const uint8_t* ink,
                      uint16_t ma_start, uint8_t r1, uint8_t r6, uint8_t r9,
                      uint8_t* fb);

/* --- Live video Device (the GA's pixel serializer) ---
 * Consumes the display bytes the Gate Array fetches on the RAM bus (Bus.ram —
 * two page-mode reads per µs during the video slots), watches the CRTC's
 * sync/DISPEN timing, and paints the full visible raster (active display +
 * border) into the caller's framebuffer as the board runs. No back-channel:
 * every displayed byte travelled over the bus. `frames` counts completed frames
 * (VSYNC edges). */
typedef struct VideoRegs {
  uint8_t mode;
  uint32_t frames; /* completed frames */
  int cur_row;     /* active line currently being filled */
} VideoRegs;

size_t video_state_size(void);
Device video_init(void* storage);
/* Point the renderer at the GA (palette/mode) and a framebuffer (RGB, w*h*3).
 * The display bytes themselves arrive over the RAM fetch bus, not through any
 * pointer. */
void video_attach(const Device* vid, const Device* gate_array, uint8_t* fb,
                  int w, int h);

/* Plus mode: give the renderer a reference to the ASIC Device. When the ASIC
 * is plugged, the pixel path switches to the 12-bit palette and composites the
 * 16 hardware sprites per beam pixel. Pass null (or leave unattached) for the
 * classic models. */
void video_attach_asic(const Device* vid, const Device* asic);

void video_peek(const Device* vid, VideoRegs* out);

/* --- Fast-tier catch-up renderer (video-device.md §batch, plan §4.4) ---
 *
 * Consume a run of CRTC character views (crtc.h CrtcCharView — the chain
 * stamps the GA's latched mode into each) and paint the SAME beam/framebuffer
 * state the per-cycle video_tick drives: per view, the char's sync edges move
 * the beam (VSYNC rise = new frame + back porch, HSYNC rise = next scanline,
 * HSYNC fall = left edge), then the cell paints from `ram` (the display
 * fetch's un-banked base-64K window, mem_video_ram) through the GA's CURRENT
 * inks. The caller upholds catch-up-then-apply: every write that could
 * change what a pending cell shows (GA inks, CRTC registers, RAM) renders
 * the cells before it first — one uniform apply point per microsecond
 * (everything with T1 in us j lands after char j's advance, before cell j's
 * render). Classic (non-Plus) path; the ASIC compositor batches in F7. */
void video_batch_cells(const Device* vid, const uint8_t* ram,
                       const struct CrtcCharView* views, int count);
/* Tier handover: set the sync-level shadows so the first per-cycle tick
 * after a batch run sees no false edge (mirrors ga_batch_set_sync). */
void video_batch_set_sync(const Device* vid, int hsync, int vsync);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_VIDEO_H */
