# Dual-path PGO FPS — the shipping-config measurement (Gate B4, `beads-lcfa`)

Status: **measured** · 2026-07-07 · closes plan §9 / risk #5 (the "114 FPS was a
monomorphic `SOLDERED=1` build; the shipping dual-path number is unmeasured" gap).
Companion to [`../board-performance.md`](../board-performance.md) and
[`../plans/2026-07-05-001-two-mode-simulation-architecture.md`](../plans/2026-07-05-001-two-mode-simulation-architecture.md)
§10-B4.

## TL;DR

The **shipping default dispatch (pluggable `board_tick`) + PGO = ~117 FPS
headless** on the dev host — **matching the monomorphic `SOLDERED` + PGO ceiling
(~117 FPS) within measurement noise**, and at/above the previously-quoted 114.
Risk #5 is resolved: the shipping build pays **no meaningful throughput penalty**
versus the special monomorphic soldered build. Under whole-program LTO the
pluggable and soldered paths **converge** — the compiler already devirtualises and
inlines `board_tick`'s fn-pointer loop when it can see every device definition, so
soldering buys ~0 on top of LTO here.

## What "dual-path" means, precisely

`run_frame()` selects the dispatcher at **compile time** (`machine.cpp`):
`#ifdef SOLDERED` → `tick_soldered()` (direct, unrolled, measurement-only), else
`board_tick()` (the generic fn-pointer loop — the **shipping default**). The two
are mutually exclusive in one binary, so a literal "both dispatchers linked in"
build is not a current config. What risk #5 actually asks is: **what is the PGO
FPS of the dispatch that ships?** That is the pluggable `board_tick` path, measured
below. The soldered path is measured alongside as the reference ceiling.

## The measurement matrix

Apple M2 Ultra (24 cores), macOS 26 (Darwin 25.6), Apple clang (LLVM 21.0.0).
Headless — no SDL, no ImGui, no present path (the engine is compute-bound;
`board-performance.md` §1 measured render-wait 0 %, so headless throughput is the
honest engine number). Harness: `sim/bench_fps.cpp` — cold-boots `rom/cpc6128.rom`
into a `subcycle::Machine`, attaches an RGB24 framebuffer, feeds "no keys" each
frame, **200 warm-up frames excluded**, then times **2000 frames** with
`std::chrono::steady_clock`. Two runs per config (variance < 0.5 %). A rolling
FNV over the framebuffer is printed each run so the render work cannot be
dead-coded and a frozen/blank frame is visible.

| Build | dispatch | ms/frame | **FPS** | % of 50 Hz | fb checksum |
|---|---|---:|---:|---:|---|
| LTO, no PGO | pluggable *(default)* | 15.12–15.44 | **64.8–66.1** | ~130 % | `15a1…1bd0` |
| LTO, no PGO | soldered | 15.19–15.25 | 65.6–65.9 | ~131 % | `15a1…1bd0` |
| **LTO + PGO** | **pluggable *(shipping)*** | **8.56** | **116.8–116.9** | **234 %** | `15a1…1bd0` |
| LTO + PGO | soldered | 8.52–8.53 | 117.2–117.4 | 234 % | `15a1…1bd0` |

All four framebuffer checksums are **identical** (`15a12c920a991bd0`): the dispatch
path and PGO are observation-neutral, as required (plan §2). The instrumented
training run reports ~50–52 FPS — expected counter overhead, not a measurement.

## Findings

1. **Risk #5 closed.** The shipping pluggable path under PGO (~117 FPS) is **not**
   slower than the monomorphic soldered ceiling (~117 FPS); both sit at/above the
   quoted 114. The open question in plan §11 ("does soldered become canonical?")
   is answered on throughput grounds: **soldered earns essentially nothing over
   pluggable once LTO+PGO are on** — so the doctrine-clean pluggable Device
   contract can stay the default without a speed cost.

2. **Whole-program LTO collapses the pluggable↔soldered gap.** `board-performance.md`
   measured soldered at 1.52× pluggable, but that was a weaker-visibility build.
   With LTO giving whole-program visibility (the shipping default builds per-`.o`
   then links `-flto`; this bench compiles the same source set in one command —
   equivalent optimisation scope), the compiler devirtualises `dev[i].tick →
   ga_tick …` and inlines the loop body without needing the hand-unroll. Soldering
   then only removes an already-removed indirection.

3. **PGO ~doubles throughput** (66 → 117 FPS, ~1.77×) on either dispatch —
   consistent with `board-performance.md` calling PGO "the answer". It is the
   single biggest build-flag lever, and it applies equally to the shipping path.

## Relation to the Core 2 Duo 50-FPS target (plan §5)

This host is an M2 Ultra — far faster than the plan's real bar (a Core 2 Duo /
Atom, several times slower single-threaded, and this is single-threaded work).
Scaling 117 FPS down by a conservative 3–5× puts a Core 2 Duo at **~23–40 FPS —
still under the 50-FPS floor.** So PGO alone (pluggable *or* soldered) does **not**
clear the weak-hardware gate; the plan's §5 ceiling holds and **wake-on-clock (B6)
and SIMD (B7) remain necessary** for the stock CPC on weak silicon. What this
measurement *does* settle is narrower and real: the shipping binary is not paying a
hidden dual-path tax relative to the 114 headline — that fear is retired. The
weak-hardware number itself must still be measured on weak hardware (plan §11,
risk #7), not extrapolated from here.

## How to reproduce

Two-phase PGO is productionised as a `make` target (macOS/Linux only — the flow is
clang/llvm; the target hard-errors on MINGW and never adds PGO/LTO flags to the
Windows toolchain):

```bash
# Shipping dual-path (pluggable board_tick) + PGO — instrument → train → merge →
# optimise → measure, all in one target:
make pgo

# The monomorphic soldered ceiling:
make pgo SOLDERED=1

# Plain release-tier baseline (no PGO), either path:
make bench            # pluggable
make bench SOLDERED=1 # soldered

# Tunables: PGO_TRAIN_FRAMES (default 1500), PGO_BENCH_FRAMES (default 2000).
```

`make pgo` builds `sim/bench_fps.cpp` + the `src/hw` + `src/subcycle` source set
instrumented (`-fprofile-instr-generate`), runs the fixed headless cold-boot trace
with `LLVM_PROFILE_FILE` to emit `koncepcja_bench.profraw`, merges it
(`llvm-profdata merge` → `koncepcja_bench.profdata`), rebuilds with
`-fprofile-instr-use`, and prints the FPS. The bench returns from `main()` normally,
so LLVM's atexit writer flushes the counters — no `%c` continuous-mode needed here
(that is only required for the `koncepcja` binary, whose `cleanExit()` calls
`_exit()`; see the `Makefile` RELEASE_FLAGS note). Artefacts are git-ignored and
removed by `make clean`.

## Caveats

- Headless throughput, not GUI. The GUI adds the Metal present path; `board-performance.md`
  §1 measured render-wait 0 % (compute-bound), so the GUI number trails headless
  only by present overhead, not engine cost.
- Single-command compile ≈ per-`.o` + `-flto` link in optimisation scope, but is
  not byte-identical to the shipping link line. The *ranking and magnitude* hold;
  treat absolute FPS as ±a few percent versus a full `koncepcja` LTO build.
- Numbers are this host on this date. The gate that matters (plan §11 risk #7) is a
  measurement on real weak hardware, still outstanding.
