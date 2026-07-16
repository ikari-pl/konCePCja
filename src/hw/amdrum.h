/* amdrum.h — the Cheetah AmDrum DAC as a Device. THE SPEC:
 * docs/hardware/amdrum-device.md. An 8-bit sample latch on the uncontested
 * I/O space (&FFxx); the sound mixing is a host-side (analog domain)
 * interpretation of the latch. */
#ifndef KONCPC_HW_AMDRUM_H
#define KONCPC_HW_AMDRUM_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AmdrumRegs {
  uint8_t dac;     /* current DAC level (reset 128 = mid-scale silence) */
  uint8_t plugged; /* 1 while the peripheral sits on the expansion port */
} AmdrumRegs;

size_t amdrum_state_size(void);
Device amdrum_init(void* storage);
void amdrum_peek(const Device* dev, AmdrumRegs* out);

/* Model plugging/unplugging the expansion: unplugged, nothing decodes. */
void amdrum_set_plugged(const Device* dev, int on);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_AMDRUM_H */
