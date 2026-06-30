# Cycle-exact, component-based CPC — reference-grade, SPARK-ready

**Status:** design locked, foundation landed · **Epic:** beads `Clean-room Z80 core` (rescoped) · **2026-06-30**

## What this is

Not "rewrite the Z80" — **rebuild the machine as uniform, reference-grade
components on a pin-level bus**, one chip at a time. The Z80 is just device #1.
The bar is *publishable*: a Z80 (and Gate Array, CRTC, …) anyone could lift into
another project and trust, and that a SPARK/Ada port could replace component by
component.

## Decisions (settled interactively, 2026-06-30)

1. **Reference-grade, component by component.** Datasheet truth, every chip at the
   same level of abstraction. Not "match legacy quickly."
2. **Central scheduler.** A board clock advances *every* device one T-state at a
   time (`board_tick`). The CPU has no special status.
3. **Pin-level bus.** Devices interact only by reading/driving bus lines, so CPC
   timing (e.g. the Gate Array holding the Z80 in WAIT to align accesses to 1 µs)
   *emerges* instead of being faked. Pure `(self, pins) -> (self, pins)` ticks are
   also what makes formal (SPARK) verification tractable.
4. **Pins are a named-field struct** (`Pins`), active-HIGH ("asserted"), not a
   packed word — clarity and a clean Ada-record mapping over density.
5. **No heap.** Caller owns each device's state (`*_state_size` + `*_init(storage)`);
   SPARK-friendly, and uniform per-device `save`/`load` gives free save-states.
6. **Device order = wiring**, documented where the CPC board is assembled.
7. **Interrupt/WAIT live on the bus** (`Pins`), not inside a device snapshot — a
   device `save` is just its own registers.

## The foundation (landed)

- `src/hw/pins.h` — the bus (`Pins`): address, data, and the Z80 control/interrupt
  lines, active-high, documented.
- `src/hw/device.h` — `Device`: the one interface every chip implements
  (`tick`, `reset`, `state_size`/`save`/`load`), no allocation.
- `src/hw/board.h` / `board.cpp` — the central scheduler: `board_tick` threads one
  `Pins` through every device in bus order; `board_reset`, `board_add`.
- `test/hw/board_smoke_test.cpp` — proves the model with two trivial devices (a
  RAM that answers `mreq+rd/wr`, a ticker), the clock, and save/load. All green,
  `-Wall -Wextra -Wconversion -Wshadow` clean.

## Oracle (truth hierarchy)

- **Primary = datasheet test suites:** FUSE Z80 tests and SingleStepTests/jsmoo
  (cycle-by-cycle). A divergence from these is a real bug *even if legacy agrees*.
- **Secondary = legacy Caprice32**, only as a whole-machine golden master for CPC
  integration where no unit oracle exists. We may end up *more* correct than it.

## Roadmap (each step: implement → standalone test green → Claude review → fix)

- **F. Foundation** — pins/device/board. ✅ done.
- **Z80-a** — the cycle-stepped Z80 FSM device: fetch/decode skeleton + the flag
  core, against FUSE. *(Decode-FSM style is the next open decision.)*
- **Z80-b … -e** — full documented set, then undocumented opcodes/flags/MEMPTR/Q,
  to zexall + jsmoo cycle-exact.
- **RAM/ROM, Gate Array, CRTC, PPI, PSG, FDC** — each a Device, datasheet-built,
  wired onto the board; whole-machine golden-master vs legacy.
- **Cutover** — board replaces the legacy core behind a flag; delete legacy.
- **SPARK** — re-implement chosen devices in SPARK/Ada behind the same `Device`
  contract, proven for AoRTE + functional contracts; same oracle.

## Still-open decisions

- **Z80 cycle-FSM decode style** (the hard one): hand-written per-T-state switch
  vs micro-op sequence vs generated decoder.
- **Reset/undefined values** per device (datasheet-pinned; document the undefined
  ones; reconcile vs the golden master at step 0).
- **CI integration**: wire `test/hw/` into the project `test_runner` so CI runs
  conformance, not just compiles the code.

## Clean-room hygiene

Build from the Zilog Z80 datasheet, Sean Young's *Undocumented Z80 Documented*,
the z80.info decode algorithm, and the public test suites. Never copy logic from
legacy `z80.cpp` — observe its *outputs* only, via the golden-master harness.
