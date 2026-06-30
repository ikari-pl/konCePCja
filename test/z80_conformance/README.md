# Z80 conformance & differential harness (Phase 0)

This directory holds the **oracle** for the clean-room Z80 core
(`src/z80core/z80core.h`). It proves that any implementation of the contract —
`legacy-caprice32`, `cpp-cleanroom`, `spark-ada` — behaves correctly, by testing
each one through the same C ABI.

See [docs/plans/2026-06-30-z80-cleanroom-core.md](../../docs/plans/2026-06-30-z80-cleanroom-core.md).

## Layers

| Layer | Source | What it catches | License |
|-------|--------|-----------------|---------|
| **Unit (per-opcode)** | FUSE `tests.in` / `tests.expected` (1,356 tests) | registers, flags, memory, per-cycle bus activity | GPLv2 (compatible) |
| **Unit (randomized)** | SingleStepTests / jsmoo z80 (10k/opcode, cycle-by-cycle) | exhaustive corner cases incl. undocumented flags, WZ | MIT |
| **Integration (firmware)** | zexall / zexdoc, run as a CPC program via headless+IPC | full-program correctness, undocumented opcodes | public |
| **Integration (golden-master)** | real CPC software corpus, per-frame VRAM/reg/mem hashes vs legacy core | CPC timing/interrupt/contention coupling | local |

## Differential runner (`z80_diff`)

The core idea: step **two** implementations of `z80core.h` against the **same**
bus, instruction by instruction, and assert their `z80_regs` snapshots are
identical after each step. On divergence, dump both snapshots + the offending
opcode and stop. This is what lets us reimplement with confidence — the legacy
core is the reference until the new core proves equal.

```
  shared bus (RAM + I/O stub)
        │
   ┌────┴────┐
   ▼         ▼
 impl A    impl B          for each step:
 (legacy)  (new)             A.step(); B.step();
   │         │               assert snapshot(A) == snapshot(B)
   └────►════◄┘               else: report first divergence
```

## Status

- [x] Contract defined (`src/z80core/z80core.h`)
- [ ] FUSE test-data importer + parser
- [ ] jsmoo importer
- [ ] `z80_diff` differential runner (drives the C ABI)
- [ ] legacy-core ABI adapter (Phase 1) — the first reference implementation
- [ ] CPC golden-master corpus + per-frame hash comparison

## Adding a new implementation

Implement every entry point in `z80core.h`, return a unique `z80_impl_name()`,
register it with the harness, and it is tested by all layers automatically — no
test changes required. That uniformity is the point: the SPARK core (Phase 6)
will be validated by exactly the suites the C++ core passes.
