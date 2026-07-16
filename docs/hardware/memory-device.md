# Memory map — sub-cycle, pin-level Device simulation reference

Spec for the sub-cycle memory Device (`src/hw/memory`), companion to `z80.md`,
`gate-array-device.md`, `crtc-device.md`. It backs the 64K Z80 address space and
overlays the ROMs the Gate Array enables.

## 1. Role

The Z80 sees a flat 64K space. The memory Device answers every `mreq`:
- **Read**: returns a ROM byte where a ROM is enabled and overlays that region,
  otherwise RAM.
- **Write**: always to RAM — writes pass *through* any ROM overlay to the RAM beneath.

It also serves the **video fetch port** (`Bus.ram`, driven by the Gate Array during
its video slots): `fetch` asserted → drive `ram.data = RAM[ram.addr]`. Video fetches
**always read RAM** — the ROM overlays apply only to CPU reads. (On the real board the
GA's DRAM access physically bypasses the ROM chips, which sit on the CPU bus.)

## 2. ROM overlays and banking

The Gate Array's mode register (I/O to A15=0/A14=1, function `data>>6 == 2`) carries
two ROM-enable bits; the memory Device **watches that write independently** (it does
not share GA state):

| Region | Overlay when enabled |
|---|---|
| `0x0000–0x3FFF` | **lower ROM** (firmware) — enabled while `rom_config bit2 == 0` |
| `0x4000–0xBFFF` | always RAM |
| `0xC000–0xFFFF` | **upper ROM** (BASIC/AMSDOS) — enabled while `rom_config bit3 == 0` |

**Plus (6128+) cartridge low ROM.** With a cartridge attached, the lower-ROM
window reads from a cartridge 16K page instead of `lower_rom[]`. The ASIC's RMR2
register (asic-device.md §2) sets both the page (`cart_lower`, bits 0-2) and the
**16K slot** it occupies (`cart_lower_slot`, membank bits 4-3: 0→`&0000`,
1→`&4000`, 2→`&8000`; membank 3 = register page, low ROM back at slot 0). Only
that slot is overlaid — the other low slots show RAM — mirroring the legacy
`memory_set_read_bank(lower_ROM_bank, …)`. Firmware RAM-under-ROM restarts rely
on this: they park the cartridge at `&8000` (membank 2) so `&0000-&3FFF` stays
RAM.

Function `data>>6 == 3` sets `ram_config` (6128 RAM banking) — **stored, not yet
applied** (a later slice remaps the banks; today RAM is a flat 64K).

Multiple upper ROMs (BASIC=0, AMSDOS=7, …) selected via port `&DF` are a later slice;
today one lower + one upper ROM.

## 2b. RAM banking — the 6128 PAL and big expansions

On a 6128 the banking is **not** done by the Gate Array: a separate **PAL 16L8**
latches any I/O write with **A15 low** and data bits 7:6 = `11` (note the decode —
plain A15, *not* the GA's A15+A14; `OUT &7F00` satisfies both, but the PAL also
answers e.g. `&3F00`). The latched byte is `11 bbb ccc`:

- `ccc` — **configuration 0..7**: which physical 16K page each CPU slot sees.
  With pages 0–3 = base 64K and 4–7 = the four pages of the *selected expansion
  bank* (64K), the PAL's eight maps are:

  | cfg | 0x0000 | 0x4000 | 0x8000 | 0xC000 | note |
  |---|---|---|---|---|---|
  | 0 | 0 | 1 | 2 | 3 | all base |
  | 1 | 0 | 1 | 2 | 7 | |
  | 2 | 4 | 5 | 6 | 7 | whole 64K = expansion |
  | 3 | 0 | 3 | 2 | 7 | CP/M+ oddity: slot 1 aliases base page 3 |
  | 4–7 | 0 | 4..7 | 2 | 3 | expansion page (cfg−4) in slot 1 |

- `bbb` — **expansion bank select** (dk'tronics scheme): which 64K bank of the
  expansion provides pages 4–7. Up to 8 banks = 512K.

- **Yarek 4MB scheme** (expansions > 512K): the bank number grows to 6 bits — the
  high 3 bits come from the **inverted address bits A13..A11 of the banking write**
  (`ext = (~addr>>11) & 7`). The standard port `&7Fxx` has A13..A11 = 111 → ext 0,
  so plain software is untouched; writing via `&77xx`, `&6Fxx`, … reaches banks
  8–63. `bank = (ext << 3) | bbb`, 64 × 64K = 4 MB.

Rules mirrored from the reference implementation:
- **No expansion attached → banking inert** (config forced 0).
- **Bank out of range** for the attached size → bank 0 (config still applies).
- Expansions ≤ 512K ignore `ext` (3-bit bank); larger ones use all 6 bits.
- ROM overlays apply **after** banking (a ROM read wins over whatever RAM page is
  mapped); writes always land in the *banked* RAM page.
- **Video fetches are never banked** — the GA's DMA reads the base 64K only, so
  expansion RAM is not displayable (real-hardware fact, and why the fetch port
  reads `ram[]` directly).

Device model: the expansion is a **caller-owned buffer** (`mem_attach_expansion`,
multiples of 64K) — same no-heap pattern as the video framebuffer and the PSG key
matrix. It is live external storage: not serialized by save/load, and reset keeps
its contents (like base RAM).

## 2c. Multi-ROM select (&DF)

The upper 16K window (0xC000, when the upper-ROM enable is on) is one of up to 256
selectable ROMs. Board logic (not the GA) latches any I/O **write with A13 low** —
the conventional port is `&DFxx` (A13=0), but any A13-low write works — and the data
byte is the **ROM number** (0..255):

- ROM 0 = onboard **BASIC** (the built-in upper ROM).
- ROM 7 = conventionally **AMSDOS** (lives on the disc interface).
- Other numbers = expansion ROM boards.
- Selecting an **unpopulated** number: no board claims the select, so the internal
  BASIC ROM answers — the fallback is BASIC, not open bus.

The select only affects which ROM appears when the GA's upper-ROM enable
(`rom_config` bit 3 clear) maps ROM over 0xC000; writes still land in the banked RAM
beneath, and the lower ROM / video fetch are untouched. Reset returns the select
to 0. Decode disjointness: `&DFxx` has A15=1 (PAL ignores it) and doesn't match the
GA select — the three latches never collide.

Device model: expansion ROM images are **caller-owned 16K buffers** registered in a
256-entry pointer table (`mem_attach_rom(n, data)`) — live wiring like the expansion
RAM, preserved across reset and save/load. The firmware's boot-time ROM enumeration
(KL ROM WALK) then finds and initializes them exactly as on real hardware.

## 3. Reset / persistence

Reset restores `rom_config = ram_config = 0` (both ROMs enabled) but **keeps RAM and
ROM contents** — the ROM is the firmware and must survive reset. `mem_load_lower_rom` /
`mem_load_upper_rom` load up to 16K each; `mem_read_ram` / `mem_write_ram` give tests
and loaders direct RAM access.

## 4. Verification

- ROM overlays 0x0000/0xC000 by default; disabling a ROM (mode-reg bit 2/3) reveals the
  RAM beneath; a write to a ROM region lands in that RAM (visible once the ROM is off).
- **Boot**: a program placed in the lower ROM runs from reset (PC=0) — the Z80 fetches
  and executes firmware (`Memory.Z80BootsFromLowerRom`).

## 5. Device model

`tick(in,out)` answers mreq and tracks the banking writes each cycle. Caller-owned,
no-heap: RAM + both ROMs live in the caller's `mem_state_size()` storage.

## 4b. Expansion overlays: /ROMDIS, /RAMDIS and the one-tick write latch

Reads: when the committed bus carries `romdis`, the Device does not drive a
read that its ROM decode would have served (lower ROM under 0x4000, upper
ROM above 0xC000); under `ramdis` it does not drive a RAM-served read. The
asserting expansion drives the byte instead. The first cycle of an access
may see both drivers (the expansion's line settles one tick behind the
strobes) — physical bus contention; the CPU samples at the end of the
cycle, after the settle, so the RESULT is order-independent.

Writes: RAM commits a CPU write ONE MASTER CYCLE after latching it (one
latch per access, on the strobes' edge), checking `ramdis` at commit time.
This lets an expansion that pages RAM over an address range (the Multiface
II's freeze RAM) veto the internal write it saw one tick earlier — the
golden master's exclusive-write behaviour, hardware-plausible as the RAM
array committing on the trailing edge of /WR after /RAMDIS has settled.
The one-tick delay is unobservable to the CPU (its next access samples
many cycles later) and does not touch the GA's video fetch path.

## Batch contract (RunTier::Fast)

- **Access seam**: `fast read/write` resolve through bank-pointer tables
  (16K granularity) rebuilt only on banking EVENTS: GA fn-2 (ROM enables /
  RMR2 when unlocked), fn-3 RAM config, the &7Fxx PAL decode, upper-ROM
  select (&DFxx), cartridge bank flips. The decode logic is SHARED with
  mem_tick (single-definition rule) — the tables are just its cache.
- **ROM/RAM overlays**: /ROMDIS//RAMDIS contributors are events too (ASIC
  register page map/unmap, expansion overlays); table rebuild on change.
- **The one-tick write latch (§4b) is a per-cycle artifact.** Batch
  semantics: a write applies immediately under the overlay state current at
  the event. Equivalence argument: overlay changes are I/O/page events and
  writes are memory events — at instruction granularity their order is the
  program's order, which is exactly what faithful's one-tick-delayed commit
  observes. ORACLE-WATCH: this is the mem@frame-164 bug area from the wake
  cycle — the Plus lockstep with per-device state naming is the guard.
- **Video fetches** read RAM directly in the renderer's catch-up (overlays
  never apply to the video port — §fetch).
- Bestiary audit: class (a) — the write latch, handled above; class (d) —
  the data-bus float is a bus notion with no batch counterpart; reads return
  the decoded byte or 0xFF for unmapped I/O by construction.

Implementation: `mem_fast_read` / `mem_fast_write` / `mem_fast_io_write`
(`memory.h` §batch) — four 16K bank pointers per direction, rebuilt lazily
(`fast_dirty`) from the SAME resolvers the per-cycle path reads through
(`banked_ptr`, the ROM-overlay predicates, the shared `mem_io_write_decode`).
The tables are a derived host-pointer cache: zeroed + dirtied in the save
blob. Expansion /ROMDIS//RAMDIS overlays and the Plus ASIC register page are
layered ABOVE this seam by the Fast machine's Z80BatchIO callbacks.
ORACLES: `Memory.FastSeam*` / `Memory.FastIoWrite*` (unit equivalence, twin
write-path store compare), `Z80BatchMem.BankingProgramLockstep` (the minimal
Fast machine vs the per-cycle board, full-state).
