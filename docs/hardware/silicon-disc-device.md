# DK'Tronics Silicon Disc — spec (why it is NOT a bus Device)

The Silicon Disc is 256K of **battery-backed RAM** that answers the CPC's
standard expansion-RAM banking — nothing more. It has no ports, no command
protocol, no address decode of its own beyond the ordinary `&7Fxx` PAL bank
select every RAM expansion shares. Software uses it as a RAMdisc precisely
because it is plain banked RAM that survives power-off. The behavioural
oracle is `src/silicon_disc.cpp` (ours, 2026 — engine-parity, not deletion).

## 1. The design decision: a persistence policy, not a Device

Every other peripheral this session became a `src/hw` Device because it had
genuine bus behaviour to model. The Silicon Disc has none. Modelling it as a
separate Device would force one of two fictions:

- duplicate the memory Device's banking decode in a second place, or
- invent a per-bank arbitration handshake between the memory Device and a
  "silicon" Device for banks the memory Device already serves.

Both model the hardware *less* faithfully than the truth: on a real machine
the Silicon Disc **is** the expansion RAM for its bank range, plus a battery.
So the honest model is exactly that — the memory Device serves banks 4-7
from its expansion buffer (it already does, `banked_ptr`), and "Silicon
Disc" is the **host-side policy** that (a) that region is battery-backed
(loaded from / saved to a file, never cleared) and (b) the expansion is
sized large enough to expose those banks. No new Device, no new bus lines.
This mirrors how battery-backed state is handled everywhere in the hw layer:
the Device holds the bytes, the host owns persistence (FDC §10, Symbiface
CMOS, the RTCs).

## 2. What the banks are

`banked_ptr` (memory-device.md) resolves an expansion page to
`expansion[bank*64K + (page-4)*16K + offs]`, where `bank = (ram_config>>3)
& 7`. The 6128's second 64K is bank 0; a DK'Tronics-style expansion adds
banks 1-7. The Silicon Disc occupies **banks 4-7** — expansion offsets
`256K .. 512K`. Example: `ram_config = 0xE4` (`11 100 100` — bank 4 in bits 5-3, config 4 in bits 2-0) maps that
bank's first 16K to `&4000-&7FFF`, so a CPU write to `&4000` lands at
expansion offset `256K`.

Therefore the machine must carry a **512K expansion** to expose banks 4-7
(the default sub-cycle machine allocates only 64K = a bare 128K 6128). With
the Silicon Disc enabled the machine sizes its expansion to 512K; banks 1-3
(offsets `64K..256K`) are ordinary zeroed expansion RAM (a minor
simplification — on hardware they would be unpopulated unless a DK'Tronics
RAM box also filled them).

## 3. Battery backing

The expansion buffer is caller-owned live wiring and is **not cleared by a
CPC reset** (the memory Device's reset touches only the banking latches), so
"survives reset" is automatic — the battery is modelled by the host simply
not zeroing the region. Power-off persistence is the host writing the region
to a file (the oracle's `KSDX`-headed format) and reloading it at start.

## 4. Host API (Machine + bridge)

- `Machine::enable_silicon_disc(bool)` — grows the expansion to 512K and
  re-attaches it (idempotent); the region is `[256K, 512K)` of the
  expansion (physical RAM addresses `0x10000 + 256K ..`).
- `Machine::silicon_disc_load(const uint8_t* src, size_t)` /
  `silicon_disc_save(uint8_t* dst, size_t)` — copy the 256K region in/out
  for host persistence.
- Bridge: on `g_silicon_disc.enabled`, enable it at start, load the config's
  Silicon Disc file into the region, and save it back on stop. Reset needs no
  special handling (the region is never cleared).

No ports, no ticks, no serialization beyond the region the host already
persists — the whole peripheral is "big battery-backed RAM," and the spec's
job is to record *why that is the faithful model* rather than a shortcut.
