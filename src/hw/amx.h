/* amx.h — the AMX mouse as a Device. THE SPEC:
 * docs/hardware/amx-mouse-device.md. A joystick-port quadrature mouse: it
 * watches the row-select lines and drives matrix row 9's external column
 * lines; a direction bit pulses LOW for one mickey per deselect/reselect
 * cycle (the interface's monostable). */
#ifndef KONCPC_HW_AMX_H
#define KONCPC_HW_AMX_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AmxRegs {
  uint8_t plugged;
  int16_t mickeys_x; /* pending horizontal mickeys (signed) */
  int16_t mickeys_y; /* pending vertical mickeys (signed) */
  uint8_t buttons;   /* host mask: bit0 left, bit1 middle, bit2 right */
} AmxRegs;

size_t amx_state_size(void);
Device amx_init(void* storage);
void amx_peek(const Device* dev, AmxRegs* out);

/* Whole mickeys + button mask from the host (sub-pixel accumulation is the
 * host's job — spec §3). Buttons: bit0 left, bit1 middle, bit2 right. */
void amx_feed(const Device* dev, int dx, int dy, uint8_t buttons);

/* Model plugging/unplugging the connector. */
void amx_set_plugged(const Device* dev, int on);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_AMX_H */
