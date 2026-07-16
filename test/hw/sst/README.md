# SingleStepTests (jsmoo) Z80 corpus — vendored slice

These `*.json` files are a **trimmed slice** of the SingleStepTests Z80 processor
tests, consumed by `test/hw/z80_singlestep_test.cpp` as a real-hardware oracle
for the CPU core (`src/hw/z80`).

- **Source:** <https://github.com/SingleStepTests/z80> (formerly jsmoo), **MIT
  licensed**. Copyright the SingleStepTests authors.
- **What's here:** the block-instruction families (`LDI/LDD/CPI/CPD/INI/IND/
  OUTI/OUTD` and their repeating `*R` forms) plus `SCF`/`CCF` (`37`/`3f`) — the
  cases FUSE cannot reach (no Q register, no incoming-Q, single-instruction).
- **Trimming:** the first **100** of each opcode's 1000 cases, with the
  per-T-state `cycles` array stripped (we compare final architectural state,
  memory, and port writes — not the ZX-specific bus trace). Regenerate or widen
  with `scripts/fetch-z80-sst.sh`.

Each case's `initial`/`final` carry the full register file including `q`
(the Q latch) and `wz` (MEMPTR); `ports` lists `[addr, value, "r"|"w"]`.

The corpus pins the post-2018 Banks/Rak undocumented-flag behaviour for the
repeating block ops; see `docs/hardware/z80.md` §6.
