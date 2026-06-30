# konCePCja Hardware Specification

A **language-neutral, reimplementable** specification of the cycle-exact Amstrad
CPC modeled as uniform components on a pin-level bus. The goal: someone could
build this machine — or any single chip — from this document alone, in C++,
SPARK/Ada, Rust, or anything with structs and function calls. The code in
`src/hw/` is one implementation of this spec; this document is the source of
truth. It grows as chips are added.

Struct shapes shown below are reference layouts: they map 1:1 to an Ada record,
a Rust `#[repr(C)]` struct, or a C struct. Nothing here depends on a language
feature beyond plain records and function pointers/closures.

---

## 1. Philosophy

- **Reference-grade.** Truth is the chip datasheets and the public conformance
  suites (FUSE, SingleStepTests/jsmoo), not the legacy emulator. We may end up
  *more* correct than legacy.
- **Uniform.** Every chip — CPU, Gate Array, CRTC, PSG, FDC, PPI, RAM, ROM — is a
  **Device** with the identical interface. The CPU is not privileged.
- **Cycle-exact by emergence.** Timing (interrupts, the 1 µs access stretch,
  pixel output) is not special-cased; it falls out of modeling the real clock and
  the real bus lines.
- **Pure & allocation-free.** A device tick is a pure transition over
  `(state, bus)`. No globals, no heap. This is what keeps a SPARK port tractable.
- **Cost is accepted.** This is several× slower than an inline interpreter. It is
  a reference, not the daily-driver core (the legacy core remains for that).

---

## 2. Architecture overview

A **Board** owns a set of **Device**s and one **Bus** value. It advances the
whole machine by ticking at the **16 MHz master clock**. Each master cycle is a
**synchronous, two-phase** update:

1. **Sample + drive.** Every device reads the *committed* bus (the value from the
   end of the previous master cycle) and writes the lines it owns into a fresh
   *next* bus that starts in the floating/resting state.
2. **Commit.** The next bus becomes the committed bus.

Because every device reads the same frozen committed bus and only writes its own
lines, **the result is independent of device order** — unlike a single ordered
pass. Signals therefore propagate **one master cycle per hop** (CPU drives an
address → memory sees it next cycle → CPU sees the data the cycle after). At
16 MHz (62.5 ns/cycle) this latency is far finer than any chip's own clock
(4 MHz CPU = 250 ns), so it is invisible to correct timing; it is documented here
so device FSMs account for it.

---

## 3. The clock

- The master oscillator is **16 MHz**. One `board_tick` = one master cycle.
- The **Gate Array** divides it and publishes **clock enables** on the bus:
  - `clk.cpu` — asserted at **4 MHz** (every 4th master cycle): the Z80 advances
    one T-state this cycle.
  - `clk.crtc`, `clk.psg` — asserted at **1 MHz** (every 16th master cycle).
  - `clk.phase` — the master-cycle index `0..15` within the 1 µs window, for
    pixel timing and for the Gate Array's WAIT generation.
- A device that runs below 16 MHz does work only when its enable is set; it still
  gets ticked every master cycle (uniformity), it just no-ops when not enabled.
- Until the real Gate Array exists, a **stub clock generator** device publishes
  the enables from a master-cycle counter.

The CPU is therefore **not** the clock master: it gates on `clk.cpu` like any
other chip, satisfying the "no privileged device" rule.

---

## 4. The buses

Split into two physical buses plus the clock fabric, bundled as one `Bus` value
threaded through every device. Lines are modeled **active-HIGH** (`true` =
asserted); real Z80 control lines are active-low (/MREQ …), flipped once here so
device logic reads as plain boolean (`if (in.cpu.mreq && in.cpu.rd)`).

### 4.1 CpuBus — the Z80 / memory bus

| Field | Bits | Driver | Meaning |
|------|------|--------|---------|
| `addr` | 16 | CPU (or DMA master) | address bus A0–A15 |
| `data` | 8 | whoever is enabled | data bus D0–D7; **floating = 0xFF** |
| `m1` | 1 | CPU | opcode-fetch machine cycle |
| `mreq` | 1 | CPU | memory request |
| `iorq` | 1 | CPU | I/O request |
| `rd` | 1 | CPU | read strobe |
| `wr` | 1 | CPU | write strobe |
| `rfsh` | 1 | CPU | refresh cycle (DRAM row on `addr`) |
| `halt` | 1 | CPU | CPU is halted |
| `wait` | 1 | Gate Array | hold the CPU (the 1 µs access stretch) |
| `irq` | 1 | wired-OR of sources | maskable interrupt (the /INT line) |
| `nmi` | 1 | source | non-maskable interrupt |
| `reset` | 1 | reset source | system reset (held ≥3 CPU clocks) |
| `busrq` | 1 | DMA | bus request |
| `busak` | 1 | CPU | bus acknowledge |

### 4.2 VidBus — the CRTC ↔ Gate-Array video/timing bus

| Field | Bits | Driver | Meaning |
|------|------|--------|---------|
| `ma` | 14 | CRTC | refresh memory address MA0–13 |
| `ra` | 5 | CRTC | row address RA0–4 |
| `hsync` | 1 | CRTC | horizontal sync (GA counts these → /INT) |
| `vsync` | 1 | CRTC | vertical sync |
| `dispen` | 1 | CRTC | display enable (border vs active) |
| `cursor` | 1 | CRTC | cursor match |

### 4.3 Clocks

| Field | Driver | Meaning |
|------|--------|---------|
| `cpu` | Gate Array | 4 MHz CPU T-state enable |
| `crtc` | Gate Array | 1 MHz CRTC enable |
| `psg` | Gate Array | 1 MHz PSG enable |
| `phase` | Gate Array | `0..15` master-cycle phase in the 1 µs window |

`Bus = { CpuBus cpu; VidBus vid; Clocks clk; }`

---

## 5. The Device contract

A Device is a small value: a name, a `self` pointer to caller-owned state, and
function pointers. It owns no memory.

```
Device {
  self        : pointer to device state (caller-owned; must outlive the Board)
  name        : stable identifier ("z80", "gate-array", ...)
  tick(self, in: *const Bus, out: *Bus)   // one master cycle
  reset(self)
  state_size(self) -> bytes               // size of the save blob
  save(self, buf)                         // serialize logical state only
  load(self, buf)
}
```

- **Lifetime.** `self` storage is provided by the caller (stack/static/BSS),
  sized by the module's free function `<dev>_state_size()` and initialized by
  `<dev>_init(storage) -> Device`. No heap (SPARK-friendly; trivial save-states).
- **`tick` rules** (this is the crux of order-independence):
  - Read only from `in` (the committed bus). **Never read `out`** except for
    wired-OR lines (below).
  - `out` arrives pre-set to the **floating/resting** state (§6). Write only the
    lines this device drives.
  - **Owned / tri-state lines** (addr, data, mreq, hsync, …): **assign**. Exactly
    one device drives each in any cycle.
  - **Wired-OR lines** (`irq`): **OR in** (`out.cpu.irq |= mine`). Order-independent
    because OR is commutative.
- **Serialization.** `save`/`load` move **logical register state only** — never
  raw host pointers. Each device's blob begins with a 1-byte format version.

---

## 6. Bus resolution & the floating/resting state

Each master cycle, `out` is initialized to the resting state before any device
runs:

- `cpu.data = 0xFF` (pull-up; an unread bus floats high on the CPC).
- All boolean control/sync lines = `false` (deasserted).
- `cpu.addr`, `vid.ma`, `vid.ra` = 0 (no driver ⇒ undefined; 0 by convention).
- `clk.*` = false / phase 0 (the clock generator re-drives them each cycle).

Resolution is therefore implicit: a line keeps its resting value unless its owner
assigns it (or, for `irq`, unless any source OR's it). A conformant build *may*
add conflict detection (two assigners in one cycle = bug) behind a debug flag.

---

## 7. The tick algorithm

```
board_tick(board):
    next = floating_resting_bus()
    for dev in board.devices:           # order does not affect the result
        dev.tick(dev.self, &board.bus, &next)
    board.bus = next
    board.master_cycles += 1
```

`board_reset` deasserts the bus and calls each device's `reset`. (Reset is also
exposed as `cpu.reset` on the bus for devices that model the reset sequence in
their FSM; see §8.)

---

## 8. Reset model

Two mechanisms, with defined roles:

- `Device.reset(self)` — **power-on initializer**: puts the device in its
  documented cold-boot state. Called by `board_reset`.
- `cpu.reset` **bus line** — the **runtime reset sequence**: a reset source holds
  it asserted for ≥3 CPU clocks; devices that care (the Z80) sample it in `tick`
  and perform their datasheet reset timing. This keeps runtime reset *emergent*
  like every other signal.

`board_reset` uses the first; a soft reset during run uses the second.

---

## 9. Serialization

The whole-machine snapshot is the concatenation of each device's `save` blob,
in board order, prefixed by a machine-level header (device count + names, for
validation). Because devices serialize logical state (not pointers), snapshots
are portable across hosts and implementations. The bus value itself is part of
the machine snapshot (it carries in-flight signal state).

---

## 10. Verification (oracle)

- **Per chip (primary):** datasheet conformance suites. For the Z80: FUSE
  (registers/flags/memory + per-cycle bus activity) and SingleStepTests/jsmoo
  (10k randomized cycle-exact tests/opcode). A divergence is a real bug **even if
  the legacy core agrees**.
- **Whole machine (secondary):** golden-master — run real CPC software headless
  through both this board and the legacy core, diff per-frame VRAM/register/audio
  hashes. Used where no unit oracle exists (CPC integration, video).

---

## 11. Conventions

- Active-HIGH line modeling, flipped once at the hardware I/O boundary (§4).
- Names are the real signal names (`irq`, not `int_`; `mreq`, `hsync`).
- Fixed-width integer fields; booleans for single lines.
- C-ABI-clean reference layout: a SPARK/Ada or Rust device maps the structs 1:1.

---

## 12. Device catalogue

Grows as chips land. Each entry: datasheet refs, lines driven/observed, the
clock enable it runs on, internal state, reset state, and conformance oracle.

- **clock-gen (stub)** — drives `clk.cpu` (÷4), `clk.crtc`/`clk.psg` (÷16),
  `clk.phase` (master counter mod 16). Placeholder until the Gate Array.
- **RAM** — responds to `mreq`+`rd`/`wr` on `addr`; drives `data` on read. (banking
  via the Gate Array later.)
- *(planned)* ROM, Z80, Gate Array, CRTC (types 0–3), PPI 8255, PSG AY-3-8912,
  FDC µPD765A.

---

## 13. Decision log

| # | Decision | Date |
|---|----------|------|
| D1 | Reference-grade, every chip a uniform component | 2026-06-30 |
| D2 | Central scheduler; CPU has no special status | 2026-06-30 |
| D3 | Pin-level bus | 2026-06-30 |
| D4 | Pins/bus as named-field structs (not packed words) | 2026-06-30 |
| — | No heap; caller-owned device state | 2026-06-30 |
| — | Active-high line modeling, flipped once | 2026-06-30 |
| R1 | **16 MHz master clock** with GA-driven enables (not 4 MHz T-state) | 2026-06-30 |
| R2 | **Two-phase synchronous** bus (sample committed → drive floated → commit); order-independent; per-hop latency | 2026-06-30 |
| R3 | **Split buses** CpuBus + VidBus (+ Clocks) | 2026-06-30 |
| — | Z80 FSM style: **micro-op sequences** | 2026-06-30 |

R1–R3 corrected the first foundation after review found a single ordered pass at
4 MHz on a Z80-only bus could not make CPC timing emerge.
