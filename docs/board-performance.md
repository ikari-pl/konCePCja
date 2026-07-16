# Board performance & the parallel-device model — design note

Status: **design / thought experiment** (no code yet — consult-before-optimising,
per project rule). Companion to `docs/hw-spec.md` §7 (the `board_tick` scheduler)
and `src/hw/board.{h,cpp}`. Tracking task: `beads-fc0b`.

## 1. The measurement (why this note exists)

GUI validation on 2026-07-04 (Apple M2 Ultra, real Metal present path — **not**
headless), uncapped (`--fps -O system.limit_speed=0 -O video.vsync=0`):

| Config (`engine=1`) | uncapped FPS | speed | render-wait |
|---|---|---|---|
| 6128 (model 2) | ~41 | ~82 % | 0 % |
| Plus (model 3) | ~38–39 | ~77 % | 0 % |

The engine is **compute-bound** (render-wait 0 % → *not* the present path), and it
can't sustain 50 FPS even uncapped. It is **not** the Plus renderer (only a ~5 %
delta) and **not** `z80_tick` (121 samples). A macOS `sample` profile puts the
cost squarely in `board_tick`'s own time (~1356/2698 emulation-thread samples;
`z80_tick` is a small child; the main-thread `render_one_frame` runs in parallel).

Grounding facts: build is `-O2 -funroll-loops -finline-functions` with **no
`-flto`**; **no per-tick heap traffic** (all `new` is placement-new in `*_init` —
the caller-owned-storage doctrine already bought that); the `Bus` is passed **by
pointer** (calling convention already lean).

Per master cycle (16 MHz → ~320 k/frame), `board_tick` does:

```
Bus next = bus_resting();                 // ~48-byte zero-init + 3 stores
for each of ~18 devices: dev->tick(...);  // indirect fn-pointer dispatch
board->bus = next;                        // ~48-byte copy
```

≈ **288 M indirect calls/sec + ~1.5 GB/sec of Bus-struct copies.**

## 2. The deep framing: devices are parallel; the bus only synchronises them

Real chips free-run, coupled *only* through the bus and their clocks. Our
two-phase commit already encodes this: within a cycle every device reads the same
committed `in` and writes its own `next` — **order-independent by construction**
(hw-spec §7). This is the actor / synchronous-dataflow model: devices = actors,
the bus = the channel, `board_tick` = one dataflow step.

The cost is **granularity**, not the model. We impose a global barrier every
62 ns. Two consequences:

- Threading across that barrier is hopeless (sync ≫ the nanoseconds of work), so
  the parallelism does **not** cash out as threads.
- Because `next` **resets to resting every cycle**, every *driving* device must
  **re-assert its lines every cycle** — the Z80 re-drives its held address/`mreq`
  16 M×/sec though they're constant between T-states. We recompute steady state
  ~4× more often than it changes.

That second point is the real lever, and it is **doctrine-aligned**: real hardware
is **clock/event-driven**, not polled at 16 MHz. A model where devices wake on
their own clock edges (Z80 4 MHz, CRTC/PSG 1 MHz) and the bus *persists* levels
between changes is both faster **and** more hardware-faithful. The tension: the
resting/floating/wired-OR bus *is* doctrine (pull-ups, single-driver truth), and
"levels persist" changes that semantics — so this is a **reference-architecture**
question, not a patch.

## 3. Levers, ranked by doctrine fit

**Free / doctrine-neutral — do first:**

- **`kResting` memcpy.** Replace `Bus next = bus_resting()` (a `Bus{}` zero-init +
  3 stores + return) with a copy from a `static const Bus kResting`. One memcpy,
  same values. Zero semantic change.
- **SoA device arrays, *inferred* on reconfigure** (see §4) — flat `tick[]` /
  `self[]` walked per tick instead of loading a ~56-byte `Device` (self + 5
  fn-pointers + `name`) per iteration. Better prefetch; internal to the board.
- **`-flto` (+ PGO).** A *build-flag* experiment — no code, no semantics. Lets the
  compiler optimise across TUs and *speculatively devirtualise* hot fn-pointer
  targets it can prove; PGO (we already have a profile) predicts/specialises the
  indirect branches. Highest-ROI, lowest-risk thing to *try*.

**Doctrine-aligned but architectural (needs a spec + fidelity proof):**

- **Clock-domain gating + level persistence.** A Device *declares* its wake clock
  (a `clock_mask` — a real hardware property, not internal logic leaking), and the
  board skips the call when that clock is low. Cuts most of the 288 M calls/sec
  (Z80 works 4/16, CRTC/PSG 1/16). Catch: "idle" ticks currently re-drive held
  lines, so skipping the call requires the bus to **hold** levels (or the board to
  cache last-driven output) — it only fully lands *together with* level
  persistence (§2). This is the deep win; it makes the sim more event-driven, i.e.
  truer to hardware.

**Doctrine tension — avoid:**

- **Pack the `Bus` bools into bitfields** (3× smaller copies). `buses.h` promises
  *"a SPARK/Ada or Rust device maps these structs 1:1 … plain C fixed-width
  fields."* Bitfield layout is implementation-defined in C and maps differently to
  Ada/Rust — this erodes the **language-neutral** contract for cycles.
- **Direct-call / de-virtualise the core devices** (to enable inlining). The
  uniform `Device{tick,…}` fn-pointer dispatch *is* the pluggable, order-free,
  language-neutral contract. Hardcoding breaks it.

**Through-line:** the wins that pay off are the ones that make the model *truer to
the hardware's real concurrency and clocking*; the shortcut was the
poll-everyone-at-16-MHz loop, not the fidelity. Speed and fidelity aren't opposed
here.

## 4. Decision: first step

Endorsed direction — the two doctrine-neutral wins, with the SoA arrays **inferred
from the device list on a `hw_reconfigure()` call** (not hand-maintained):

1. **`kResting` memcpy** in `board_tick` / `board_reset`.
2. **SoA execution arrays as derived state.** The board keeps the `dev[]` list as
   the **source of truth** (save/load, peek, order-independence proofs). A
   `hw_reconfigure(Board*)` (re)builds tight parallel arrays — `tick_fns[]`,
   `selves[]` — from `dev[]`. `board_tick` iterates *those*.

`hw_reconfigure()` is called whenever the machine's **composition** changes, which
is rare:
- once after the `board_add` sequence in `machine.build()`;
- when a peripheral is plugged/unplugged at runtime (which changes what's fitted).

So the flat arrays are always consistent with the device list, and the per-tick
path never touches the `Device` structs or rebuilds anything. It also sets up the
future step cleanly: `hw_reconfigure()` is the natural place to *also* bucket
devices by clock domain (e.g. `cpu_domain[]`, `crtc_domain[]`, `always[]`) once
level-persistence (§2) makes clock-gating safe — the reconfigure hook infers the
schedule; `board_tick` just runs it.

### Doctrine check for the first step

- Order-independence: **preserved** — SoA is the same set of callees in the same
  order; only the storage layout changes.
- Device isolation & contract: **preserved** — devices are untouched; the board's
  internal execution representation is an implementation detail behind
  `board_tick`.
- Language-neutral 1:1 structs: **untouched** — the `Bus` and `Device` shapes
  don't change; SoA/`kResting` are board-internal.
- Pin-level truth: **untouched** — same values on the bus every cycle.

### Verification plan (per the measure-every-change rule)

Build, run the full suite (order-independence + all 1,395 tests must stay green),
then re-measure uncapped with `--fps` (6128 and Plus, `engine=1`) and record the
before/after here. No change ships without a green suite *and* a measured delta.

## 5. Measured results (2026-07-04) — and the SOLDERED finding

Every change measured **two deltas**: headless sim (`koncepcja_sim_headless`,
1000 frames, ±0.4% variance) *and* the real GUI (`--fps`, uncapped, 6128,
`engine=1`). Baselines: headless 21.6 ms/frame (93%); GUI 41 FPS (82%).

| Change | headless | GUI | verdict |
|---|---|---|---|
| **LTO** (`-flto`, macOS) | 21.59→21.14s (+2%) | 41→44 FPS (82→88%) | **landed** (build flag, suite green, ~24% smaller binary) |
| **`__restrict`** (impl-only, 18 ticks) | 21.14→20.60s (+2.6%) | 44→45–46 FPS (88→90–92%) | **landed** — *shipping path*, md5-identical, typedef untouched |
| **PGO** (profile-guided) | 20.60→**14.30s** (+31%) | 45→**63–64 FPS (126–128%)** | **the answer** — *pluggable path*, md5-identical, needs build infra (beads) |
| kResting + SoA + reorder | ~0 (sub-noise) | — | dropped (small states already L1-resident; layout wasn't the cost) |
| device-state arena | *predicted ~0* | — | not pursued (small states fit L1; the 128 K RAM working set dominates d-cache and is inherent) |
| **SOLDERED** (direct dispatch + LTO) | 21.14→**13.87s** | 44→**67 FPS (134%)** | **1.52× — opt-in flag** |
| **SOLDERED + PGO** | 13.87→**6.36s** | 67→**114 FPS (228%)** | **the ceiling** — opt-in; ~3.4× vs baseline, md5-identical |

**The headline:** the pluggable fn-pointer array dispatch costs **~1/3 of
throughput**. `board_tick` calls `board->dev[i].tick` (a pointer set at runtime by
`board_add`), so the compiler can't prove the target and each tick stays an opaque
indirect call — no inlining. Replacing it (`SOLDERED`, `machine.cpp::tick_soldered`)
with an unrolled fixed sequence of the devices' own tick pointers lets LTO
devirtualize `gdev_.tick → ga_tick`, **inline** each tick, keep the shared `next`
in registers, and DCE the resting-bus writes each device doesn't touch. Both
harnesses agree: **1.52×**, GUI now **134% of realtime**. Behaviour-correct:
pixel-identical output (md5) vs the array path.

The `next` object is shared by all devices intentionally (two-phase, §hw-spec 7);
inlining *helps* because the compiler can then see `next` (a local) and `in`
(`&board_.bus`) provably don't alias — the opaque array call can't.

### The trade — why SOLDERED is opt-in, not default

Soldering is the **inverse of the pluggable Device contract**. The fn-pointer seam
*is* the language-neutral replaceability boundary: a device compiled separately
(C, or a foreign-language Rust/Ada implementation exporting the same ABI) drops in
via its pointer and relinks — no board rebuild. Inlining **dissolves that seam**:
an inlined tick must be a C++ TU visible to LTO, so a soldered device can no longer
be swapped independently (or in another language), and the device *list itself* is
baked into `tick_soldered` at compile time (no runtime composition / hot-plug).

Granularity note: it degrades per-device, not all-or-nothing — a foreign device's
pointer simply stays an un-inlined call inside `tick_soldered`. So a device is
either **soldered** (inlined, C++, fixed, fast) or **socketed** (fn-pointer,
swappable, any language). Default build keeps the whole board socketed (the
doctrine-compliant, replaceable, order-free contract); `make SOLDERED=1` solders it
for max throughput.

### Doctrine-aligned future direction

A real CPC's **core chips are literally soldered**; only the **expansion port** is
pluggable. So the faithful *and* fast design is a **hybrid**: an inlined fast path
for the fixed core (Z80/GA/CRTC/PPI/PSG/RAM) + the pluggable array for hot-plugged
peripherals — selected by `hw_reconfigure()` when the config matches, verified
pixel-equal against `board_tick`, with a `static_assert`/runtime guard so the
soldered device set can't silently drift from the board's. Same recurring theme:
the faster path is the more hardware-honest one. Tracked in `beads-fc0b`.

## 6. Conclusion — two tiers: doctrine-clean PGO, and the SOLDERED+PGO ceiling

**Correction (measured):** an earlier draft here called SOLDERED "redundant"
once PGO landed. That was wrong — PGO and SOLDERED **compound**. On the pluggable
path PGO spends its budget on indirect-call promotion; on SOLDERED the calls are
already direct, so PGO spends it all hot/cold-splitting the inlined ticks along
the profiled idle/active cadence — *automatically building the clock-gated fast
path* the manual phase-unroll would have (correctly, no held-line wall). Result:
**SOLDERED+PGO = 6.36s / 114 FPS (228%)**, md5-identical — ~1.8× beyond
pluggable PGO, ~3.4× over baseline.

So there are two tiers:

- **Doctrine-clean (default, pluggable):** LTO + `__restrict` shipping today
  (45–46 FPS); **+ PGO → 63–64 FPS (127%)**. Fully replaceable, language-neutral,
  order-free — and already clears 50 FPS realtime with margin. The right default.
- **Opt-in ceiling:** **SOLDERED + PGO → 114 FPS (228%)**. ~1.8× more, at the cost
  of the pluggable Device contract (fixed device list, no cross-language swap, no
  runtime hot-plug). Worth it only when the headroom is needed — a heavier config
  (Plus + full peripheral stack, higher-res, faster CPU modes) or a much slower
  host where pluggable-PGO would dip below realtime.

Recommended path forward:

- **Ship LTO + `__restrict`** (done — doctrine-clean, +~12% GUI, no infra).
- **Land PGO** as the primary win (build infra: a `make pgo` two-phase target —
  instrument → run a representative workload → `llvm-profdata merge` → rebuild
  with `-fprofile-use` — plus a stored/regenerated `.profdata` and CI story).
  Tracked in `beads-2sy1`. Applies to **both** tiers.
- **SOLDERED** stays the committed opt-in ceiling — not "redundant", it's the
  max-throughput build for when the doctrine price is acceptable.

The recurring lesson held to the end: the biggest wins (LTO, PGO, their combo)
changed *no code and no semantics* — the pin-level model was never the problem;
teaching the compiler about it was.
