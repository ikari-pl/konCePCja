/* smartwatch.h — the Dobbertin SmartWatch (Dallas DS1216) as a Device.
 * THE SPEC: docs/hardware/smartwatch-device.md. A phantom RTC in the upper
 * ROM socket: address-encoded serial protocol (A0 = data, A2 = mode),
 * answers by overriding data bit D0 under /ROMDIS. */
#ifndef KONCPC_HW_SMARTWATCH_H
#define KONCPC_HW_SMARTWATCH_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SmartwatchRegs {
  uint8_t plugged;
  uint8_t state;     /* 0 = idle, 1 = matching, 2 = reading */
  uint8_t bit_index; /* 0..63 within the current phase */
} SmartwatchRegs;

size_t smartwatch_state_size(void);
Device smartwatch_init(void* storage);
void smartwatch_peek(const Device* dev, SmartwatchRegs* out);

/* The host clock, in the DS1216's own register layout (8 BCD bytes:
 * hundredths, seconds, minutes, hours | 0x80 for 24h mode, day-of-week,
 * day, month, year). The FSM latches the CURRENT value on a pattern match
 * (spec §4); the host refreshes it whenever it likes. */
void smartwatch_set_time(const Device* dev, const uint8_t bcd[8]);

/* Model plugging/unplugging the socket adapter. */
void smartwatch_set_plugged(const Device* dev, int on);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_SMARTWATCH_H */
