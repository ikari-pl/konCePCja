/* memory.h — the CPC memory map as a Device. See
 * docs/hardware/memory-device.md.
 *
 * The 64K Z80 address space backed by RAM, with the lower (firmware) and upper
 * (BASIC/AMSDOS) ROMs overlaid when the Gate Array enables them, plus the 6128
 * PAL's RAM banking (§2b of the spec): eight slot→page configurations,
 * dk'tronics bank select, and the Yarek extended-bank scheme for expansions up
 * to 4 MB. The Device independently watches the banking I/O writes (it does not
 * share GA state) and answers every mreq read/write and the GA's video fetch
 * port.
 *
 * Caller-owned, no heap: ROM/RAM live in the caller's storage; expansion RAM is
 * a caller-attached buffer (mem_attach_expansion). */
#ifndef KONCPC_HW_MEMORY_H
#define KONCPC_HW_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MemRegs {
  uint8_t rom_config; /* GA mode register: bit2 = lower-ROM disable, bit3 =
                         upper-ROM disable */
  uint8_t ram_config; /* PAL banking latch: 11 bbb ccc */
  uint8_t
      ram_ext; /* Yarek extended bank bits (inverted A13..A11 of the write) */
  uint8_t
      rom_select; /* upper-ROM number latched by an A13-low I/O write (&DFxx) */
} MemRegs;

size_t mem_state_size(void);
Device mem_init(void* storage);
void mem_peek(const Device* dev, MemRegs* out);

/* Load ROM contents (up to 16K each). Persist across reset (they are the
 * firmware). */
void mem_load_lower_rom(const Device* dev, const uint8_t* data, size_t len);
void mem_load_upper_rom(const Device* dev, const uint8_t* data, size_t len);
/* Direct BASE-64K RAM access for tests / loaders (bypasses the bus and
 * banking). */
void mem_write_ram(const Device* dev, uint16_t addr, uint8_t val);
uint8_t mem_read_ram(const Device* dev, uint16_t addr);

/* Attach caller-owned expansion RAM (a multiple of 64K, up to 4 MB). Enables
 * the 6128 PAL banking; without an attachment the banking latch is inert. The
 * buffer is live external storage: it persists across reset and is not
 * serialized. */
void mem_attach_expansion(const Device* dev, uint8_t* buf, size_t len);

/* Attach a caller-owned 16K expansion ROM image as upper-ROM number `n`
 * (1..255; AMSDOS conventionally 7). Selecting an unattached number falls back
 * to BASIC (the built-in upper ROM). Pass data = NULL to detach. Live wiring,
 * like the expansion RAM: persists across reset and is not serialized. */
void mem_attach_rom(const Device* dev, uint8_t n, const uint8_t* data);

/* Plus (6128+) cartridge: overlay the low/high ROM windows with a parsed CPR
 * image (`bytes` = whole 16K banks, caller-owned, up to 32 banks). Enables
 * cartridge banking — low ROM = RMR2-selected bank (boots bank 0 = OS), high ROM
 * = ROM-select-mapped bank (boots bank 1 = BASIC). Live wiring; bytes < 16K (or
 * a non-Plus machine) leaves it a plain ROM box. */
void mem_load_cartridge(const Device* dev, const uint8_t* image, size_t bytes);

/* Wire the ASIC handle whose asic_unlocked() gates the RMR2 low-ROM remap. Live
 * wiring (like the attach_* above); no-op effect until a cartridge is loaded. */
void mem_attach_asic(const Device* dev, const Device* asic);

/* The CPU-VISIBLE view (debug peeks: what the Z80 would read/write at this
 * address right now) — honours the active ROM overlays and RAM banking, like a
 * logic analyzer replaying an mreq cycle. Writes go to the banked RAM byte and
 * never touch ROM, exactly like a real mreq write. */
uint8_t mem_peek_cpu(const Device* dev, uint16_t addr);
void mem_poke_cpu(const Device* dev, uint16_t addr, uint8_t val);

/* --- Fast-tier batch seam (memory-device.md §batch, plan §4.3) ---
 *
 * The same CPU-visible view as mem_peek_cpu/mem_poke_cpu, but table-driven:
 * four 16K bank pointers per direction, rebuilt lazily after a banking-
 * relevant latch changes instead of re-deriving the decode per access. The
 * tables are BUILT FROM the same helpers mem_tick reads through (banked_ptr /
 * the ROM-overlay predicates), so there is one banking truth.
 *
 * Expansion overlays (/ROMDIS, /RAMDIS) and the Plus ASIC register page are
 * NOT the memory device's to answer — in the Fast tier the machine's
 * Z80BatchIO callbacks layer them above this seam, exactly as those boards
 * sit on the bus above the internal decode. */
uint8_t mem_fast_read(const Device* dev, uint16_t addr);
void mem_fast_write(const Device* dev, uint16_t addr, uint8_t val);

/* Where a CPU write to addr physically lands: the offset into the base-64K
 * RAM window (the one mem_video_ram exposes), or -1 when banking routes it
 * to expansion RAM — which display fetches can never read. The Fast tier's
 * write filter uses this to skip render catch-up for writes that cannot
 * touch fetchable bytes. */
int32_t mem_fast_write_off(const Device* dev, uint16_t addr);

/* Apply one I/O WRITE event to the banking latches — the identical decode
 * mem_tick snoops from the bus (GA fn2/RMR2, PAL ram_config + Yarek bits,
 * A13-low ROM select). The Fast scheduler routes every OUT here; ports the
 * decode ignores are ignored here too. */
void mem_fast_io_write(const Device* dev, uint16_t port, uint8_t val);

/* The 64K base RAM the GA's display fetches read (never banked, never
 * ROM-overlaid — the same coupling the Bus.ram fetch port expresses). The
 * Fast tier's catch-up renderer reads display bytes through this window. */
const uint8_t* mem_video_ram(const Device* dev);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_MEMORY_H */
