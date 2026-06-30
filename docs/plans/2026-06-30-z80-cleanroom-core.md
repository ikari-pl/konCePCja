# Clean-room, ABI-modular Z80 core (C++ now, SPARK-ready)

**Status:** planned · **Epic:** see beads `Clean-room Z80 core` · **Started:** 2026-06-30

## Why

konCePCja's inherited ~29% (the Z80 dispatch, gate array/CRTC timing, FDC, PSG)
is battle-tested 1997-era C-with-classes: correct because it's old, ugly because
it's old. The goal is to replace the **Z80 CPU core** first with a clean-room
implementation that is:

1. **Code to be proud of** — algorithmically decoded, no 1,268-entry macro table.
2. **Easier to test/extend** — a narrow, language-neutral seam with no global state.
3. **More accurate** — cycle-stepped (T-state accurate), the modern gold standard.

A fourth benefit comes free: written from datasheets (not their source), it is
genuinely *ours*, shrinking the inherited GPLv2 surface.

This is only safe because konCePCja already has the one thing most emulator
rewrites lack: an **oracle**. The IPC harness, 1,136 tests, headless mode and
`hash` commands let us prove a new core matches a reference bit-for-bit.

## The keystone: a C ABI contract (`src/z80core/z80core.h`)

The CPU is defined as a **language-neutral C ABI module**, not a C++ class. Any
implementation that exports the contract — C++, **Ada/SPARK**, Rust — can be
linked in and selected at build/run time. This is the decision that makes the
core modular and the SPARK path possible.

- The machine drives the CPU only through `z80_step` / `z80_irq` / etc.
- The CPU reaches the machine only through `z80_bus` callbacks (`read`, `write`,
  `in_`, `out`, `tick`).
- `z80_regs` is a flat POD snapshot of **all** observable + internal state
  (including `wz`/MEMPTR and the `q` SCF/CCF latch) — the basis of differential
  testing, and mappable 1:1 to an Ada record.

For a SPARK implementation, the bus callbacks are `Import, Convention => C` and
sit *outside* the proof boundary (they are the environment); the verified CPU
logic is decode + flags + sequencing. The contract header has zero C++ types so
GNAT can `pragma Export` the entry points.

## Architecture: cycle-stepped behind the seam

Advance one T-state at a time via `bus->tick`, because the CPC gate array derives
display timing and interrupts from the CPU clock — cycle granularity is where
accuracy lives. Decode via the octal `x/y/z/p/q` algorithm (z80.info), not a
table.

## The oracle (three layers)

1. **FUSE Z80 test suite** — 1,356 opcode tests with exact registers, flags,
   memory and per-cycle bus activity.
2. **SingleStepTests / jsmoo z80** — 10,000 randomized cycle-by-cycle tests per
   opcode.
3. **zexall / zexdoc** — runnable as a CPC program through the headless+IPC rig.
4. **Legacy core as integration oracle** — run real CPC software through old vs
   new in lockstep; diff per-frame VRAM/register/memory hashes.

All four target the **C ABI** — so the harness can drive any implementation
(legacy-wrapped, C++, SPARK) identically.

## Phases (each an independently mergeable PR — so it can't stall)

- **Phase 0 — Oracle rig.** Define the contract (done in this PR). Import FUSE +
  jsmoo into `test_runner`; build a `z80_diff` runner that steps two
  implementations on a shared bus and reports the first divergence; assemble a
  CPC software corpus for golden-master hashing. *No user-facing change.*
- **Phase 1 — The seam.** Wrap the **legacy** z80 behind `z80core.h` (an
  adapter), and make the CPC drive the CPU only through the contract. Pure
  refactor, diff-tested identical. Now there is a socket the new chip plugs into.
- **Phase 2 — Documented core (C++).** New cycle-stepped core: registers, real
  flag logic, documented opcodes + CB/ED/DD/FD + displacement. Gate: passes
  FUSE-documented + zexdoc.
- **Phase 3 — The nasty bits.** Undocumented opcodes (SLL, IXH/IXL), undocumented
  flags (XF/YF, bits 3/5), MEMPTR/WZ, the Q quirk (SCF/CCF), EI-delay, IM0/1/2,
  R register. Gate: passes zexall + full FUSE + jsmoo cycle-exact.
- **Phase 4 — CPC integration.** Couple `tick` to the gate array; interrupt
  timing; run the golden-master corpus headless until new == legacy bit-identical
  (or every difference is understood and justified).
- **Phase 5 — Cutover.** Default to the C++ core behind `--z80=<impl>`; soak;
  then delete legacy `z80.cpp`. ~3,000 lines of 1997 macro-soup leave the tree.
- **Phase 6 — SPARK/Ada core (north star).** Second implementation of the same
  contract, proven for absence-of-runtime-errors and key functional contracts
  (flag/decode correctness against a spec). Add GNAT/Alire to CI; validate with
  the same oracle rig. Selectable via `--z80=spark`.

## Clean-room hygiene

Write from: the Zilog Z80 datasheet, Sean Young's *Undocumented Z80 Documented*,
the z80.info decode algorithm, and the public test suites. **Never** open legacy
`z80.cpp` to copy logic — only observe its *outputs* through the diff harness.

## Risks & mitigations

- **Lost accuracy fixes** → the diff harness against the legacy core surfaces
  every divergence before cutover.
- **Performance regression** (cycle-stepped is more work) → modern cycle-stepped
  cores run a 4 MHz Z80 trivially; measure each phase, keep the legacy core
  selectable until parity is proven.
- **SPARK build complexity** → kept to Phase 6, fully optional; the C ABI means
  it never blocks the C++ path.

## Success criteria

- New C++ core passes FUSE + zexall + jsmoo and runs the golden-master corpus
  bit-identically headless.
- CPC drives the CPU exclusively through `z80core.h`.
- Legacy `z80.cpp` deleted.
- (Stretch) SPARK core selectable and passing the same suites with proofs green.
