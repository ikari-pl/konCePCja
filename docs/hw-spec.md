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

### 3.1 Timing contract (the binding numbers)

Everything timing-critical lives in one 1 µs window = **16 master cycles**,
indexed by `clk.phase` 0..15. The Gate Array owns this schedule; every other
device's correctness is defined relative to it. The Z80 FSM **must** be written
against this table, not against assumptions.

**Address-bus time-multiplexing (resolves "who drives `cpu.addr`").** The single
`cpu.addr`/`cpu.data` bus is time-division-multiplexed within each µs. In any
master cycle exactly one device drives it — so the single-driver invariant (§6)
holds and there is no arbiter:

| phase window | bus owner | activity |
|---|---|---|
| **video fetch** | Gate Array | GA drives `cpu.addr` = the CRTC video address (from `vid.ma`/`vid.ra`), asserts `mreq`+`rd`; RAM responds; GA latches `cpu.data` for pixel generation. |
| **CPU access** | Z80 | CPU drives `cpu.addr`, runs its memory M-cycle; RAM responds. |
| **idle/refresh** | Z80 (refresh) or none | DRAM refresh / bus idle. |

The CPU and the video fetch are in **disjoint phases**, so they never both drive
the bus in one master cycle.

**WAIT / µs alignment.** `cpu.wait` is **not** request-driven; the Gate Array
asserts it as a free-running function of `clk.phase`, holding the Z80 until its
memory M-cycle can begin in the CPU's slot of the µs grid. This is *the* reason
every CPC instruction takes a multiple of 4 T-states (1 µs). The CPU gets RAM in
**one of every four cycles**, giving an effective ~3.3 MHz. [cpcwiki Gate_Array
"1 in 4 cycles"; `docs/hardware/gate-array.md:130-135`]

#### What is pinned vs. convention — read this before coding the FSM

The research (full sources in the device catalogue) found the per-µs **aggregate**
is fully hardware-documented *and* matches the legacy golden master, but **no local
source pins the exact phase index 0..15** of the fetches/CPU-slot/WAIT edges, and
the **golden master has no sub-µs model** (its finest quantum is 1 µs; opcode
timings are pre-stretched to multiples of 4 in `cc_op[]`, `src/z80.cpp:889`, and
the world advances via `crtc_cycle(Tstates>>2)`, `src/z80.cpp:1110-1141`). So:

| Quantity | Value | Status |
|---|---|---|
| master÷4 → PHI (4 MHz), ÷16 → CCLK (1 MHz) | fixed | **HW-pinned** |
| video fetches per µs | **2 sequential** bytes (`addr`, `addr+1`), in the first ~½ µs | **HW-pinned** (golden master `crtc.cpp:945-948`) |
| CPU RAM access | 1 of 4 cycles; instruction total rounds up to a multiple of 4 T-states | **HW-pinned + golden-master** |
| split-raster mode latch | mid-character = **phase 8 (CPC)** / phase 4 (Plus) | **HW-pinned sub-µs** (the *only* one) |
| exact phase of the 2 fetches | fetch0 ≈ phases 0–3, fetch1 ≈ 4–7 | **CONVENTION** — no source pins it |
| exact CPU slot / WAIT-release phase | CPU ≈ phases 12–15; WAIT asserted ≈ 0–11 | **CONVENTION** |
| phase the Z80 samples WAIT | entering its slot | **CONVENTION** |

**Validation split:** the Z80's *internal* T-state timing is validated cycle-exact
by **FUSE/jsmoo** (independent of the GA). The CPC per-µs instruction totals + the
video address + the interrupt are validated by the **legacy golden master**. The
sub-µs GA WAIT/fetch phase edges are **convention**. No chip may hard-code a phase
constant that is not in this table.

#### The WAIT mechanism — HW-confirmed by Bread80's decapped-GA analysis

Bread80's electronic analysis of the real Gate Array (bread80.com, "Understanding
the … Gate Array Subsystem") pins the *mechanism* even though it gives no numbered
0..15 table:

- The GA runs a **16-step cycle** (one 16 MHz cycle per step) — confirms our phase
  model directly.
- **Video first, CPU after:** two video reads via DRAM **page mode** (two `/CAS`,
  `/CCLK` toggling the address LSB between them), and **the CPU's RAM window follows
  the second video `/CAS`**. So in phase terms the GA owns the early steps, the CPU
  the later ones (the exact boundary is the remaining convention).
- **The alignment rule (now exact and validatable):** *"three-clock-cycle M-states
  get stretched to four."* Each Z80 **memory read/write M-cycle (normally 3 T) is
  stretched to 4 T** by READY; the 4 T opcode-fetch M1 is unchanged. Summing M-cycles
  each rounded to 4 T is exactly why every CPC instruction total is a multiple of 4 —
  and it reconciles with the golden master's `cc_op[]` (e.g. `LD A,(HL)` = M1 4 T +
  read 3→4 T = **8 T**, matching `cc_op`).

So the Z80 FSM's WAIT contract is concrete: **insert one Tw into each 3 T memory
M-cycle** (the GA holds `cpu.wait` for that T). This is observable and golden-master-
validated; only the intra-µs *phase* at which the Tw lands stays convention.

#### Interrupt timing (HW-pinned; golden master + Grimware agree exactly)

The GA raises `cpu.irq` (/INT) by counting CRTC HSYNC falling edges in a 6-bit
counter `R52`:

- `R52++` on each HSYNC end (`crtc.cpp:796`); **at 52 → assert /INT, reset to 0**
  → 312/52 = **6 INT/frame = 300 Hz** (`crtc.cpp:798-802`).
- **Interrupt acknowledge:** when the Z80 accepts /INT, the GA does `R52 &= 0x1F`
  (clears bit 5), so no INT can recur within 32 lines (`src/z80.cpp:1674`).
- **VSYNC sync:** 2 HSYNCs after VSYNC begins, if `R52 >= 32` fire /INT; either way
  reset `R52 = 0` (`crtc.cpp:764, 806-813`) — the "+2 line" offset.
- First /INT of each frame at lines **2, 54, 106, 158, 210, 262** (the +2 form; a
  doc inconsistency with a "0,52,…" listing is resolved in favour of the golden
  master, which produces +2). RMR bit 4 clears pending INT + resets `R52`.
- /INT pulse is **1.4 µs** wide on real hardware; the golden master models it as a
  latched boolean, so pulse width is convention if modeled.

#### CRTC → video address (HW-pinned, all three sources + golden master agree)

The byte the GA fetches comes from MA/RA shuffled into a 16-bit address:
`A15..A0 = MA13 MA12 · RA2 RA1 RA0 · MA9..MA0 · CCLK`; A0 = CCLK toggles to read
the two consecutive bytes. MA10/MA11/RA3/RA4 are unconnected (the 2 KB stride).
[`docs/hardware/crtc.md:374-401`; golden master `MAXlate[]` + `crtc.cpp:1171`.]

**Producer/consumer phase skew.** Because the clock generator drives `clk` into
`out`, consumers read it from `in` one master cycle later. A device that **both
publishes and consumes** `clk.phase` (the real Gate Array does: it generates the
phase *and* acts on it for WAIT/fetch) must drive its decisions from its own
internal counter for the current cycle, and account for other devices observing
that phase one master cycle later. State this offset explicitly in the GA entry.

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

**Validity qualifiers.** Address-style lines have no "driven" flag of their own,
so their validity is gated by a strobe: `cpu.addr` is meaningful only while
`cpu.mreq || cpu.iorq || cpu.rfsh` is asserted; `vid.ma`/`vid.ra` only while
`clk.crtc` is asserted. A consumer must not act on a resting (0) address.

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
validation). The bus value itself is part of the snapshot (it carries in-flight
signal state). `board_save`/`board_load` are **(planned)** — not yet implemented;
only per-device `save`/`load` exist today.

For snapshots to be portable across hosts **and implementations** (the stated
goal), the byte encoding is fixed, not host-native:

- **Endianness: little-endian** for every multi-byte field.
- **Field order: explicit**, documented per device in §12 (a C `memcpy` of a
  struct is therefore *not* a conformant encoding unless the struct's layout
  already matches the documented order — demo devices that `memcpy` POD are a
  convenience, not the contract).
- Each device blob begins with a **1-byte format version**; a reader that sees an
  unknown version must **fail loudly** (not silently zero — the test stub's
  zero-on-mismatch is test-only, not the contract).
- Booleans serialize as one byte, `0`/`1`.

## 9a. Non-bus outputs (video, audio)

`tick` writes only into `Bus out`, which carries **inter-chip pin state only**.
Outputs that are not bus signals — the Gate Array's RGB pixels, the PSG's audio
samples — are emitted into **device-owned sinks reachable through `self`** (e.g.
the GA owns a framebuffer, the PSG owns a sample ring), read out-of-band by the
host. They are **never** added to the Bus. This needs no contract change; it is
stated here so the decision is not made ad hoc inside the first chip.

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
- **Z80 (CPC Z80A, NMOS)** — full implementation reference in
  [`docs/hardware/z80.md`](hardware/z80.md): registers, M-cycles, octal decode,
  MEMPTR/Q, interrupts, the 3 T→4 T WAIT stretch, FUSE/jsmoo verification.
- *(planned)* ROM, Gate Array ([`hardware/gate-array.md`](hardware/gate-array.md)),
  CRTC types 0–3 ([`hardware/crtc.md`](hardware/crtc.md)), PPI 8255, PSG AY-3-8912,
  FDC µPD765A ([`hardware/floppy-disc-system.md`](hardware/floppy-disc-system.md)).

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
| R4 | **Time-multiplexed shared address bus**: CPU and Gate-Array video fetch own `cpu.addr` in disjoint phases (§3.1) — no arbiter, single driver per cycle | 2026-06-30 |
| R5 | Serialization is **little-endian, explicit field order, fail-loud on version mismatch** (§9) | 2026-06-30 |
| R6 | **Non-bus outputs** (pixels, audio) via device-owned sinks in `self`, never the Bus (§9a) | 2026-06-30 |
| R7 | Timing research: per-µs aggregate **locked & golden-master-validated**; sub-µs phase indices are **documented convention** (only split-raster phase 8 is HW-pinned) — Z80 FSM unblocked | 2026-06-30 |

R1–R3 corrected the first foundation after review found a single ordered pass at
4 MHz on a Z80-only bus could not make CPC timing emerge. R4–R6 close the binding
contracts (timing/bus-mastering, serialization, non-bus output) the second review
required before the first real chip.

### Timing research outcome (R7) — the Z80 FSM is unblocked

Research against `docs/hardware/gate-array.md` + `crtc.md`, the cpcwiki dumps, and
the **legacy core as golden master** (2026-06-30) established:

- the per-µs **aggregate** (2 video fetches, CPU 1-in-4 / µs-rounded instructions,
  300 Hz interrupt with the `&0x1F` ack and +2-after-VSYNC offset, the MA/RA→addr
  mapping) is **fully pinned and golden-master-validated** — now in §3.1;
- the exact sub-µs **phase indices** are **not** pinned by any local source and are
  **not validatable** against the golden master (it has no sub-µs model), so they
  are recorded in §3.1 as **convention** (only the split-raster latch at phase 8 is
  hardware-pinned). Pinning the true edges needs an external cycle-exact reference
  (Bread80 GA signal analysis) — a deferred refinement, not a blocker.

The Z80 FSM may therefore proceed: validate its **internal** T-state timing against
FUSE/jsmoo, and its **CPC per-µs** behaviour against the golden master; treat the GA
WAIT phase as the documented convention until/unless the external reference is added.
