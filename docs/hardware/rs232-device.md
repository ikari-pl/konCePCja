# The RS232 interface card (Amstrad SI / Pace) — Device spec

The Amstrad Serial Interface: a Z80 DART (Z8470) and an Intel 8253 timer on
the expansion bus, with an optional sideways firmware ROM. It is the CPC end
of the serial wire; whatever sits at the other end of that wire (the HP 7470A
plotter Device, a file tap, a host TTY) is a **separate Device** exchanging
bits on `SerialBus` — the tape-deck precedent (tape-device.md): the card is
the connector-side electronics, the peer is beyond-the-connector hardware.

Replaces the legacy `serial_interface.cpp` port hooks AND the BDOS serial
hooks (`z80_set_bdos_serial_out/in_hook`): under a bus-resident card, CP/M
Plus drives AUX through its own BIOS SIO driver against the DART registers,
exactly as on real hardware — zero firmware-vector intercepts. This card is
a Wave-1 S3 deletion blocker for z80.cpp (replacement-ledger feature parity).

## 1. The wires

New bus section (buses.h, after TapeBus):

```c
/* The RS232 serial line: two wires between the interface card and whatever
 * is plugged into its DB25. Logic levels post line-driver (idle HIGH = mark);
 * bit-serial, sampled/driven at master-cycle resolution. */
typedef struct SerialBus {
  bool txd; /* CPC → peer data — the card drives */
  bool rxd; /* peer → CPC data — the peer Device drives */
} SerialBus;
```

Both lines idle high (mark). Framing is async 8N1 in V1 (start bit low, 8
data bits LSB-first, 1 stop bit high). DART WR4/WR5 parity/stop-bit fields
are stored and reported back but do not change V1 framing — the SI ROM, the
CP/M SIO driver, and the GSX plotter chain all run 8N1.

The card also drives `cpu.data` during IORQ reads of its ports and never
touches any other line. RTS/DTR/CTS/DCD stay internal card state in V1
(status-readable; not wires) — flow control on the plotter chain is in-band
XON/XOFF, handled entirely above the wire by the driver and the peer.

## 2. Decode

Observed decode (legacy oracle, verified by the SI ROM and the CP/M Plus SIO
driver):

| Port  | Chip select | Register |
|-------|-------------|----------|
| $FADC | DART        | Ch A data |
| $FADD | DART        | Ch B data (unused on the Amstrad SI) |
| $FADE | DART        | Ch A control (WR0 pointer / RRn) |
| $FADF | DART        | Ch B control (unused) |
| $FBDC–$FBDE | 8253  | counters 0–2 (counter 0 = baud) |
| $FBDF | 8253        | mode/control word |

Decode is `(addr & 0xFF00) == 0xFA00 / 0xFB00` **and** `(addr & 0x00FF)` in
$DC–$DF — the full low-byte check keeps the FDC ($FB7E) and other $FBxx
peripherals uncontested, matching the legacy handler. Unplugged (config
off): nothing decodes.

## 3. The DART register model

Channel A only in V1 (channel B decodes, stores writes, reads back zeros —
the Amstrad SI leaves it unwired).

- **WR0** — register pointer (bits 0–2) + command (bits 3–5: error reset,
  reset ext/status interrupts, channel reset). A control write lands in the
  register the pointer selects, then the pointer snaps back to 0.
- **WR1** — interrupt enables (stored; see §6). **WR2** (ch B) — vector.
  **WR3** — RX parameters (bit 0 = RX enable). **WR4** — clock mode ×1/×16/
  ×32/×64 (bits 6–7), stop bits, parity. **WR5** — TX parameters (bit 3 =
  TX enable, bit 1 = RTS).
- **RR0** — bit 0 RX char available, bit 2 TX empty, bit 3 TX buffer empty,
  bit 5 CTS (always set in V1). **RR1** — bit 0 all-sent + error bits.
  **RR2** (ch B) — interrupt vector readback.
- **RX FIFO**: 3 deep (the real DART's receive stack). Overrun past 3 sets
  RR1 overrun, newest byte lost.

Register-pointer quirk carried from the legacy oracle: real code addresses
Ch A control at $FADE; data at $FADC. There is no offset remap in the
Device — the decode table above is the hardware truth (the legacy 2→1 remap
was an artifact of its simplified register array, not of the hardware).

## 4. Time base — the 8253 makes the baud rate

The SI clocks the 8253 at **2 MHz** (expansion clock /2); counter 0 in mode 3
(square wave) feeds the DART TX/RX clock. With the DART in ×16 mode:

```
bit_time_master = divisor × 8 × 16        (16 MHz master cycles)
byte_time = 10 × bit_time_master          (start + 8 data + stop)
baud      = 2e6 / divisor / 16
```

divisor 13 → 9615 baud → 1664 master cycles per bit — the same rounding real
hardware exhibits. Divisor 0 is treated as 1. Counter modes other than 3 are
stored and read back; only counter 0's divisor value affects the line.

## 5. Transmission is not instantaneous

The legacy backend delivered TX bytes instantly; the Device serializes them
on `serial.txd` at the programmed baud. A data-port write loads the TX
buffer (RR0 TX-buffer-empty drops); the shift register picks it up (buffer
empty rises — double buffering, so back-to-back writes overlap correctly);
each bit occupies `bit_time_master` cycles on the wire; RR1 all-sent rises
with the stop bit. RX mirrors this: the card samples `serial.rxd` mid-bit
(start-edge + bit_time/2, then every bit_time), assembles frames, pushes to
the FIFO. Polled drivers (SI ROM, CP/M SIO, DDHP7470.PRL) therefore see
authentic pacing with zero special cases.

## 6. Interrupts

The DART /INT pin is wired to the expansion bus interrupt line. V1 stores
WR1 enables and computes `interrupt_pending` (status-visible) but does NOT
drive `cpu.irq` — every shipped driver on this machine polls, and the CPC's
IM 1 + Gate Array interrupt scheme makes expansion IM 2 delivery its own
project. Flagged V2; the wire assignment (`out->cpu.irq |= …` wired-OR) is
reserved for it.

## 7. Host API

```c
size_t rs232_state_size(void);
Device rs232_init(void* storage);
void   rs232_peek(const Device* dev, Rs232Regs* out);  /* WRn/RRn, divisor,
                                                          fifo depth, line
                                                          bit states */
void   rs232_set_plugged(const Device* dev, int on);
```

The SI firmware ROM stays a plain expansion-ROM image handled by the memory
Device's upper-ROM mapping (slot 2 default) — ROM loading is machine
configuration, not card behavior.

## 8. Acid tests

1. **DART register semantics** — pointer writes, channel reset, FIFO depth 3
   + overrun flag, TX double buffering: unit oracles against §3.
2. **Wire framing** — TX a byte at divisor 13, assert txd edge positions at
   exact master-cycle offsets (1664/bit); loop txd→rxd, assert the received
   byte and RR0 timing.
3. **Legacy-oracle byte log** — the recorded GSX plotter session byte stream
   (see plotter-device.md §7) replayed through the card produces the same
   TX byte sequence the legacy backend saw.
4. **Tier lockstep** — fast vs wake with serial traffic in flight:
   byte-exact registers, wire, and framebuffer (the standing bar).

## Batch contract (RunTier::Fast)

Between I/O accesses to the card, the wire evolves closed-form: the card's
next event horizon is the next bit edge of an in-flight TX/RX frame (or
infinity when both shift registers are idle and the FIFO untouched). The
batch scheduler may skip to `min(next_bit_edge, next_io_access)`;
`*_advance`-style catch-up computes elapsed whole bits arithmetically.
Idle card (no frame in flight, RX line at mark) contributes no events —
elision-eligible exactly like the parked tape deck.
