# AmDrum (Cheetah) — Device spec

An 8-bit DAC drum machine on the expansion port. It decodes the
"uncontested" I/O space — writes with **all upper address bits high**
(`(addr & 0xFF00) == 0xFF00`), where no internal chip listens — and latches
the data byte as its current DAC level. The golden master (`amdrum.cpp`):
`dac_value = val` on any &FFxx OUT, reset to 128 (mid-scale silence).

## 1. Decode and latch

- I/O write (`iorq && wr && !m1`) with `(addr & 0xFF00) == 0xFF00` → latch
  `data`. Edge semantics (one update per access).
- Reads never answer; the Device drives nothing on the Bus.
- **Plugged gate**: an expansion peripheral either sits on the port or does
  not. `amdrum_set_plugged(dev, on)` models insertion; unplugged, the tick
  ignores the bus entirely (no decode — the space really is empty). The
  gate is part of the Device state and serializes with it.

## 2. Sound

The DAC output mixes into the sound path in the ANALOG domain, outside the
chips, so the emulation mixes ABOVE the hw layer: `subcycle::Machine` reads
the latch via `amdrum_peek` at its audio sample rate and, while plugged,
adds `(dac − 128) × 31 / 128` (one PSG channel's swing) to the LEFT channel
— the golden master mixes `Level_AmDrum[]` into LevL.

## 3. Host API

`amdrum_state_size/init/peek` (`AmdrumRegs { dac, plugged }`),
`amdrum_set_plugged`. Reset: DAC to 128; the plugged state persists (a
reset does not eject the cartridge from the expansion port).
