/* video.h — the Gate Array video pixel path (pure functions). See
 * docs/hardware/video-device.md. MA/RA→address, per-mode pixel decode, the CPC
 * 27-colour palette, and an active-line renderer. Pixel-exact vs the real GA. */
#ifndef KONCPC_HW_VIDEO_H
#define KONCPC_HW_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte k (0/1) of the character at CRTC (ma, ra) → RAM byte address. */
uint16_t vid_byte_addr(uint16_t ma, uint8_t ra, uint8_t k);

/* Decode one fetched video byte in screen `mode` (0/1/2) into pen indices
 * (left→right). Writes to pens_out and returns the pixel count: 2 / 4 / 8. */
int vid_decode(uint8_t mode, uint8_t byte, uint8_t pens_out[8]);

/* CPC hardware colour (0..31, what an ink register holds) → 8-bit RGB. */
void vid_hw_rgb(uint8_t colour, uint8_t* r, uint8_t* g, uint8_t* b);

/* Render one active character row's pixels: `chars` characters (2 bytes each)
 * from `ram` starting at CRTC address `ma_base`, scanline `ra`, decoded in
 * `mode`, each pen mapped through ink[0..16] to a hardware colour, written to
 * `out` as RGB triplets (3 bytes/pixel). Returns the pixel count. */
int vid_render_line(const uint8_t* ram, uint8_t mode, const uint8_t* ink,
                    uint16_t ma_base, uint8_t ra, uint8_t chars, uint8_t* out);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_VIDEO_H */
