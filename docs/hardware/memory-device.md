# Memory map — clean-room Device implementation reference

Spec for the clean-room memory Device (`src/hw/memory`), companion to `z80.md`,
`gate-array-device.md`, `crtc-device.md`. It backs the 64K Z80 address space and
overlays the ROMs the Gate Array enables.

## 1. Role

The Z80 sees a flat 64K space. The memory Device answers every `mreq`:
- **Read**: returns a ROM byte where a ROM is enabled and overlays that region,
  otherwise RAM.
- **Write**: always to RAM — writes pass *through* any ROM overlay to the RAM beneath.

## 2. ROM overlays and banking

The Gate Array's mode register (I/O to A15=0/A14=1, function `data>>6 == 2`) carries
two ROM-enable bits; the memory Device **watches that write independently** (it does
not share GA state):

| Region | Overlay when enabled |
|---|---|
| `0x0000–0x3FFF` | **lower ROM** (firmware) — enabled while `rom_config bit2 == 0` |
| `0x4000–0xBFFF` | always RAM |
| `0xC000–0xFFFF` | **upper ROM** (BASIC/AMSDOS) — enabled while `rom_config bit3 == 0` |

Function `data>>6 == 3` sets `ram_config` (6128 RAM banking) — **stored, not yet
applied** (a later slice remaps the banks; today RAM is a flat 64K).

Multiple upper ROMs (BASIC=0, AMSDOS=7, …) selected via port `&DF` are a later slice;
today one lower + one upper ROM.

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
