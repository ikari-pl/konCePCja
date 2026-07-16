# One faithful simulator, several fast ways to run it — the plan

**Status:** design, first-principles rewrite · **2026-07-05** · supersedes the first draft of this file.
**Builds on:** [`2026-06-30-cycle-exact-machine.md`](2026-06-30-cycle-exact-machine.md) (the pin-level
component foundation) and [`../board-performance.md`](../board-performance.md) (the cost analysis).
**Grew out of:** a four-lens adversarial review (adversarial / feasibility / scope / architect) and a
first-principles conversation. The decisions and their reasons are logged in §12.

---

## 1. Why this exists (plain, first principles)

We are copying a 1980s computer — the Amstrad CPC: a handful of chips wired together, all marching
to one clock. The only way to be **truly faithful** is to do what the real machine does — nudge every
chip a tiny step at a time and let them talk over the wires. That faithfulness has **exactly one
problem**, and this entire plan exists to solve just that one problem:

> **Being faithful to the hardware is expensive — expensive enough that, done the obvious way, it
> cannot run at the real machine's speed on the machines people actually own.**

The trap we nearly fell into: we measured "fast enough" on an **M2 Ultra**, which is already ~2× a
typical computer. The real bar is a **Core 2 Duo / Atom** — several times slower. Our faithful engine
does ~40 FPS on the M2 Ultra; on a Core 2 Duo that is single-digit FPS. Even the current *fastest*
build (soldered + PGO, ~114 FPS here) lands well under 50 there. **The stock CPC must run at 50+ FPS
for everyone, on weak hardware.** WinAPE managed 400 % on 15-year-old silicon; that is the bar to
beat, not to admire.

If faithfulness were free we would only ever run the faithful way — one thing, no plan. It isn't free.
So we must **buy speed without giving up faithfulness.** How to do that safely is the whole plan.

---

## 2. The two words everything hangs on

`★ observations vs observability ────────────────`
- **Observations** — what the *emulated program* can detect: the screen, the sound, and above all the
  **timing** of every event (raster position, interrupt cycle, exactly when a read returns what).
  Demos live or die on this. **Every mode must reproduce observations exactly, timing included.**
  This is the floor. It is why "fast" can never mean "approximate": firing an event at the wrong
  time is not a small error — it *breaks the emulator*.
- **Observability** — what *we* (the developer) can peek: each chip's pin at each cycle phase, the
  waveforms, the insides. This is the **simulator's gift**, and it is the thing a fast mode is allowed
  to **trade away for speed**.
`────────────────────────────────────────────────`

"Approximation," now defined precisely: **getting observations wrong.** Forbidden, in every mode.
Trading observability: allowed — that is the speed lever. A fast mode may be a black box about its
*insides*, but never about its *timing-visible behaviour*.

**The bar for "observations" is set by the most timing-sensitive software** — CPC demos that split the
screen and re-time interrupts every scanline. So "fast" is not free-form: it must be **cycle-accurate
in observable effect**, just not necessarily inspectable inside. This is exactly why a fast mode can
never be a behavioural / high-level-emulation shortcut — those get observations wrong.

---

## 3. Three axes, not "two modes"

The false binary in the first draft ("faithful engine vs fast engine") hid that there are really
**three independent axes**. Any way of running the machine is a point in this space:

1. **Speed** — how fast it runs (compiler build tier: `-O2` → LTO → PGO → soldered direct dispatch → SIMD → event-driven scheduling).
2. **Observability** — how much of the insides you can watch (nothing → the standard monitors → full per-pin-per-phase scopes).
3. **Composability** — can you swap/add chips at runtime (fixed stock CPC → hot-swap a SPARK CRTC → build a *custom* machine, e.g. a SID in place of the AY).

"Modes" are just useful *presets* over these axes. Keeping the axes separate is what stops us from
building heavy machinery to reconcile things that were never actually coupled.

---

## 4. The three named levels (presets over the axes)

1. **Pluggable simulator** — full observability *and* full composability. The sub-cycle hardware
   debugger *and* the machine-builder (swap a SID for the AY; build the hypothetical "Ultra"). Slowest.
   This is the **reference**: the definition of what our model says the machine does.
2. **Soldered** — the *same* standard machine with its core chips hardwired for speed. **A faithful
   implementation of the simulator, not a different one** — so it may become **canonical** for the
   stock CPC. It mainly trades *composability* (you can't unplug the Gate Array); it keeps observations
   and can keep much observability. (It is "technically hard to turn off the Gate Array" — and that's
   fine, because the stock machine never wants to.)
3. **Fast** — trades *observability* for raw speed (e.g. wake-on-clock scheduling, SIMD), but **still
   reproduces observations exactly.** This is the one that gets a Core 2 Duo to 50+ FPS.

Composability earns its keep precisely where soldered can't go: **custom machines** (SID-for-AY, the
Ultra). Soldered is the fast stock CPC; pluggable is the workbench where you build a machine that never
existed.

---

## 5. The every-tick loop is the reference; faster methods are the speedup

Settled (§12, decision Q3): **the every-16 MHz-tick loop stays as our hardware simulator — the
reference.** It is the simplest, most obviously-correct scheduler for the machine. Faster methods
(wake each chip only on its own clock edge; let wires hold their level between changes) are **speedups**
that must reproduce the reference's *observations*. Inside, a fast method may do whatever it wants.

**Maintainability rule (settled):** prefer **one body of chip logic, wired two ways** (the loop and the
fast scheduler both call the same `*_tick` code — which is already true of `board_tick` vs
`tick_soldered` today). Where a genuine speed technique *cannot* reuse the shared logic, we **plan the
split deliberately** — factor the chip so both worlds share the semantics and differ only in scheduling.
No accidental fork into two divergent emulators.

The honest reframe (architect review): neither scheduler is "the truth." The truth is each chip's
**clock-wake contract** — "which clock wakes me, what I do on that edge." The tick-loop is the trivial
scheduler over that contract; wake-on-clock is a smart scheduler over the *same* contract. Make the
contract explicit and the two are equal by construction — the loop becomes the checker, the fast
scheduler the thing you ship for the stock machine.

**Ceiling, stated honestly:** wake-on-clock buys speed by skipping *idle* stretches. A timing-critical
demo that hammers the hardware every cycle leaves few idle stretches, so it gets the *least* speedup —
exactly where observations matter *most*. Also, per-cycle host work (audio at 1 MHz, tape sampling,
firmware taps — `run_frame`, machine.cpp:289–301) must still fire on the right cycles, which bounds how
many cycles can truly be skipped. So: real speedup, but not uniform, and smallest on the hardest
workloads. We do not promise a flat "1600 %."

---

## 6. Correctness: what proves a fast build is still right

Settled (§12, Q2). Two *separate* checks — do not conflate them:

- **Correctness = matches real hardware.** Both the simulator and any fast build must match a
  hardware-behaviour corpus (real-machine captures, demos known to work, and the CPU test suites).
  This is the definition of "right." The **simulator is not automatically right** — see §7.
- **Switch-safety = byte-identical, but only where earned.** We require byte-for-byte sameness between
  two ways of running **only where the fast one is literally the same chip code re-scheduled** (so
  flipping between them mid-session is safe). We do **not** let "must equal the simulator" become the
  definition of correct — otherwise a *more* hardware-accurate fast scheduler would be rejected for
  "disagreeing" with a less-accurate reference. Correctness answers to hardware, not to the reference.

**What "fast" must always reproduce (the observation-level monitors):** the panels we already have —
**CRTC state, CPU state, stack, memory, AY state.** These show program-observable state and must be
correct in every mode. The deep **per-pin-per-cycle-phase scopes** are *observability* and stay
**simulator-only**.

---

## 7. The judge must earn its badge back

The faithful model is the intended oracle, but it is **not yet trustworthy**: it has been *wrong* three
times this month alone (the two RMR2 banking bugs `9ea6a6e`/`e372557`, the mode-0 decode `2313200`).
So, settled:

- Before we lean on the clean-room as ground truth, it must pass the **tests-vs-hardware-specs audit**
  (`beads-mtca`): every `test/hw/*.cpp` assertion checked against `docs/hardware/*.md` and the oracle,
  killing self-consistent-but-wrong tests (the class that hid the mode-0 bug).
- **Legacy (`engine=0`) stays** as the independent whole-machine oracle until the gates pass. The
  reviews were unanimous: because the simulator and the fast build **share all the chip code**, a
  "simulator vs fast" comparison is a *self-consistency* check, not a *correctness* check — legacy is
  the only *independent* witness for the non-CPU chips (GA/CRTC/ASIC; the CPU has FUSE/jsmoo).

Trust is earned by the audit, not assumed.

---

## 8. The keystone tool: a differential harness as CI oracle

The one genuinely great idea from the first draft, correctly scoped by the reviews: not a shipping
dual-mode, but a **test oracle**. It runs two builds from the same seed + same input and flags
divergence, so any speed change that breaks *observations* fails CI the moment it lands. This is what
lets us chase aggressive speed (SIMD, wake-on-clock) **without fear**.

**Cheap tier (works today):** per-frame framebuffer md5 — this is exactly how soldered was validated.
**Deep tier (has a real prerequisite — see below):** full machine-state hash at frame boundaries, to
catch a wrong register *before* it reaches the screen.

**BLOCKER prerequisite (feasibility review, confidence 100):** the deep tier is **not buildable on
today's `save()`**. `z80_save`/`video_save` emit only a version byte (stubs); several devices
`memcpy` raw *pointers* (would false-diverge on frame 0 and violates the contract's "logical state
only" rule, device.h:38); the upper 64 K expansion RAM lives behind a pointer and isn't serialised at
all. So the deep harness needs, first, a **complete, logical, pointer-free `save()`/`load()` across all
18 devices** (or a `hash_state()` built from the existing `*_peek` accessors). This same completeness
gates record/replay and any future "delete legacy" proof. It is folded into the roadmap explicitly.

**Other real prerequisites the reviews surfaced:**
- **No input record/replay exists yet** — the harness needs deterministic "apply event at cycle N."
- **Determinism isn't universal** — `smartwatch`/`symbiface` RTC and `m4` read host time/FS; the harness
  corpus must use deterministic device sets and feed host inputs from a frozen trace.
- **The per-cycle divergence localiser** works only against per-cycle schedulers; the wake-on-clock
  scheduler needs a different classification tool (it has no per-cycle bus by design).

---

## 9. What is actually true in the code today (so we don't plan on fiction)

- **The "two engines" are one engine in two shapes.** `tick_soldered` (machine.cpp:254–277) is
  `board_tick` (board.cpp:40–50) unrolled: same device `tick` pointers, same `bus_resting()`, same
  commit. Good — it means "one body of code, wired two ways" is the *existing* reality, not a new
  invention.
- **But they already diverge on composition.** `board_tick` honours `active_count`; `tick_soldered`
  hardcodes 18 devices. So "flip to fast, nothing changes" holds **only when the device set matches** —
  fast dispatch must be derived from the same active/socketed set (the `hw_reconfigure` SoA idea in
  board-performance.md §4 is the vehicle), not a frozen 18-call list.
- **"Fast compiles out observability" is false today.** The `probe` is ticked inside `tick_soldered`,
  and the **telnet/BDOS taps ride on the probe** (machine.cpp:198–204, 304–309) — a shipping feature.
  So the taps and the observation-level monitors are things fast mode **keeps** (they're observations /
  features); only the deep scopes are the droppable *observability*.
- **The 114 FPS was a monomorphic build** (`SOLDERED=1`, `board_tick` compiled out). The dual-path,
  both-dispatchers-in-one-binary number under PGO is **unmeasured** — measure it before leaning on it.

---

## 10. The roadmap — gated, in the order you set

The sequence you fixed: **reconcile tests → build hybrid + fast → drop legacy → rewrite history.**

**Gate A — The judge earns trust.** (`beads-mtca`)
- A1. Audit every `test/hw` assertion vs `docs/hardware/*.md` + the legacy oracle; kill/repair
  self-consistent-wrong tests.
- A2. Per-chip conformance where an external oracle exists (FUSE/jsmoo for Z80; datasheet vectors for
  GA/CRTC/PSG). *Exit:* the simulator is trustworthy enough to be a reference.

**Gate B — Build the hybrid + fast modes.** (blocks on A for trust, not for code)
- B1. **State completeness**: real logical, pointer-free `save()`/`load()` for all 18 devices (or
  `hash_state()` from `*_peek`). *Prerequisite for B3, and for ever dropping legacy.*
- B2. **Input record/replay**: deterministic trace capture + apply-at-cycle-N.
- B3. **Differential harness as CI**: framebuffer md5 now; full-state hash once B1 lands; wired into
  `test_runner`, gating `src/hw/**` and `src/subcycle/**`.
- B4. **`make pgo`**: productionise the manual 2-phase flow (the `PGO_GEN`/`PGO_USE` scaffold + `%c`
  continuous-mode note already exist; drive it on a fixed headless trace). **Measure dual-path PGO FPS**
  and record it beside the monomorphic 114.
- B5. **Runtime speed selection, thin and composition-aware**: pick build/scheduler tier at runtime,
  derived from the *same* active device set as the simulator (not a frozen list). Menu/hotkey/IPC.
  Default to the fastest that keeps observations; **auto-raise observability when a debug panel/scope is
  opened** (implemented as "attach instruments to the running machine" via the probe-as-Device model —
  not swapping engines).
- B6. **Fast scheduler R&D — wake-on-clock**: make each chip's clock-wake contract explicit; build the
  event scheduler as a *second scheduler over the same chip code*; prove observations-equality against
  the simulator via B3 over the corpus, and argue the sub-cycle-transient cases (wired-OR `irq`,
  floating `data`) per-device from the contract, not just by corpus. Honour the per-cycle host cadence
  (§5 ceiling). *Exit:* a Core 2 Duo hits 50+ FPS on the stock machine with observations intact.
- B7. **SIMD** the hot compositors (`plus_sprite_at`, pixel decode) with a scalar reference as ground
  truth; **idle-device gating** with per-device datasheet-derived wake predicates, harness-proven +
  fuzzed traces (a 5-title corpus won't surface the long tail).

**Gate C — Drop legacy.** *Only after A + B prove the clean-room* across the corpus on full state.
Retire `engine=0`; the simulator becomes the sole model; the differential harness continues as
simulator-vs-fast (now a self-consistency check, which is all it needs to be once correctness is
established vs hardware).

**Gate D — Rewrite the branch's history: a finally-new emulator.** Reshape `feat/sub-cycle-chip-sim`
into a clean narrative — no longer a Caprice32 fork, a new honest emulator. **Constraint (project
rule):** no `git rebase` / `pull --rebase`; do it with `git reset --soft` + recommit or a fresh
orphan-branch import + merge. Update naming/docs/attribution to reflect a new project, not a fork.

Dependency spine: **A (trust) ‖ B1–B4 (harness+infra) → B5 (switch) → B6/B7 (fast) → C (drop legacy) →
D (history).** Deferred to their own tracks, off this critical path: the full observability suite beyond
the 5 baseline monitors (→ `beads-77k3`), and SPARK devices (→ the foundation plan's own SPARK step,
after C).

---

## 11. Risks & open questions (folded from the review)

| # | Risk / question | Handling |
|---|---|---|
| 1 | `save()` is stubbed/pointer-dumping — deep harness unbuildable | B1 makes complete logical save() a hard prerequisite |
| 2 | Simulator≡fast tests dispatch, not correctness | Correctness is vs hardware (A); keep legacy (§7) until gates pass |
| 3 | Switch/soldered diverge on composition (`active_count`) | Fast dispatch derived from the same active set (B5), not a frozen list |
| 4 | Wake-on-clock may not preserve observations for all software | Prove per-device at contract level + corpus + fuzz (B6); it's optional — soldered+PGO+SIMD is the provable default |
| 5 | 114 FPS unmeasured on the shipping (dual-path) binary | Measure in B4 before relying on it |
| 6 | Determinism not universal (RTC/M4/FS) | Harness scoped to deterministic device sets; host inputs frozen in traces |
| 7 | Weak-hardware target may still miss 50 FPS after all levers | The gate is the *measurement* on real weak hardware, not this doc's optimism |
| Open | Does soldered become canonical for the stock CPC, or stay a build option? | Decide after B4's dual-path measurement |
| Open | A non-exact "turbo" tier at all? | Default: **no** — it would break observations. Only revisit if a class of users wants speed over exactness. |

---

## 12. Decisions log (settled interactively, 2026-07-05)

- **Q1 — Architecture:** *Hybrid.* One engine / three axes as the spine; a runtime speed switch kept
  thin and added where measurement (on weak hardware) proves it's needed — not a speculative permanent
  dual-mode.
- **Q2 — Correctness:** *Match real hardware; byte-identity only for switch-safety* where fast is a pure
  re-scheduling of the same chip code. Identity-to-simulator is **not** the definition of correct.
- **Q3 — Timing model:** *The every-tick loop stays as the reference simulator.* Wake-on-clock (or any
  faster method) is the speedup; it must reproduce observations (incl. the CRTC/CPU/stack/memory/AY
  monitors); internals are free. Prefer one code body wired two ways; plan the split where impossible.
- **Q4 — Legacy:** *Keep* as the independent oracle until Gates A+B pass; drop only at Gate C.
- **Sequence:** reconcile tests (mtca) → build hybrid + fast → drop legacy → rewrite branch history.
- **Perf target:** 50+ FPS on **Core 2 Duo / Atom** for the stock CPC — not "≥50 on the dev machine."

## 13. Appendix — substrate map & numbers

- Contract: `src/hw/device.h` `Device{self,name,tick,reset,state_size,save,load}`.
- Bus: `src/hw/buses.h` (`Bus` ~48 B, resting/floating/wired-OR; `Clocks{cpu,crtc,psg,phase}` — the
  clock-wake fabric §5 needs).
- Scheduler: `src/hw/board.cpp` `board_tick` (honours `active_count`); `src/subcycle/machine.cpp`
  `tick_soldered` (fixed 18), `run_frame` (per-cycle host work at :289–301), probe/taps at :198–204,
  :304–309.
- Observability seed: `src/hw/probe.h` (ICE bus probe; telnet/BDOS taps ride it).
- Stubs to fix (B1): `z80_save`/`z80_load` (z80.cpp:2316–2321), `video_save`/`video_load`
  (video.cpp:365); pointer-dumping `*_save` in memory/ppi/asic/fdc; expansion RAM behind `mem_state.expansion`.
- Perf (M2 Ultra): faithful `-O2` ~40 FPS; +LTO +6 %; soldered+LTO ~67; soldered+PGO ~114 (monomorphic);
  pluggable+PGO ~63. **All need re-measuring on weak hardware — that's the real gate.**
- Monitors baseline that fast must keep: CRTC, CPU, stack, memory, AY. Deep pin scopes: simulator-only.

## 14. Decisions — session addendum (2026-07-05, second round)

- **Q5 — Turbo tier:** *Allowed* as an opt-in, clearly-labelled **non-exact** max-speed tier
  (frame-boundary switch only) — the one mode permitted to mis-time the pickiest demos; every other
  mode stays observation-exact.
- **Q6 — Default engine:** **Soldered is the default** for the stock CPC (pending the B4 dual-path
  FPS measurement); the pluggable build is for custom machines (SID-for-AY) + deep debugging.
- **Q7 — Switch granularity:** **Frame-boundary only.** Cycle-stepping lives in faithful mode.
- **Q8 — Launch / switch UX:** Start in **Fast**. Switching is **manual via a submenu**, *or* via a
  **prompt** when the user opens a hardware-pin-level debug view ("this view needs faithful mode —
  switch?") — **never a silent auto-switch** (refines §5.3).
- **Observed (`beads-gbey`):** the clean-room engine=1 cartridge boot is currently **unreliable**
  (intermittent `PC=0x0001` hang / blue-screen-nothing); legacy engine=0 boots reliably. Top
  clean-room reliability bug; gates Gate C (drop legacy).
