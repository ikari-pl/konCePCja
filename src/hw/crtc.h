/* crtc.h — the CPC CRTC (6845) as a Device. See docs/hardware/crtc-device.md.
 *
 * First slice: the character-timing engine — the H/V counters generating HSYNC,
 * VSYNC, DISPEN, and the MA/RA video address. Rendering is the video backend's job.
 *
 * Caller-owned, no heap: allocate crtc_state_size() bytes, hand them to crtc_init(). */
#ifndef KONCPC_HW_CRTC_H
#define KONCPC_HW_CRTC_H

#include <stddef.h>
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
  uint8_t reg[18];
} CrtcRegs;

size_t crtc_state_size(void);
Device crtc_init(void* storage);
void crtc_peek(const Device* dev, CrtcRegs* out);
/* Set a register directly (test convenience; bypasses the &BC/&BD I/O path). */
void crtc_poke_reg(const Device* dev, uint8_t idx, uint8_t val);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_CRTC_H */
