/* memory.h — the CPC memory map as a Device. See docs/hardware/memory-device.md.
 *
 * The 64K Z80 address space backed by RAM, with the lower (firmware) and upper
 * (BASIC/AMSDOS) ROMs overlaid when the Gate Array enables them. This Device
 * independently watches the GA's ROM/RAM banking I/O writes (it does not share GA
 * state) and answers every mreq read/write. 6128 RAM banking is a later slice.
 *
 * Caller-owned, no heap: the ROM/RAM live in the caller's storage. */
#ifndef KONCPC_HW_MEMORY_H
#define KONCPC_HW_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MemRegs {
  uint8_t rom_config;  /* GA mode register: bit2 = lower-ROM disable, bit3 = upper-ROM disable */
  uint8_t ram_config;  /* 6128 RAM banking (function 3) */
} MemRegs;

size_t mem_state_size(void);
Device mem_init(void* storage);
void mem_peek(const Device* dev, MemRegs* out);

/* Load ROM contents (up to 16K each). Persist across reset (they are the firmware). */
void mem_load_lower_rom(const Device* dev, const uint8_t* data, size_t len);
void mem_load_upper_rom(const Device* dev, const uint8_t* data, size_t len);
/* Direct RAM access for tests / loaders (bypasses the bus). */
void mem_write_ram(const Device* dev, uint16_t addr, uint8_t val);
uint8_t mem_read_ram(const Device* dev, uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_MEMORY_H */
