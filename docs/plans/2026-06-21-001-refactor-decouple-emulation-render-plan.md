---
title: "refactor: Decouple emulation from render (triple-buffer) + live display under native menus"
type: refactor
status: active
date: 2026-06-21
depth: deep
branch_base: ux/sweep2-taxonomy
---

# refactor: Decouple emulation from render

> **Stacking:** this work targets a new branch off `ux/sweep2-taxonomy` (the current UX stack), delivered as **two PRs** — Phase 1 (decoupling) and Phase 2 (live display under native menus). The boundary is intentional: measure Phase 1 first, then decide whether Phase 2 is needed.

---

## Summary

The emulator stutters, drops frames, and collapses to ~6 FPS when a key is held in a game — on an M2-class machine that should run a 4 MHz Z80 thousands of times faster than real time. A pacing audit (see **Origin / Evidence**) shows the cause is **architectural, not compute**: the Z80 emulation thread is welded to the render thread through a *single* shared frame buffer and a blocking handshake, and present is hardcoded to vsync. So the display's clock (60 Hz locally, ~41 Hz over remote desktop) leaks into the emulation's clock (50 Hz). Any render hiccup — vsync beat, a slow present, a heavy per-key-event operation on the render thread — stalls the Z80 and drops emulated FPS.

This plan **decouples** the two: the Z80 runs free at its existing wall-clock 50 Hz cadence into a triple-buffered frame ring and never waits on the render thread; the render thread presents the latest completed frame independently. That single change fixes the stutter, the keypress collapse, and the native-menu *emulation* stall. Phase 2 then adds a CADisplayLink/run-loop driver so the *display* also stays live while a native macOS menu is open.

---

## Problem Frame

Three felt symptoms, one root cause.

| Symptom | Where it shows | Mechanism |
|---|---|---|
| General stutter / judder; can't hold steady 50 FPS | Always, worse over remote desktop | 50 Hz emulation vs 60/41 Hz vsync present, coupled through the frame handshake → beat + drag |
| ~6 FPS while holding a key in a game | Key auto-repeat during gameplay | A heavy per-event operation on the render/main thread stalls render → Z80 blocks in `wait_consumed()` → emulation stalls |
| 0 FPS while a native macOS menu is held open | macOS only | NSMenu modal tracking suspends the main-thread render loop → no `signal_consumed()` → Z80 blocks |

**Root cause (shared):** `kon_cpc_ja.cpp:4157-4158` — after each frame the Z80 thread calls `signal_ready()` then **blocks in `wait_consumed()` until the render thread finishes Phase A** (texture upload + ImGui render). There is exactly one CPC frame buffer (`vid` / `back_surface` in `src/video.cpp`), so the Z80 cannot start the next frame until the render thread is done reading the current one. Combined with hardcoded vsync present (`src/video.cpp:476`, `SDL_GPU_PRESENTMODE_VSYNC`), the render thread's timing dictates the Z80's timing.

**Non-goal framing:** emulation pacing itself is *correct* and must be preserved — a wall-clock 50 Hz deadline (sleep-then-spin) lives in the Z80 thread (`kon_cpc_ja.cpp:3886`, gated by the F9 speed limiter `CPC.limit_speed`). The bug is the *coupling*, not the pacer.

---

## Origin / Evidence

This plan is grounded in a read-only pacing audit performed this session. Key citations (GUI/threaded path):

- **Emulation pacer (keep):** `koncepcja.h:71` `FRAME_PERIOD_MS 20.0`; deadline computed at `kon_cpc_ja.cpp:2320` scaled by `CPC.speed`; sleep-then-busy-spin throttle at `kon_cpc_ja.cpp:3886-3903`, gated by `CPC.limit_speed`, hooked to `EC_CYCLE_COUNT` (`z80.cpp:1860`). Pacing is **time-based**, not audio- or vsync-driven.
- **The coupling (remove):** `FrameSignal` handshake `koncepcja.h:522-588`; Z80 `signal_ready()`/`wait_consumed()` at `kon_cpc_ja.cpp:4157-4158`; render `try_wait_ready_for`/`video_display`/`signal_consumed` at `kon_cpc_ja.cpp:4923-4961`.
- **Hardcoded vsync (make configurable):** `src/video.cpp:476` and `:2651` set `SDL_GPU_PRESENTMODE_VSYNC`; no config/CLI option exists.
- **Single frame buffer (triple-buffer):** `vid` / `back_surface` in `src/video.cpp`; GPU submit happens inside `gpu_flip_a` (`video.cpp:565,612`), `gpu_flip_b` is empty (`video.cpp:625`).
- **Busy-poll smell:** render loop spins `SDL_PumpEvents()` every 1 ms when no frame is ready (`kon_cpc_ja.cpp:4923`).
- **Frame-skip already present:** `CPC.skip_rendering` decided at `kon_cpc_ja.cpp:3910-3924`, `MAX_CONSECUTIVE_SKIPS=5`.
- **Headless twin loop** (single-threaded, no coupling) at `kon_cpc_ja.cpp:~5000-5520` — out of scope, must remain working.

Related institutional knowledge to respect (from CLAUDE.md / memory): the RendBuff overrun crash class, the keyboard-scan race (double-buffered `keyboard_matrix`), and the macOS Metal "Cocoa run loop must stay alive for drawable-completion" pitfall.

---

## Scope Boundaries

### In scope
- Decouple the Z80 thread from the render thread via a triple-buffered CPC frame ring (Phase 1).
- Preserve the wall-clock 50 Hz emulation limiter as the sole emulation pacer.
- Find and fix the per-key-event cost that collapses FPS to ~6 (Phase 1).
- Make vsync present configurable (default on) (Phase 1).
- Extract `render_one_frame()` and add a CADisplayLink/run-loop driver so the display stays live during native macOS menu tracking (Phase 2).

### Deferred to Follow-Up Work
- Replacing the entire main `while` loop with a fully run-loop-driven architecture on **all** platforms. Phase 2 adds the macOS driver only; a portable rewrite is a later decision once Phase 1+2 are proven.
- Window-resize modal-tracking stall (same class as the menu stall; revisit after Phase 2).
- The app-menu lowercase-`koncepcja` cosmetic (`beads-1w33`), unrelated.

### Outside this effort
- The headless single-threaded loop (no coupling problem there).
- Any change to the emulated CPC timing/accuracy.

---

## Key Technical Decisions

1. **Triple buffer, not double.** Emulation (≤50 Hz when limited) is normally *slower* than present (60 Hz vsync), so double-buffering would suffice in the good case. But the whole point is the *slow-render* case (remote desktop ~41 Hz, native menu, GPU stall) where the Z80 laps the render thread. With 2 buffers the Z80 would have to overwrite the buffer the render thread is still reading. **Three buffers** (a small ring with an atomic "latest published index") let the Z80 always have a free buffer to write while the render thread reads another and a third holds the most-recent-complete frame. The Z80 never blocks; under slow render it simply drops *displayed* frames, which is correct.

2. **Keep `FrameSignal` as a wake-up, drop the back-pressure.** The Z80 should still *notify* the render thread that a new frame exists (so the render thread can wake from its wait), but it must **not block** waiting to be consumed. Replace `wait_consumed()` back-pressure with a non-blocking publish (atomic index + condition-variable notify). The render thread reads the published index; the Z80 keeps running.

3. **Emulation pacing stays exactly where it is.** The `kon_cpc_ja.cpp:3886` wall-clock limiter is untouched and remains the only thing that holds 50 Hz. Decoupling must not accidentally let the Z80 free-run (which would happen if the handshake was its only brake) — verify the limiter still gates every frame.

4. **Vsync stays default-on but becomes a config option.** Add `video.vsync` (default 1). This does not fix the coupling by itself, but once decoupled, turning vsync off becomes a safe escape hatch (e.g., for remote desktop) without dragging emulation.

5. **Phase 2 uses the least-invasive driver.** Extract the loop body into a non-blocking `render_one_frame()`. On macOS, drive it from a `CADisplayLink` (macOS 14+) or a `CFRunLoopTimer` registered in `NSEventTrackingRunLoopMode`, started/stopped on the existing menu begin/end-tracking observers. Do **not** rewrite the main loop on Windows/Linux (no native-menu modal stall there).

6. **Ownership model for the frame ring is explicit.** Exactly one writer (Z80 thread) and one reader (render thread). Every existing reader/writer of `vid`/`back_surface` (ASIC sprite draw, OSD text `print()`, screenshot/thumbnail capture, dock-icon preview, devtools panel surfaces) must be audited and pointed at the correct buffer. This audit is the main risk and gets its own unit.

---

## High-Level Technical Design

*This illustrates the intended approach and is directional guidance for review, not implementation specification. The implementing agent should treat it as context, not code to reproduce.*

Today (coupled):

```
Z80 thread:   ...emulate frame -> write back_surface -> signal_ready -> WAIT(consumed) ----+
                                                                                            | blocks
Render thread: wait_ready -> upload back_surface -> ImGui -> submit(vsync) -> signal_consumed +
              ^----------------------------- render clock dictates Z80 clock -------------------
```

After Phase 1 (decoupled, triple-buffered ring):

```
frames[3], atomic published_index, atomic write_index

Z80 thread:   emulate -> write frames[write] -> publish(write) [notify] -> pick next free buffer -> continue
                (never blocks on render; paced ONLY by the 50 Hz wall-clock limiter)

Render thread: on notify or vsync tick -> read frames[published] -> upload -> ImGui -> submit(vsync)
                (presents latest complete frame; if slow, simply misses intermediate frames)
```

After Phase 2 (macOS display stays live under menus):

```
render_one_frame()  == one non-blocking iteration of the render thread body
normal:        main loop calls render_one_frame()
menu tracking: CADisplayLink / CFRunLoopTimer(NSEventTrackingRunLoopMode) calls render_one_frame()
```

Buffer-safety invariant: the render thread holds a read lease on `published_index`; the Z80 chooses its next write buffer as "any index that is neither `published` nor the render's in-use lease." With three buffers this set is never empty.

---

## Implementation Units

### Phase 1 — Decouple emulation from render (PR 1)

### U1. Baseline measurement harness

**Goal:** Establish before/after numbers so the refactor is provably an improvement, not a vibe.
**Requirements:** Advances all symptoms (measurement gate).
**Dependencies:** none.
**Files:** `src/kon_cpc_ja.cpp` (extend the existing `--fps`/`--debug` passive log), `test/integrated/ipc_harness.py` (add a steady-FPS + keypress-stress assertion if feasible), `docs/plans/2026-06-21-001-refactor-decouple-emulation-render-plan.md` (record numbers).
**Approach:** Reuse the non-perturbing `[fps]` stdout log (do NOT use IPC `wait vbl` — it perturbs). Add an opt-in counter for "Z80 frames where `wait_consumed` blocked > N ms" so the coupling is quantified before removal. Capture three baselines on the current binary: idle FPS, FPS while holding a key in a running program, FPS while a native menu is open.
**Execution note:** Characterization-first — capture current behavior before changing it.
**Patterns to follow:** the existing `g_log_fps`/`--fps` path (`kon_cpc_ja.cpp:3817`).
**Test scenarios:**
- Idle: passive `[fps]` log shows the steady-state baseline (expect ~41–50 depending on display path).
- Keypress stress: hold a key in a loaded program; record the FPS floor (expect the ~6 FPS collapse to reproduce).
- Native menu: open a native menu; record FPS (expect ~0).
**Verification:** Three baseline numbers recorded in this plan's appendix; the keypress collapse reproduces on demand.

### U2. Triple-buffered CPC frame ring

**Goal:** Replace the single CPC frame buffer with a 3-surface ring so the Z80 always has a free buffer to write.
**Requirements:** Root-cause fix (coupling) — advances stutter, keypress, native-menu stall.
**Dependencies:** U1.
**Files:** `src/video.cpp` (allocation/lifetime of the CPC surfaces; today's `vid`/`back_surface`), `src/video.h` (accessors for "current write surface" and "publish"), `src/koncepcja.h` (extend or replace `FrameSignal` with the ring's atomics + notify).
**Approach:** Allocate three CPC surfaces identical to today's `back_surface`. Maintain `std::atomic<int> published_index` and a render-side in-use lease. Provide `video_frame_write_surface()` (the Z80's current target) and `video_frame_publish()` (advance published index + condvar notify). The Z80 writes into its working surface exactly as today, then publishes. Keep allocation/format/pitch identical to the existing surface so all pixel code is unchanged.
**Technical design (directional):** see High-Level Technical Design; the free-buffer pick is "the index that is neither `published_index` nor the render lease."
**Patterns to follow:** the keyboard double-buffer (`keyboard_matrix` → `keyboard_matrix_live`, `publish_keyboard_snapshot`) is the closest in-repo precedent for a one-writer/one-reader publish.
**Test scenarios:**
- Single-thread sanity: with vsync and limiter on, frames advance and the published index cycles through all three buffers over time.
- No tearing: the render thread, reading `published_index`, never observes a half-written frame (visual check + an assertion that the Z80 never writes the index currently leased by render).
- Lap case: with the render thread artificially slowed, the Z80 keeps producing (published index keeps advancing) and never blocks.
**Verification:** Z80 thread no longer references `wait_consumed`; ring invariant (writer never targets the render lease) holds under a slowed-render test.

### U3. Render thread consumes published frame without blocking the Z80

**Goal:** Make the render loop read the latest published frame and remove the back-pressure on the Z80.
**Requirements:** Root-cause fix (coupling).
**Dependencies:** U2.
**Files:** `src/kon_cpc_ja.cpp` (render loop ~4900: replace `wait_ready`/`signal_consumed` back-pressure with "read published index"), `src/video.cpp` (`gpu_flip_a` and the other `flip_a` variants read the leased published surface instead of the single `vid`).
**Approach:** Render loop: wait (condvar) for "a newer frame than last presented" OR a vsync tick; take a read lease on `published_index`; upload + ImGui + submit; release lease. The Z80's publish no longer waits for consume. Keep the `SDL_PumpEvents` cadence that services the macOS run loop, but it no longer needs to busy-spin against the Z80 (revisit the 1 ms poll).
**Patterns to follow:** existing `gpu_flip_a` upload path (`video.cpp:376-403`).
**Test scenarios:**
- Decoupling proof: with the render thread slowed (sleep injected), the `[fps]` emulation log holds at 50 while presented FPS drops — emulation no longer follows render.
- Deadlock probe: `ipc_harness.py` `wait bp` style check — rapid pause/resume and frame stepping never hang (no new deadlock from the changed handshake).
- Native-menu emulation: open a native menu; the `[fps]` log keeps printing ~50 (emulation runs) even though the display is frozen.
**Verification:** Phase-1 success criteria (below) for stutter and native-menu emulation are met; no IPC-harness deadlock regressions.

### U4. Confirm the 50 Hz limiter is the sole pacer

**Goal:** Guarantee the wall-clock limiter still gates every frame after the handshake back-pressure is gone.
**Requirements:** Preserve emulation pacing (KTD 3).
**Dependencies:** U3.
**Files:** `src/kon_cpc_ja.cpp` (Z80 thread `z80_thread_main` ~3787, the `EC_CYCLE_COUNT` limiter block ~3886).
**Approach:** Audit that with the handshake gone, the limiter still runs once per emulated frame and the deadline math is intact. Verify F9 still toggles cap on/off and that "uncapped" now runs the Z80 truly flat-out (a fast-forward that was previously masked by render back-pressure).
**Test scenarios:**
- Limiter on: emulated FPS holds 50 ± small jitter, independent of display refresh.
- Limiter off (F9): emulated FPS rises well above 50 (true fast-forward) and the UI stays responsive.
- Speed config: `system.speed` other than 4 scales the deadline as before.
**Verification:** Capped FPS is a flat 50 on both 60 Hz local and remote-desktop paths; uncapped is materially faster than before.

### U5. Fix the per-key-event FPS collapse

**Goal:** Find and remove the heavy per-key-repeat operation that drives FPS to ~6.
**Requirements:** Keypress-collapse symptom.
**Dependencies:** U3 (decoupling removes the *amplification*; this unit removes the *source*).
**Files:** `src/kon_cpc_ja.cpp` (SDL key event handling in the main loop), `src/keyboard.cpp` (`applyKeypress`, `publish_keyboard_snapshot`), `src/macos_menu.mm` (`koncpc_restore_keyboard_focus` — `makeKeyAndOrderFront` per event is a prime suspect), and the ImGui input path if implicated.
**Approach:** Characterize first: instrument per-key-event handling time; hold a key and find the operation costing ~160 ms/event. Prime suspects, in order: (a) `koncpc_restore_keyboard_focus` / Cocoa window ops fired per event; (b) an ImGui state rebuild or layout recompute triggered by input; (c) mutex contention with `publish_keyboard_snapshot`; (d) key-repeat events not being coalesced/filtered (note: prior work already filters repeat on *emulator command* keys — verify game keys aren't doing something heavy). Fix the specific cause (debounce/coalesce, move off the hot path, or remove the per-event window op).
**Execution note:** Characterization-first — this is a measured bug hunt; do not guess-patch.
**Patterns to follow:** the existing key-repeat filter for emulator commands; the keyboard-scan-race fix's snapshot discipline.
**Test scenarios:**
- Repro: hold a key in a loaded program on the *baseline* binary → FPS floor ~6 (from U1).
- Fixed: same action post-fix → FPS stays at 50 (or within normal jitter).
- No regression: single key presses still register correctly in the CPC (no dropped/duplicated keys); the keyboard-scan-race fix still holds (shifted digits correct).
**Verification:** Holding any key during gameplay no longer drops emulated FPS below the normal steady state.

### U6. Make vsync present configurable

**Goal:** Add a `video.vsync` option (default on) so the present mode can be switched without code edits.
**Requirements:** KTD 4; escape hatch for remote-desktop/judder cases.
**Dependencies:** none (independent; can land any time in Phase 1).
**Files:** `src/video.cpp` (`:476`, `:2651` — choose present mode from config), `src/video_gpu.cpp` if the mode is set there, the config loader (`configuration.cpp` / wherever `[video]` keys are parsed), `koncepcja.cfg` (document the key), `CLAUDE.md` config section (document).
**Approach:** Read `video.vsync` (default 1). Map to `SDL_GPU_PRESENTMODE_VSYNC` (on) vs `SDL_GPU_PRESENTMODE_IMMEDIATE`/`MAILBOX` (off), guarded by `SDL_WindowSupportsGPUPresentMode`. With decoupling done, vsync no longer affects emulation pacing, so toggling it is safe.
**Test scenarios:**
- Default: no config change → vsync on (present mode VSYNC), behavior identical to today.
- Off: `video.vsync=0` → present mode is IMMEDIATE/MAILBOX where supported; emulation FPS unchanged (still 50, because decoupled).
- Unsupported mode: falls back to VSYNC with a logged warning, no crash.
**Verification:** Config key flips the present mode; emulated FPS is unaffected either way.

### Phase 2 — Live display under native menus, macOS (PR 2)

### U7. Extract `render_one_frame()`

**Goal:** Move the render-loop body into a single non-blocking function callable from multiple drivers.
**Requirements:** Prerequisite for U8.
**Dependencies:** U3 (decoupled render path).
**Files:** `src/kon_cpc_ja.cpp` (render loop ~4900).
**Approach:** Pure mechanical extraction: `render_one_frame()` does one iteration — pump events, if a newer published frame exists upload+ImGui+submit, else return immediately. The main `while` loop calls it in a loop exactly as today. No behavior change.
**Execution note:** Characterization-first — confirm 50 FPS and identical behavior before and after the extraction (it must be a no-op refactor).
**Test scenarios:**
- Equivalence: idle and gameplay FPS identical before/after extraction.
- Re-entrancy guard: `render_one_frame()` never calls `ImGui::NewFrame` twice in one display cycle (single ownership of the ImGui frame).
**Verification:** Diff is a pure extraction; measured FPS unchanged.

### U8. CADisplayLink/run-loop driver during native-menu tracking (macOS)

**Goal:** Keep the display animating while a native macOS menu is held open.
**Requirements:** Native-menu *display* symptom (the part decoupling alone leaves frozen).
**Dependencies:** U7.
**Files:** `src/macos_menu.mm` (reuse the menu begin/end-tracking observer hooks — already present in this file's history), `src/kon_cpc_ja.cpp` (expose `render_one_frame()` for the driver).
**Approach:** On `NSMenuDidBeginTracking`, start a `CADisplayLink` (macOS 14+) or a `CFRunLoopTimer` registered in `NSEventTrackingRunLoopMode`, whose callback calls `render_one_frame()`. On `NSMenuDidEndTracking`, stop it. A yield flag prevents the main `while` loop and the tracking driver from both rendering in the same display cycle. Confirm SDL_GPU swapchain acquire + submit work from the tracking-mode callback (spike early — the Cocoa run loop is alive during tracking, so they should).
**Patterns to follow:** the begin/end-tracking observer registration explored earlier this session (the reverted pause experiment), minus the pause.
**Test scenarios:**
- Live display: hold a native menu open over a running program → the CPC screen keeps animating; `[fps]` log stays ~50.
- Transition safety: rapid open/close and nested submenus → no double `NewFrame`, no deadlock, no stuck "yielded" state.
- Present-during-tracking: SDL_GPU present from the tracking callback succeeds (no Metal drawable hang).
- Non-macOS: unaffected (the driver is macOS-only; Windows/Linux keep the plain loop).
**Verification:** A native menu held open shows live emulation behind it; FPS holds; no deadlock on repeated open/close.

### U9. Cleanup pass

**Goal:** Remove now-dead workarounds the decoupling makes unnecessary.
**Requirements:** Hygiene.
**Dependencies:** U8.
**Files:** `src/kon_cpc_ja.cpp` (the 1 ms `SDL_PumpEvents` busy-poll if reducible now), `src/koncepcja.h` (drop `FrameSignal` members the ring replaced), comments referencing the vestigial "Phase B concurrency" model.
**Approach:** Delete back-pressure remnants and stale comments; ensure the headless twin loop is untouched and still builds.
**Test scenarios:**
- Headless build + IPC harness still pass (`SDL_VIDEODRIVER=dummy`).
- No reference to removed `FrameSignal` members remains.
**Verification:** Clean build (macOS + headless), IPC harness green, no dead code referencing the old handshake.

---

## System-Wide Impact

- **Frame-buffer readers** beyond the render path must be audited against the ring (covered by U2/U3): ASIC sprite draw (`asic_draw_sprites` before handoff), OSD text `print()` into the CPC surface, screenshot + `.kthm` thumbnail capture (`video_capture_cpc_thumbnail` reads `vid`), the macOS dock-icon preview (reads `back_surface`), devtools panel surfaces. Each must read/write the correct buffer (the Z80's working surface for writers; a stable published surface for readers).
- **Headless path** (`kon_cpc_ja.cpp:~5000`) is single-threaded and unaffected — must keep building and passing the IPC harness.
- **Other platforms:** Phase 1 is cross-platform and pure win (decoupling + vsync option). Phase 2 is macOS-only; Windows/Linux keep the existing loop.

---

## Risk Analysis & Mitigation

| Risk | Likelihood | Mitigation |
|---|---|---|
| A frame-buffer reader (screenshot, dock preview, OSD, sprites) reads a buffer the Z80 is writing → tearing or the RendBuff-class crash | Med | U2/U3 audit every reader; one-writer/one-reader leases; visual + assertion tests; mirror the keyboard double-buffer discipline |
| New deadlock at the changed handshake (history: frame-wait deadlock, Metal run-loop starvation) | Med | Keep condvar notify but remove blocking back-pressure; IPC-harness `wait bp`/pause-resume stress; keep `SDL_PumpEvents` servicing the Cocoa run loop |
| Decoupling accidentally lets the Z80 free-run (loses 50 Hz) | Low | U4 explicitly verifies the wall-clock limiter still gates every frame |
| Present-from-tracking-callback hangs on Metal (Phase 2) | Med | Spike it first in U8; the run loop is alive during tracking; fall back to "decoupling only" (Phase 1) if it proves unsafe |
| Keypress root cause is multiple/elusive | Low | U5 is characterization-first with ordered suspects; decoupling already removes the amplification even if the per-event cost is only reduced, not eliminated |

---

## Phased Delivery

- **PR 1 (Phase 1, U1–U6):** the decoupling + keypress fix + vsync option. Independently shippable; fixes the felt pain. **Decision gate:** measure against U1 baselines. If stutter and keypress are resolved and native-menu-frozen-display is acceptable, Phase 2 is optional.
- **PR 2 (Phase 2, U7–U9):** `render_one_frame()` extraction + macOS CADisplayLink driver + cleanup. Only proceed if the frozen-display-under-menus still bothers in practice.

Both PRs stack on `ux/sweep2-taxonomy`.

---

## Verification / Success Criteria

**Phase 1:**
- Emulated FPS is a steady 50 with the limiter on, **independent of display refresh** (60 Hz local and remote-desktop paths both show 50 in the `[fps]` log).
- Holding a key during gameplay no longer drops emulated FPS below steady state.
- Opening a native macOS menu no longer stalls *emulation* (the `[fps]` log keeps printing ~50 while the menu is open; the display may still be frozen).
- No IPC-harness deadlock/regression; headless build green; no tearing or RendBuff-class crash under a slowed-render stress test.

**Phase 2:**
- A native menu held open shows the emulator animating behind it; FPS holds ~50.
- Rapid open/close and nested submenus: no deadlock, no double-render, no stuck state.

---

## Deferred to Implementation (execution-time unknowns)

- Exact root cause of the keypress collapse (U5) — to be found by instrumentation, not assumed.
- Whether the 1 ms `SDL_PumpEvents` poll can be lengthened/removed after decoupling (U9) — depends on observed run-loop servicing.
- Whether `CADisplayLink` (macOS 14+) or a `CFRunLoopTimer` is the better Phase-2 driver — decide during the U8 spike based on Metal present behavior.
- Final shape of the ring accessors in `video.h` — settled when touching the real code.
