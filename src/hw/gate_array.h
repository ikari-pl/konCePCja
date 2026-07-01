/* gate_array.h — the CPC Gate Array as a Device. See docs/hardware/gate-array-device.md.
 *
 * First slice: the clock generator (÷4 CPU / ÷16 CRTC/PSG from the 16 MHz master)
 * and the 300 Hz raster interrupt (HSYNC line counter, VSYNC resync). The WAIT µs
 * quantiser and the register side (palette / mode / banking) land in later slices.
 *
 * Caller-owned, no heap: allocate ga_state_size() bytes, hand them to ga_init(). */
#ifndef KONCPC_HW_GATE_ARRAY_H
#define KONCPC_HW_GATE_ARRAY_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Introspection snapshot (tests / debugging). */
typedef struct GateArrayRegs {
  uint8_t phase;     /* 0..15 master-cycle phase in the 1 us window */
  uint8_t sl_count;  /* 6-bit HSYNC line counter (raster interrupt) */
  uint8_t hs_count;  /* VSYNC-resync HSYNC countdown (2,1,0) */
  uint8_t irq;       /* 1 while the GA is asserting the Z80 INT line */
} GateArrayRegs;

size_t ga_state_size(void);
Device ga_init(void* storage);
void ga_peek(const Device* dev, GateArrayRegs* out);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_GATE_ARRAY_H */
