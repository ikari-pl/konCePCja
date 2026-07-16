/* mf2.h — the Multiface II as a Device. THE SPEC:
 * docs/hardware/multiface-device.md. Freeze cartridge on the expansion port:
 * STOP drives /NMI and pages 8K ROM + 8K RAM over 0x0000-0x3FFF (asserting
 * /ROMDIS + /RAMDIS), and a bus snooper shadows hardware writes into fixed
 * RAM cells so the freeze menu can read the machine's state. */
#ifndef KONCPC_HW_MF2_H
#define KONCPC_HW_MF2_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Mf2Regs {
  uint8_t plugged;   /* 1 while the cartridge sits on the expansion port */
  uint8_t active;    /* 1 while the ROM/RAM overlay is paged in */
  uint8_t invisible; /* 1 after a freeze session ends, until reset */
} Mf2Regs;

size_t mf2_state_size(void);
Device mf2_init(void* storage);
void mf2_peek(const Device* dev, Mf2Regs* out);

/* The 8K MF2 ROM (caller-owned live wiring, must outlive the attachment). */
void mf2_attach_rom(const Device* dev, const uint8_t* rom8k, size_t len);

/* Model plugging/unplugging the cartridge; unplugged, nothing decodes. */
void mf2_set_plugged(const Device* dev, int on);

/* The red STOP button: /NMI plus immediate page-in (spec §3). */
void mf2_stop(const Device* dev);

/* Read the freeze RAM (tests / DevTools): offset 0..0x1FFF. */
uint8_t mf2_ram_peek(const Device* dev, uint16_t offset);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_MF2_H */
