# Core refactor roadmap

Summary of the MemoryBus/IoBus/CpcMachine refactor and follow-up options for the "for now" memory layers.

## Completed (Phases 1–5)

| Phase | Description |
|-------|-------------|
| 1 | **CpcMachine** — Non-owning aggregate of core globals (Z80, drives, etc.). Merged in PR #48. |
| 2 | **MemoryBus + IoBus** — Thin non-owning wrappers over `membank_read`/`membank_write` and `io_dispatch_*`. Merged in PR #49. |
| 3 | **Wire hot paths** — Z80 raw memory uses `g_memory_bus.read_raw`/`write_raw`; IN/OUT use `g_io_bus.in`/`out`. Merged in PR #49. |
| 4 | **Route remaining memory** — `z80_read_mem_via_write_bank` and ASIC DMA/register writes go through `g_memory_bus`. No direct `membank_*` access for data. |
| 5 | **Centralize bank switching** — `memory_set_read_bank()` / `memory_set_write_bank()`; only `kon_cpc_ja.cpp` touches the bank arrays. IPC and imgui use the setters. |

## Current "for now" state: memory layers in z80.cpp

`MemoryBus` is intentionally **raw banked access only**. It does not know about:

- **SmartWatch** — Intercepts upper ROM reads (0xC000+) when enabled; implemented in `read_mem_no_watchpoint()` in z80.cpp after `g_memory_bus.read_raw()`.
- **Watchpoints** — Read/write breakpoints with conditions; implemented in `read_mem()` / `write_mem()` wrapping the no-watchpoint path.
- **ASIC register page** — Writes to 0x4000–0x7FFF when register page is on; handled in `write_mem()` before `write_mem_no_watchpoint()`.
- **Multiface II RAM** — Writes to 0x2000–0x3FFF when MF2 is active go to MF2 SRAM; handled in `write_mem()`.

So today:

- **Single layering point**: z80.cpp `read_mem` / `read_mem_no_watchpoint` and `write_mem` / `write_mem_no_watchpoint`.
- **Public API**: `z80_read_mem` / `z80_write_mem` call the no-watchpoint path (so they see SmartWatch on read; they do **not** go through watchpoints, MF2 RAM redirect, or ASIC register page).
- **IPC / DevTools**: Use `z80_read_mem` / `z80_write_mem` (and `z80_read_mem_via_write_bank` for raw write-bank view). So they get SmartWatch on read but raw bus for the write path (no MF2/ASIC redirect there).

## Planned follow-up: Phase 6+ (memory layers)

We aim for **Option B** then **Option C** as a **sequence**: B centralizes the layered accessor logic in one place; C turns that into a first-class type (LayeredMemory / MemoryController) on CpcMachine. They are not alternatives—C builds on B.

### Phase 6 (Option B): Shared "CPU view" accessor ✅

- `z80_cpu_write_mem()` now applies MF2 RAM redirect + ASIC register page + IPC events (same as `write_mem()` but without watchpoints).
- `z80_cpu_read_mem()` applies SmartWatch on upper ROM reads (unchanged, was already correct).
- `LayeredMemory` facade updated to document the correct semantics.
- IPC `mem write` commands and DevTools memory poke can use `z80_cpu_write_mem()` to get proper device intercept behavior.

### Phase 7 (Option C): LayeredMemory / MemoryController type ✅

- `LayeredMemory` exists at `src/layered_memory.h` — thin facade over MemoryBus + device intercepts.
- Lives on `CpcMachine` as `g_machine.memory` (`src/cpc_machine.h:24`).
- `read_cpu()`/`write_cpu()` provide full device layering (SmartWatch/MF2/ASIC) without watchpoints.
- Callers can migrate from `z80_cpu_read_mem`/`z80_cpu_write_mem` to `g_machine.memory.read_cpu()`/`write_cpu()` as cleanup.
