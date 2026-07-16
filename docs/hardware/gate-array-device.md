# Gate Array — sub-cycle, pin-level Device simulation reference

Language-neutral spec for the sub-cycle Gate Array **Device** (`src/hw/gate_array`),
the companion to `docs/hardware/z80.md`. For the real-chip detail see the hardware
reference `docs/hardware/gate-array.md`; **this** doc is the spec the Device code
follows — the clock/WAIT/interrupt model and the Device contract.

The GA is the heart of CPC timing: it divides the master clock, holds the Z80 with
WAIT to quantise it to microseconds, generates the 300 Hz raster interrupt, and owns
the palette / screen mode / ROM-RAM banking registers. See also `docs/hw-spec.md`
(the system bus/Device model).

Scope note — **build order** (agreed): the **timing + interrupt core** (§2–§4) lands
first, validated against the legacy `cc_op[]` golden master, with the CRTC stubbed by a
synthetic HSYNC/VSYNC source. The **register side** (§5) and the real CRTC follow as
later slices; they are specified here so the whole chip is documented, but not yet
implemented.

---

## 1. Role and bus position

- Sits between the 16 MHz crystal and every other chip: it is the **clock generator**
  (`Clocks` on the bus — `clk.cpu`, `clk.crtc`, `clk.psg`, `phase`).
- Drives **`cpu.wait`** to stall the Z80 (the µs quantiser, §3).
- Watches **`vid.hsync` / `vid.vsync`** (from the CRTC) and drives **`cpu.irq`** — the
  raster interrupt (§4).
- Decodes I/O writes with **A15=0, A14=1** (`(addr & 0xC000) == 0x4000`): the top two
  data bits select the function (§5).
- A device like any other: `tick(in,out)` once per 16 MHz master cycle.

---

## 2. Clock division

The 16 MHz master is divided by a free-running 4-bit phase counter (`phase`, 0..15,
one 1 µs window):

| Enable | Divisor | Asserted when | Consumers |
|---|---|---|---|
| `clk.cpu`  | ÷4  | `phase & 3 == 0` (phase 0,4,8,12) | Z80 advances one T-state |
| `clk.crtc` | ÷16 | `phase == 0`                      | CRTC advances one char |
| `clk.psg`  | ÷16 | `phase == 0` (÷4 further in the PSG for 125 kHz) | PSG |

So one **1 µs window = 16 master cycles = 4 CPU T-states = 1 CRTC cycle**. The exact
sub-µs phase at which each enable fires is a convention (locked in `hw-spec.md §3.1`);
what matters is the ratio and that `clk.cpu` fires exactly 4× per µs.

### 2b. Video fetch slots — the GA owns the RAM address multiplexer (bus DMA)

On the real board the Z80's address bus never reaches the DRAM pins: the GA (with the
two 74LS153 muxes) drives the RAM address, interleaving the CPU's access with its own
**two video fetches per µs** (a page-mode pair — the two display bytes of the current
CRTC character differ only in A0). The µs window splits as:

| Phases | Owner | Activity |
|---|---|---|
| 0–11 | CPU slot | the mux passes the CPU address through (modeled: memory answers the `cpu` bus) |
| 12–15 | video slots | GA drives `ram.addr = vid_byte_addr(MA, RA, k)` + `ram.fetch` at phase **12** (k=0) and **14** (k=1); RAM answers on `ram.data` one master cycle later |

The fetches run **every µs regardless of DISPEN** — the slots are always reserved,
which is why CPC timing is uniform (no Spectrum-style contended memory). Video fetches
always read **RAM**, never a ROM overlay (the ROMs sit on the CPU bus). The pixel
serializer (`video` Device) latches byte 0 off the bus at `clk.phase == 13` and paints
the character cell when byte 1 lands at `phase == 15`.

---

## 3. The WAIT quantiser — µs alignment (the timing crux)

The CPC makes every instruction take a whole number of microseconds. The mechanism is
**per-M-cycle alignment**: the GA asserts WAIT during 3 of every µs's 4 T-states, so
the T1 of **every bus machine cycle** — opcode fetch (M1), memory read, memory write,
I/O, interrupt acknowledge — can only land on a µs boundary. Internal cycles touch no
bus and run freely; the next bus M-cycle then re-aligns to the grid.

This supersedes an earlier fetch-only model (`cpc_time = roundup(raw, 4)`), which
agrees on single-access opcodes but **undercounts any instruction with several bus
M-cycles**. Decisive cases from the legacy `cc_op[]` golden master (§6):

    PUSH BC     raw 11 (M1 5 + write 3 + write 3)      cc_op = 16   roundup → 12 ✗
    LD HL,(nn)  raw 16 (M1 4 + read 3 ×4)              cc_op = 20   roundup → 16 ✗
    EX (SP),HL  raw 19 (4 + 3 + 4 + 3 + 5)             cc_op = 24   roundup → 20 ✗
    INC (HL)    raw 11 (4 + read 4 + write 3)          cc_op = 12   both models 12
                (the earlier doc miscounted this case as distinguishing — the read
                 M-cycle's internal T4 tucks inside its µs, so both models agree)

Worked PUSH example: M1 occupies µs0 (its 5th T spills into µs1); write 1 must then
wait for µs2, write 2 for µs3; the next fetch aligns to µs4 → 16 T. ✓

**Modelling in the Device (IMPLEMENTED, validated against cc_op):** at T1 of any
non-INTERNAL machine cycle the Z80 holds until `clk.phase == 0` (the µs boundary the
GA publishes on the clock bus). Reading `clk.phase` directly — rather than a derived
`cpu.wait` pulse — sidesteps the one-hop bus latency between "GA asserts wait" and
"CPU samples it", and it is equivalent (the phase *is* what the GA's /WAIT is derived
from). Key property: with the always-on test clock `phase` is always 0, so the hold is
a no-op there — raw datasheet timing (and the 1356-test FUSE suite) is unaffected;
quantisation engages only under the real GA.

This alignment is also what makes the video DMA (§2b) contention-free by construction:
every CPU bus access starts at phase 0 and its strobes complete by phase 11, leaving
phases 12–15 — the GA's video slots — untouched.

---

## 4. Raster interrupt (300 Hz)

A 6-bit line counter, `sl_count` (legacy `GateArray.sl_count`), driven by HSYNC:

- **Increment** `sl_count` on each falling `vid.hsync` (HSYNC end, one per
  scanline). This edge is hardware-pinned by the legacy golden master and the
  CPC timing references; using HSYNC start shifts conditional raster ISRs
  relative to VSYNC.
- **Fire** at `sl_count == 52`: assert the Z80 INT (`cpu.irq`), then `sl_count = 0`.
  312 lines/frame ÷ 52 = 6 interrupts per 50 Hz frame → **300 Hz**.
- **Acknowledge**: the Z80's real `m1`+`iorq` acknowledge cycle (the Z80's `MC::IOACK`)
  makes the GA do `sl_count &= 0x1F` — clearing **bit 5** — and drop the INT line. The
  classic CPC behaviour that lets a long-delayed interrupt still resynchronise. Wired
  end to end (GateArray.EndToEndInterruptAcceptedAndAcknowledged).
- **VSYNC resync**: on `vid.vsync` the GA arms `hs_count = 2`; after **2 more HSYNCs**
  it forces `sl_count = 0` (with the legacy "≥32 → also fire now" save-margin so an
  interrupt isn't skipped near the boundary). Keeps interrupts phase-locked to the
  frame regardless of CRTC register tricks.
- **Mode-register bit 4** (§5, function 2): writing it clears any pending interrupt and
  resets `sl_count = 0` (software interrupt-rearm / "interrupt delay").

The GA holds `cpu.irq` until the CPU acknowledges; the Z80 side already gates
acceptance on `IFF1` and an instruction boundary (see z80.md §6).

CRTC dependency: for the first slice, `vid.hsync`/`vid.vsync` come from a **synthetic
generator** in tests (a fixed 64-µs line, 312-line frame). The real CRTC (HD6845)
arrives as its own Device later and simply drives the same two lines.

---

## 5. Register side (specified; implemented in a later slice)

I/O write with `(addr & 0xC000) == 0x4000`; function = `data >> 6`:

| `data>>6` | Function | Payload |
|---|---|---|
| 0 | **Pen select** | `data & 0x0F` = pen 0–15; `data & 0x10` = border |
| 1 | **Set ink** | `data & 0x1F` = one of 32 hardware colours for the selected pen |
| 2 | **Screen mode + ROM + INT** | bits1:0 = mode 0/1/2(/3); bit2 = lower-ROM enable; bit3 = upper-ROM enable; **bit4 = interrupt-delay/rearm (§4)** |
| 3 | **RAM banking** (6128/expansion) | `data & 0x3F` = RAM config; decoded with `A15=0` |

- **Palette**: 16 pens + border, each an index into the 27-colour CPC hardware palette
  (stored as 32 entries incl. the 5 duplicates); mode 0 uses 16 pens, mode 1 uses 4,
  mode 2 uses 2.
- **Screen mode** latches at the next HSYNC (`requested_scr_mode` → `scr_mode`), not
  mid-line.
- **ROM banking**: lower ROM (firmware, 0x0000–0x3FFF) and upper ROM (0xC000–0xFFFF)
  enable bits gate what the memory map returns — really a memory-controller concern the
  GA drives.

---

## 6. Verification

**Golden master = the legacy `cc_op[]`/`cc_cb[]`/`cc_ed[]`/`cc_xy[]`/`cc_xycb[]` tables**
(`src/z80.cpp:889`+), the CPC-adjusted per-opcode T-state totals. Since these equal
`roundup(raw, 4)` and our Z80 already matches the raw datasheet totals (FUSE 100%), the
timing test is: run each opcode through the GA-quantised Z80 and assert the instruction
total equals `cc_op[opcode]`. This is the CPC counterpart to the raw-T FUSE check,
closing the loop end to end.

Raster interrupt: unit-test `sl_count` against a synthetic HSYNC/VSYNC stream — fire at
52, `&0x1F` on ack, 2-HSYNC-after-VSYNC resync, mode-bit-4 rearm.

---

## 7. Mapping to our Device model

- The GA `tick(in,out)` runs every master cycle. It owns `out->clk.*` and drives
  `out->cpu.wait` and `out->cpu.irq`; it reads `in->vid.hsync/vsync` and the CPU bus
  for I/O decode.
- Caller-owned, no-heap: `ga_state_size()` + `ga_init(storage)`, uniform
  `tick`/`reset`/`state_size`/`save`/`load` like every Device (spec §5).
- It replaces the test clock stub: on a real board it is device #0, feeding the Z80,
  CRTC, and PSG their enables.

## Batch contract (RunTier::Fast)

- **Events consumed**: HSYNC rising edge (latch req_mode → mode), HSYNC falling
  edge (advance the 52-line raster counter, §4), VSYNC rising edge (arm the
  2-HSYNC resync),
  CPU INT-ack (clear counter bit 5 + the INT line), I/O writes to the GA
  select (pen / ink / mode+ROM / RAM-config — each catches the video renderer
  up FIRST, then applies; ink and mode writes are the raster-trick hot path).
- **Predictable deadline**: `next_irq_at` — pure geometry from the CRTC's
  scanline cadence and the current sl_count/hs_count state. Recomputed on any
  event that touches the counter (rearm write, VSYNC resync, ack). This is
  the primary term of the scheduler's horizon.
- **Clock fabric**: per-cycle tiers synthesize it via ga_clock_out; in Fast
  the clock IS the timeline — no per-cycle outputs exist. /WAIT lives inside
  the Z80's CPC-grid instruction timing (z80.md §batch).
- **Wired-OR /INT**: shared with the ASIC on a Plus — the scheduler owns the
  OR of both drivers' predicted assertions.
- Bestiary audit: raster counter = class (b) timestamped state → advance is
  exact edge counting, no free-running term; no pipeline publishes; ink/mode
  application is idempotent per event.

Batch implementation (`gate_array.h` §batch): the GA is edge-driven in the
Fast tier — `ga_batch_hsync_rise` (mode latch), `ga_batch_hsync_fall` (raster
count),
`ga_batch_vsync_rise` (resync arm), `ga_batch_int_ack` (the M1+IORQ
signature, via Z80BatchIO.int_ack), `ga_fast_io_write` (the shared ga_write
decode). The ÷16 clock fabric needs no runtime: it is closed-form arithmetic
in the Z80 batch driver. `ga_predict_irq_hsyncs` dry-runs ga_raster_count on
a shadow for the scheduler's next_irq_at horizon. An irq change from CRTC
char k is CPU-visible at T-state 4k+1. ORACLES: `FastVideoChain.*`.
