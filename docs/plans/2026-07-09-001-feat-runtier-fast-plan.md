# RunTier::Fast — legacy-class speed from clean-room code

**Status**: planned · **Epic**: beads (see `bd show` for the epic created with this doc)
**Prerequisite for**: Gate C (legacy deletion) — *Gate C MUST NOT proceed until this
tier exists and measurably beats the legacy engine* (user decision, 2026-07-09).

---

## 1. Mission

Build the third runtime tier of the sub-cycle Machine:

| Tier | Granularity | Purpose | Speed target |
|---|---|---|---|
| `RunTier::Faithful` | per 16 MHz master cycle | pin-level observability, any composition | 1× (baseline) |
| `RunTier::Wake` | per master cycle, contract-scheduled | **the shipping default** | ~2.4× Faithful |
| **`RunTier::Fast`** | **per Z80 instruction, catch-up scheduled** | legacy-replacement speed | **beat the legacy engine** |

Constraints, verbatim from the project owner:
- *fresh OOP code, clean patterns, composability, no globals, zero copied code*;
- dynamically switchable at frame boundaries, like the existing tiers;
- all tiers PGO-optimized (`make pgo` + the GUI recipe in the makefile already
  train the shipping configuration — extend, don't fork).

"Zero copied code" means: the legacy engine (`src/z80.cpp`, `src/crtc.cpp`,
`src/asic.cpp`, …) is a **behavioral oracle to read, never a source to paste**.
Hardware constant tables (cycle counts, palette values, register masks) are
facts about the silicon, not code — and clean-room versions already exist in
`src/hw/`. Reuse `src/hw/` freely: it is ours, spec-first, and FUSE-validated.

## 2. The bar: what "beat Legacy" means, in numbers

All numbers measured 2026-07-09 at `26c10dd`/`eb4e675`, M2, same-harness method
(§8.3). These are the targets to EXCEED (strictly greater, interleaved A/B):

### Legacy engine (`engine=0`), headless GUI, limiter off, --fps averages

| Workload / core | no-PGO | **PGO (combined profile) — THE BAR** |
|---|---|---|
| 6128, P-core | 1911 | **2554 FPS** |
| 6128, E-core | 562 | **619 FPS** |
| Plus (system.cpr), P-core | 1626 | **2016 FPS** |
| Plus, E-core | 495 | **547 FPS** |

### Reference: the sub-cycle wake tier on the SAME harness

| Workload / core | no-PGO | engine=1-trained PGO |
|---|---|---|
| 6128, P-core | 108 | 145 |
| 6128, E-core | 38 | 46 |
| Plus, P-core | 79 | 108 |
| Plus, E-core | 27 | 33 |

So Fast must deliver **~17–20× over Wake**. That is not achievable by scheduling
tricks over per-cycle dispatch — the wake-tier ledger (beads-708f) proved the
per-cycle floor is real silicon work (three negative results with mechanisms:
SIMD compositor, dirty-flag caching, Z80 stall-skip). Fast is a **different
execution granularity**, not a faster loop.

Pure-bench references (koncepcja_bench, CKSUM-verified): Faithful ~70 FPS,
Wake ~173 no-PGO / ~208-233 PGO (6128, matched `--frames 1000`,
CKSUM `67f98743a6105d08`; Plus `85f12f9018ade568`; **CKSUMs are
frame-count-dependent — always compare at matched `--frames`**; 400f values:
`7d8072bef3c0c790` / `4be0fe3b1a589ed0`).

## 3. Why the legacy engine is fast (read `src/z80.cpp` & friends as an oracle)

1. **Instruction granularity**: one dispatch per Z80 instruction, not 4–16+
   calls per µs. Cycle cost comes from the `cc_op` tables (CPC-µs-quantized
   per-instruction counts — the same grid our per-cycle core reproduces and our
   `z80_ccop_sweep_test` locks).
2. **Direct memory access**: reads/writes go through bank pointer tables
   (`membank_read[addr >> 14][addr & 0x3FFF]`) — no bus, no handshake.
3. **Lazy devices**: CRTC/GA advance in character/scanline steps inside the
   emulation loop; PSG renders in per-sample batches; the FDC uses "fast
   timing" (immediate per-access handshakes); nothing runs per master cycle.
4. **Event-driven interrupts**: the 52-HSYNC counter is maintained at scanline
   granularity, so the CPU core rarely checks anything.
5. Its *costs*: globals everywhere, copied lineage code, zero composability,
   hidden couplings (see the BOARD_MAX_DEVICES incident, beads memory), and it
   is untestable at the pin level. That is precisely what Fast must NOT be.

## 4. Architecture: catch-up scheduling over the SAME devices

The classic timestamped-component design (MAME's device scheduler / Ares
lineage), instantiated on our clean Devices. **The devices' STATE STRUCTS,
save/load, peek surfaces and decode helpers are shared with the per-cycle
tiers** — Fast adds *batch execution contracts*, it does not fork chips.

### 4.1 Timeline
- One master clock in **16 MHz master cycles** — the same unit as
  Faithful/Wake. Non-negotiable: it is what makes tier-switching trivial
  (same state, same clock) and equivalence testable.
- Each device keeps `synced_until` (master-cycle timestamp).

### 4.2 The driver: Z80 in instruction mode (F2)
- Reuse the FUSE-proven core in `src/hw/z80.cpp` by giving it a **batch mode**:
  run the existing micro-op sequences without returning to the bus between
  T-states. Memory M-cycles call a mem seam directly; I/O M-cycles trigger
  device sync (catch-up) then apply. Do NOT write a second interpreter first —
  dual-moding the validated core is the low-risk path; a bespoke interpreter
  is a later optimization if F8 falls short.
- Per-instruction time advance must equal the per-cycle core's on the CPC grid
  (µs-quantized M-cycles, I/O TW stretch — see the quantiser comment in
  `z80_tick` and `z80_ccop_sweep_test.cpp`). **Oracle: extend the ccop sweep to
  run both modes and diff every opcode's tstates.** This is the single most
  important correctness gate of the whole epic.
- Interrupt delivery: compute `next_irq_at` (GA 52-HSYNC counter + CRTC
  geometry; ASIC PRI on Plus); run instructions freely until
  `min(next_irq_at, next_device_event, frame_end)`; deliver at instruction
  boundaries honoring EI-delay/HALT semantics exactly as the per-cycle core
  (same micro-ops!).

### 4.3 Memory seam (F3)
- `mem_fast_read(state, addr)` / `mem_fast_write(state, addr, val)`: direct
  banked access honoring ROM enables, 6128 banking, cartridge banks, ASIC page
  overlay + /RAMDIS veto. **Share the decode helpers with `mem_tick`** (the
  `ga_clock_out` single-definition rule — one banking truth). Bank-pointer
  tables recomputed on banking writes, not per access.

### 4.4 Video: the catch-up renderer (F5)
- `video_catch_up(state, until_cycle)`: render CRTC characters from
  `synced_until` to `until`, batched per scanline, using the ALREADY-BUILT and
  byte-identity-proven `vid_render_line` / `vid_decode_lut` (`src/hw/video.h`).
- **Every GA/CRTC/ASIC register write catches video up first, then applies.**
  This is how µs-level raster tricks (mode splits, ink flips mid-frame, RUPTURE
  demos) stay exact at instruction granularity — the same catch-up discipline
  legacy uses. Scanline-lazy is NOT enough; catch-up-on-write is the invariant.
- Plus compositor: **sprite-major per scanline with per-line active lists**
  (the hvgg lesson: legacy `asic_draw_sprites` is sprite-major and culled;
  pixel-major × 16 sprites was our measured mistake, and SIMD there was 28%
  slower — do not re-litigate, see beads-hvgg notes).

### 4.5 CRTC/GA scanline engine (F4)
- `crtc_advance_chars(state, n, events*)`: advance whole characters, emitting
  timestamped HSYNC/VSYNC/DISPEN edges + frame_line. GA consumes edges to run
  its raster counter (`ga_raster_count` is already a pure helper) and to
  compute `next_irq_at`. Mode latching at HSYNC per `gate-array-device.md`.
- R7-write-VSYNC-start and friends: handled by catch-up-then-apply on the I/O
  write (same rule as video).

### 4.6 PSG audio (F6)
- `psg_render(state, n_psg_cycles, sink)`: batch `sound_step` (tone/noise/
  envelope counters are simple dividers — tight loop first, closed forms only
  if profiling demands). AY bus writes: catch-up then apply-once. **Beware the
  bestiary idempotency exception**: a reg-13 write RESTARTS the envelope; the
  per-cycle semantics re-restart it every held cycle — define batch semantics
  as "restart once per write event" and VERIFY against Faithful with the audio
  byte-compare oracle (the per-cycle "restart per held cycle" is a spec
  question worth settling against real-hardware refs; whichever way, the
  oracle decides equality or a documented, spec'd divergence).
- Audio equality is byte-compare per frame (the Plus lockstep pattern).

### 4.7 FDC / tape / printer (F6)
- Extend the shipped contracts (`fdc_quiet`/`fdc_advance`,
  `printer_advance`, tape counters) into full event-horizon execution: the
  deadline set is already enumerated in `fdc_tick` (`rot` wrap → INDEX event,
  `motor_ready_at`, `next_step_at[u]`, `due_at`, `next_byte_at`). Every
  deadline is exact math on `now` — batchable precisely, events timestamped.
- Every I/O access to their ports: catch-up then apply (the per-access
  handshake is then identical to legacy's "fast timing", which is itself the
  documented model — `fdc-device.md`).

### 4.8 Plus ASIC (F7)
- PRI + split: per-scanline (frame_line events from the CRTC engine).
- DMA sound: per-HSYNC burst execution exactly as `asic-device.md` documents
  (legacy runs it per-HSYNC too); PSG writes go through the same
  catch-up-then-apply path.
- Register page: mem seam honors the overlay + /RAMDIS veto (§4.3); the
  RMR2/knock decodes are I/O-write events.

### 4.9 Tier plumbing (F1, F9)
- Enum: `RunTier { Faithful, Soldered, Wake, Fast }` — the current
  `RunTier::Fast` (tick_soldered) is RENAMED to `Soldered` and **stays
  switchable** (user decision); `Fast` is reserved for this tier. Fix
  `effective_run_tier`, the harness test names, and the B5 IPC strings when
  renaming.
- Switch at frame boundaries only (existing pattern). State transfer is
  free — same device structs. The one rule: entering Fast requires devices
  synced (they are, at a frame boundary); leaving Fast requires all
  `synced_until == now` (run_frame ends with a full catch-up).
- Degradation: Fast valid only for the canonical composition in v1 (same
  gate style as `wake_valid_`); falls back to Wake.
- `5vom` (menu/hotkey/IPC tier surface) lands here.

## 5. The equivalence contract (what the oracles enforce)

Fast cannot match per-cycle *micro*-state (there is no `t`/`mc` mid-instruction
notion). Define equality at **frame boundaries**:

1. Framebuffer: byte-identical (bench CKSUM at matched `--frames` == canonical).
2. Audio: byte-identical stream per frame (the Plus-lockstep audio pattern; add
   a 6128 audio lockstep too).
3. Z80 architectural state: `Z80Regs` equal (peek surface).
4. Every device's PEEK surface equal; RAM + expansion RAM equal.
5. Instruction timing: ccop dual-mode sweep — per-instruction tstates equal on
   the CPC grid, all opcodes, all addressing modes.
6. Scenario lockstep: the Plus boot→menu→F2→title keyboard script frame-for-
   frame (fb+peek+audio), and a 6128 typing scenario (`SubcycleMachine.
   BootsTypesAndSounds` pattern).

The per-device save-blob hash comparison (diff_harness) may include micro-state
fields that legitimately differ mid-paradigm; where it does, add a **logical
comparator per device** (peek-based) and document each excluded field with the
reason. No silent exclusions.

## 6. Phases → child beads (each exits on oracles, not vibes)

- **F0 — Batch-contract specs**: add a "batch contract" section to each
  `docs/hardware/*-device.md` (what the device does per event vs per cycle;
  its deadline set; its idempotency exceptions). Spec-first doctrine: write
  these BEFORE the code, review against the legacy oracle's behavior.
- **F1 — Timeline skeleton + tier plumbing**: RunTier rename (Soldered) +
  reserve Fast; `run_frame_fast()` scaffold with per-device `synced_until`,
  horizon calculator, full-catch-up frame exit; KONCPC_FAST bisect mask
  (per-device "batch vs per-cycle-fallback" — day one, non-negotiable; it is
  how every wake bug was found in one run).
- **F2 — Z80 instruction mode**: dual-mode the core; mem/IO seams. Exit: FUSE
  1356/1356 in batch mode; ccop dual-mode sweep zero-diff; boots BASIC via F3.
- **F3 — Memory seam**: shared-decode direct access + bank tables. Exit: RAM
  equality on boot scenarios; banking unit tests green in both modes.
- **F4 — CRTC/GA scanline engine + IRQ prediction**. Exit: frame cadence equal
  (frames counter, vsync times), interrupt delivery times equal on the ccop
  grid (test: irq-latency scenario vs Faithful).
- **F5 — Catch-up video renderer**. Exit: 6128 bench CKSUM canonical at
  matched frames; raster-trick unit scenarios (mid-frame mode/ink/R7 writes)
  pixel-equal vs Faithful.
- **F6 — PSG batch + FDC/tape/printer event-horizon**. Exit: full 6128 suite
  green under Fast; 6128 audio lockstep; disc boot scenario (AMSDOS + a DSK)
  equal. First FPS milestone measured & recorded.
- **F7 — Plus/ASIC**. Exit: Plus lockstep (fb+peek+audio) green; Plus bench
  CKSUM canonical; the Burnin derail regression suite green under Fast.
- **F8 — Performance war**: profile→fix loop (sample recipe §8.4), PGO train
  includes Fast, interleaved A/B vs legacy. Exit: **all four cells beat §2's
  PGO bar**. Revert discipline applies (slower experiment = revert + record).
- **F9 — Integration**: 5vom tier surface; snapshot + tier-switch tests; docs;
  GUI defaults discussion (Wake vs Fast as default is a USER decision to ask,
  not assume). Exit: Gate C formally unblocked.

Dependencies: F0→F1→F2→F3→F4→F5→F6→F7→F8→F9, with F0 parallelizable per device.

## 7. Best practices (hard-won this cycle — do not relearn them)

1. **Oracle-first**: no batch code lands without its comparison test running.
   The CKSUM bench + lockstep + per-device state naming caught every single
   bug this cycle, usually on the first run.
2. **The bug bestiary** (memory: `wake-scheduler-contracts.md`) applies to
   batch contracts verbatim: (a) drive-then-latch pipelines (PPI publish, mem
   write-latch under /RAMDIS); (b) timestamped counters need exact `*_advance`
   catch-up; (c) idempotency exceptions (AY reg-13 env_restart!); (d) shared
   lines key on the DRIVER. Audit every device's batch contract against all
   four, in the F0 spec.
3. **Single-definition rule**: batch and per-cycle paths share helpers
   (`ga_clock_out`, `vid_render_line`, banking decode). A verbatim copy WILL
   drift; the harness will catch it late and expensively.
4. **Bisect masks from day one** (KONCPC_FAST per-device fallback): a
   divergence then names its device in one run.
5. **Measurement discipline**: interleaved A/B only (single-run FPS swings
   ±15% with thermals); matched `--frames` for CKSUMs; `taskpolicy -b` for
   E-core; PGO per §8.2 before quoting final numbers.
6. **Revert discipline**: an optimization that measures slower is reverted the
   same session and its mechanism recorded on the bead (precedents: SIMD
   compositor, dirty-flag, Z80 stall-skip — all in beads-708f/hvgg notes).
7. **Repo conventions**: branch off, PR to `master`, never push master, no
   rebase (merge-based), no AI attribution lines; MINGW CI (no `strncpy`,
   winsock guards, file-lock TearDown); clang-tidy naming (≥3-char names);
   spec comments in the house style (reference `docs/hardware/*` sections).
8. **Consult, don't copy, legacy**: when behavior is unclear, read the legacy
   implementation AND the real-hardware references
   (`/Users/ikari/src/cpc/gem-knight/references/`, cpcwiki) — the references
   are the authority, legacy is a hint.

## 8. Tools & recipes

### 8.1 Bench
`koncepcja_bench` — build line (zsh; note `${=SRCS}`):
```sh
SRCS="sim/bench_fps.cpp src/hw/*.cpp(-ordered list in makefile/bench docs) src/subcycle/machine.cpp src/subcycle/record_replay.cpp"
c++ -std=c++17 -O2 -funroll-loops -ffast-math -fomit-frame-pointer -finline-functions -flto -Isrc -o koncepcja_bench ${=SRCS}
./koncepcja_bench --frames 1000 --quiet          # 6128; CKSUM must be 67f98743a6105d08
./koncepcja_bench --cpr rom/system.cpr --frames 1000 --quiet   # Plus; 85f12f9018ade568
```
Add `--tier=fast|wake|faithful` to bench when F1 lands (today: env-driven).

### 8.2 PGO
- Bench: `make pgo` (trains the shipping dispatch, both workloads; extend
  `PGO_WAKE`-style knob for Fast).
- GUI: recipe in the makefile (~line 256): `PGO_GEN=1` build → train with
  `LLVM_PROFILE_FILE="kon-%p-%c.profraw"` (the `%c` is REQUIRED — `_exit`
  skips atexit flushing) on engine=1 both workloads → merge → `PGO_USE=`.

### 8.3 Same-harness FPS protocol (the §2 numbers)
```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy [taskpolicy -b] ./koncepcja --fps \
  -O system.limit_speed=0 [-O system.engine=1] [-O system.model=3 rom/system.cpr]
```
Average ≥12 once-per-second `[fps]` samples after warm-up; the FPS field is
space-padded (`[fps]  37 FPS`) — extraction regex must allow multiple spaces.
Kill by PID only (never by port).

### 8.4 Profiler
Build `-O2 -g -fno-omit-frame-pointer` (NO LTO, keeps symbols), run under
load, `sample <pid> 10 -file out.txt`, count symbol occurrences. Shares are
indicative only — LTO changes the landscape; confirm with A/B.

### 8.5 Oracles inventory
`DifferentialHarness.*` (fb + per-device state hashes with culprit naming),
`PlusCartBoot.WakeTierMatchesFaithfulIncludingAudio` (lockstep incl. audio —
clone for Fast), `z80_fuse_test` (1356 cases), `z80_ccop_sweep_test` (CPC
timing grid), bench CKSUMs, `test/integrated/ipc_harness.py`.

## 9. Risks & mitigations

- **Sub-µs raster tricks**: catch-up-on-I/O-write is the answer (§4.4); build
  the raster-trick scenario tests in F5 BEFORE optimizing.
- **Interrupt-timing drift**: the ccop dual-mode sweep + an IRQ-latency
  scenario test in F4; the per-cycle core is the reference, always.
- **Batch semantics of held-strobe re-application** (reg-13 class): settle in
  F0 per device against hardware references; oracle-verify.
- **"Beat legacy" shortfall at F8**: profile-driven iteration; then a bespoke
  Z80 interpreter (replacing the dual-mode) is the escalation, still clean-room.
- **Timeline**: this is a Gate-B-scale epic (weeks). Gate C waits (decided);
  the wake tier keeps users fast-enough meanwhile.

## 10. Where everything lives

- Wake-tier ledger + negative results: `bd show` beads-708f, beads-hvgg notes.
- Bug bestiary + measurement lessons: memory `wake-scheduler-contracts.md`.
- Baseline numbers: §2 above + beads-4q7r note (2026-07-09).
- Specs: `docs/hardware/*.md`; hw plan: `docs/hw-spec.md`.
- Real-hardware references: `/Users/ikari/src/cpc/gem-knight/references/`.
