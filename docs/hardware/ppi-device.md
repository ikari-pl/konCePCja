# PPI 8255 — sub-cycle, pin-level Device simulation reference

Language-neutral spec for the sub-cycle **PPI 8255** Device (`src/hw/ppi`), the
CPC's programmable peripheral interface. For real-chip detail see the hardware
reference material; **this** doc is the spec the Device code follows. Companion to
`docs/hardware/z80.md`, `gate-array-device.md`, `crtc-device.md`. See also
`docs/hw-spec.md` (the system bus / Device model).

The PPI is the CPC's I/O hub: it carries the **keyboard**, the **AY-3-8912 sound
chip control/data path**, the **cassette** motor + read/write, and the machine's
**status inputs** (VSYNC, refresh rate, manufacturer id, printer ready). It is a
Device like any other: `tick(in, out)` once per 16 MHz master cycle, no heap,
caller-owned state.

---

## 1. Role and bus position

- **I/O select**: the PPI answers when address line **A11 = 0** (`(addr & 0x0800) == 0`).
  The two low address bits of the high byte, **A9..A8**, pick the port:

  | A9 A8 | Port | CPC use |
  |---|---|---|
  | 0 0 | A (0xF4xx) | AY-3-8912 data bus (bidirectional) |
  | 0 1 | B (0xF5xx) | status inputs (read): VSYNC, printer, tape, jumpers |
  | 1 0 | C (0xF6xx) | keyboard row select + AY control + cassette |
  | 1 1 | Control (0xF7xx) | 8255 mode / bit-set-reset register |

- **Reads** `vid.vsync` (from the CRTC) for Port B bit 0.
- **Drives the internal AY bus** (`ay.da`, `ay.bdir`, `ay.bc1`, `ay.kbd_row`) — see
  §5 and `psg-device.md`. The AY is reached *only* through the PPI; the Z80 never
  addresses it directly.
- Drives **`cpu.data`** on an I/O read it owns (like every I/O device).

---

## 2. The three ports and the control register

The 8255 has three 8-bit ports (A, B, C) and a control register. Each port (and each
half of C) can be independently input or output, set by the control register.

### Control register (port index 3), MSB decides the command

- **bit 7 = 1 — mode-set**: reconfigure directions. Layout:
  - bit 6..5: Group A mode (CPC always 00 = mode 0)
  - bit 4: **Port A** direction — 1 = input, 0 = output
  - bit 3: **Port C upper** (bits 4..7) — 1 = input
  - bit 2: Group B mode (00)
  - bit 1: **Port B** direction — 1 = input
  - bit 0: **Port C lower** (bits 0..3) — 1 = input
  - **Side effect** (real 8255): a mode-set command **clears all output latches**
    (portA = portB = portC = 0). Modeled.
  - CPC firmware programs **0x82** = Port A output, Port B input, Port C output.

- **bit 7 = 0 — bit set/reset (BSR)**: set or clear one bit of Port C.
  - bits 3..1 select the bit (0..7); bit 0 = new value (1 = set, 0 = clear).
  - Applies to the Port C *output latch*; the upper/lower output side effects
    (keyboard row, AY control, tape motor) re-fire exactly as for a Port C write.

### Port A — AY data bus

- **Output** (CPC default): the latched value is the data presented to the AY on the
  next AY bus operation (`ay.da`). Writing Port A alone does not pulse BDIR/BC1; the
  AY only acts when Port C carries a control code (§5).
- **Input**: the value last driven by the AY on `ay.da` (used to read AY registers
  and the keyboard — see §5).

### Port B — status inputs (read-only on the CPC)

Bit assignment when read (Port B in input mode; on the 6128+ always input):

| bit | meaning | simulated value |
|---|---|---|
| 0 | CRTC **VSYNC** | `in->vid.vsync` |
| 1..3 | manufacturer id (Amstrad = 7) | from `jumpers` (default 7) |
| 4 | refresh rate (1 = 50 Hz) | from `jumpers` (default 1) |
| 5 | expansion **/EXP** — 1 = no device; a device signals presence by driving it to 0 (cpcwiki/cpctech 8255) | 1 |
| 6 | printer **ready** (0 = ready) | `printer_ready ? 0 : 0x40` |
| 7 | cassette **read** data | `tape.rdata` (bus wire — the deck drives it) |

Default `jumpers = 0x1E` packs id 7 (bits 1..3) + 50 Hz (bit 4). VSYNC is OR'd in
live each read. If Port B is in output mode (non-plus, rare) the latch is returned.

### Port C — keyboard row + AY control + cassette

Output latch bit layout:

| bits | meaning |
|---|---|
| 0..3 | **keyboard row select** (0..15; rows 0..9 exist) → `ay.kbd_row` |
| 4 | cassette **motor** on → `tape.motor` |
| 5 | cassette **write** data → `tape.wdata` |
| 6 | AY **BC1** → `ay.bc1` |
| 7 | AY **BDIR** → `ay.bdir` |

Writing Port C (or a BSR bit) updates the latch, then:
- **lower half output** → publish `kbd_row = latch & 0x0F`.
- **upper half output** → publish `bdir = bit7`, `bc1 = bit6`; update tape motor;
  and, because BDIR/BC1 changed, the AY performs its bus operation (§5).

A **Port C read** returns the latch, but for any half in *input* mode the 8255
returns defined substitutes (upper: AY-control bits + tape sense; lower: 1s). The CPC
firmware keeps Port C output, so reads normally return the latch.

---

## 3. State

```
portA, portB, portC   : uint8   output latches
control               : uint8   last mode-set command (direction bits)
jumpers               : uint8   manufacturer id + refresh rate (default 0x1E)
printer_ready         : bool     (default: not ready → bit stays 0x40)
tape_level            : uint8    0x00 / 0x80 — cassette read line
tape_motor            : bool
```

Directions are decoded from `control`: Port A input = `control & 0x10`; Port B input
= `control & 0x02`; Port C lower input = `control & 0x01`; Port C upper input =
`control & 0x08`.

---

## 4. Tick / reset / save

- **tick**: on an owned I/O cycle (A11 = 0, `iorq` asserted), decode the port from
  A9..A8 and service a read (drive `cpu.data`) or a write (update latch + side
  effects). Every cycle, publish the current AY-bus outputs (`da` = portA when Port A
  is output; `kbd_row`, `bdir`, `bc1` from portC) so the PSG sees a stable bus.
- **reset**: control = **0x9B** — the real 8255 RESET latch: mode 0 with Port A, B,
  and both halves of C all set to **input**. (Encoding a `0` here would instead mean
  all-outputs, and the firmware would read Port B — VSYNC — as a dead latch.) Latches
  = 0, tape motor off. `jumpers` persists (it is a hardware strap, not runtime state).
- **save/load**: version byte + the latches/control/jumpers/tape state. No pointers.

---

## 5. The internal AY bus (PPI ⇆ AY-3-8912)

The AY is wired to the PPI, not the Z80. Three signals, modeled as a private bus on
the `Bus` value (`ay.*`), active-high:

| line | driver | meaning |
|---|---|---|
| `ay.da` (8) | PPI on write / AY on read | data DA0..7 |
| `ay.bdir` | PPI (Port C bit 7) | bus direction |
| `ay.bc1` | PPI (Port C bit 6) | bus control 1 |
| `ay.kbd_row` (4) | PPI (Port C bits 0..3) | selected keyboard row |

BC2 is tied high on the CPC, so `(BDIR, BC1)` alone selects the AY function — matching
the legacy `PSG.control & 0xC0` decode:

| BDIR BC1 | code | AY action |
|---|---|---|
| 0 0 | 0x00 | inactive |
| 0 1 | 0x40 | **read** selected register → drives `ay.da` |
| 1 0 | 0x80 | **write** `ay.da` into selected register |
| 1 1 | 0xC0 | **latch** `ay.da` as the register address |

The PPI is the master of this bus: it publishes `bdir`/`bc1`/`da`/`kbd_row` and, on a
read of Port A in input mode, samples `in->ay.da` (the value the AY drove) and presents
it on `cpu.data`. One master-cycle bus latency is absorbed by the multi-cycle Z80 I/O
machine (control is set by earlier OUTs; the data is stable by the IN). The AY register
model, keyboard read-back, and sound synthesis live in `psg-device.md`.

---

## 6. Keyboard scan sequence (why the pieces connect)

Firmware reads a key row like this — every step is a Z80 `OUT`/`IN` the PPI decodes.
The **critical subtlety**: Port A must be an **output** to hand the AY a register number,
then an **input** to read the AY data bus. The firmware toggles the control register
between **0x82** (Port A out) and **0x92** (Port A in). Because a mode-set command
clears the Port C latch (§2), the row/BC1 are re-established *after* the direction
switch:

1. `OUT` control = **0x82** (Port A output).
2. `OUT` Port A = 14; `OUT` Port C = 0xC0 (BDIR+BC1) → AY latches register 14 (Port A).
3. `OUT` Port C = 0x00 (inactive — Port A may now change without re-latching).
4. `OUT` control = **0x92** (Port A input; this clears Port C).
5. `OUT` Port C = 0x40 | row (BC1 read + row) → AY drives register 14 = the selected
   row's keyboard columns onto `ay.da`.
6. `IN` Port A → PPI returns `ay.da` = the row's key bits (**0 = pressed**).

The row decode lives on the keyboard connector (PPI Port C → rows); the AY only senses
columns. In the Device model the PSG owns the sampled matrix (its Port A input pins),
indexed by `ay.kbd_row`. See `psg-device.md` §keyboard. Writing an AY register is the
mirror image: keep Port A **output**, latch the address (0xC0), set Port A to the value,
then pulse BDIR-only (0x80).

## Batch contract (RunTier::Fast)

- **Pure event device**: runs only on its I/O accesses (and BUSAK handovers
  on a Plus). Port writes apply immediately — the per-cycle drive-then-latch
  publish (outputs driven from pre-write state for one tick, bestiary class
  a) is a bus-pipeline artifact with no observable counterpart at
  instruction granularity: the next possible consumer is a later
  instruction's event, which sees the post-write state either way.
- **Port B reads sample LIVE inputs**: catch up the CRTC (vsync) and the
  tape deck (rdata) to the read's timestamp first, then compose (§3).
- **AY handoff**: port C / port A writes forward to the PSG as AY-bus events
  (psg-device.md §batch), atomically with this event.
- **Cassette wires**: motor/wdata changes are events consumed by the tape
  deck's batch timeline.
- Bestiary audit: class (a) discharged above; no counters; no idempotency
  exceptions (BSR and mode-set writes are idempotent per event).

Implementation (F6): the PPI is a pure event device between accesses —
`ppi_fast_io_write` applies one write through the shared `ppi_write` and
reports AY/tape line changes (relayed to `psg_fast_lines`);
`ppi_fast_io_read` takes the live inputs a Port B status read passes through
(VSYNC from the caught-up CRTC, rdata from the deck) and the AY bus value a
Port A input read latches (`psg_fast_read` in the READ state);
`ppi_fast_lines` exposes the published state for tier handover and the exit
bus synthesis. ORACLES: FastTierMachine.* (keyboard scans, the SOUND beep).
