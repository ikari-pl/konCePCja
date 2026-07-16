# The printer port latch — Device spec

The CPC's Centronics printer port is one write-only 8-bit latch decoded by
**A12 = 0** on an I/O write (conventionally &EF7E; the decode is partial, so
any address with bit 12 low selects it alongside whatever else the address
hits — the latch just grabs the data byte). The base machine inverts bit 7
before it reaches the connector: bit 7 of the latched value is **/STROBE**
and bits 6..0 are the data lines (the CPC only wires 7 data bits to the
plug). The golden master (`kon_cpc_ja.cpp`): `CPC.printer_port = val ^ 0x80`,
reset value `0xFF`.

## 1. Decode and latch

- I/O write (`iorq && wr && !m1`) with `(addr & 0x1000) == 0` → latch
  `data ^ 0x80`. One latch update per ACCESS (edge semantics, same rule as
  the FDC data register).
- Reads never answer: the port is write-only, the bus floats.
- The Device drives nothing on the Bus — the connector's consumers are all
  host-side (below).

## 2. Byte capture (the connector)

A real printer clocks a byte on the **falling edge of /STROBE** (latched
bit 7 going 0 after being 1 — post-inversion: latch bit 7 CLEAR = strobe
asserted). The Device logs each such byte into a small drop-oldest event
ring (`PrinterEvent { cycle, byte }`, bytes are the 7 data bits); the host
drains it with `printer_drain_events` for:

- the legacy "capture printer output to file" feature (parity: the golden
  master grabs bytes when the strobe bit is clear), and
- the plotter subsystem, which is a host-side consumer of the same stream.

## 3. Digiblaster (an interpretation, not a Device)

The Digiblaster is a resistor-ladder DAC hanging off the printer connector:
the LATCHED BYTE is the sample — its bit 7 rides the /STROBE line, giving
the full 8 bits. There is no additional state and no bus behaviour beyond
the latch above; the mixing into the sound output happens in the ANALOG
domain, outside every chip. Accordingly the emulation mixes it ABOVE the hw
layer (the doctrine for jack/audio features): `subcycle::Machine` reads the
latch via `printer_peek` at its audio sample rate and, when the user enables
Digiblaster, adds `(latch − 128) × 31 / 128` (one PSG channel's swing) to
the LEFT channel — the golden master mixes `Level_PP[]` into LevL.

## 4. Host API

`printer_state_size/init/peek` (`PrinterRegs { latch }`),
`printer_drain_events(dev, out, max)`. Serialization: the latch (one byte);
the event ring is live telemetry, never serialized. Reset: latch back to
`0xFF`, ring cleared.

## Batch contract (RunTier::Fast)

- **Pure event device**: runs only on I/O writes with A12 low; the /STROBE
  falling edge (bit 7 latched 1 → 0) pushes a PrinterEvent stamped with the
  timeline's master-cycle clock — `now` ceases to exist as device state in
  this tier (the shipped printer_advance contract already treats it as
  scheduler-owned).
- Bestiary audit: class (b) — the timestamp base moves to the timeline,
  eliminating the counter entirely; one latch per access is the natural
  event semantics (edge detector artifact-free).
