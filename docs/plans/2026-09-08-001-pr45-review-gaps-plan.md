---
title: "chore: Dress leftover PR #45 review gaps"
date: 2026-09-08
type: chore
depth: lightweight
origin: ce-code-review of PR #45 (run 20260908-220434-40549574)
---

# chore: Dress leftover PR #45 review gaps

## Overview

PR #45 (`chore/beads-next-batch`) plus `13db3b7c` (`fix(review): …`) closed the
validated P0/P1 findings and the P2 failed-push host-view rollback. Five gaps
from that review are still open: two product/reliability leftovers and three
test holes. This plan dresses them in one vertical-slice order so agents get
disk save/eject parity, quiescence cannot hang forever, and the live-FDC
contract is proven under `engine=1`.

## Problem Frame

The review’s remaining items were explicitly *not* applied:

1. **Agent-native:** File ▸ Save Disk / Eject Disk have no IPC verbs. Agents
   can `disk put`/`rm`/`format` but cannot persist or unmount a disc.
2. **Reliability:** `cpc_wait_quiescent` spins on `g_z80_quiescent` with no
   deadline. A stuck Z80 thread turns every `CpcPauseLease` into a hang.
3. **Testing:** flux size-mismatch refuse is untested against an attached SCP;
   no IPC harness exercises pull/push on a live sub-cycle board; `profile load`
   ERR/resume is untested (happy-path rebuild only).

#6 (host rollback after failed push) is **done** — do not re-open it.

## Requirements

- **R1** — IPC `disk save <A|B> <path> [dsk|scp|hfe]` writes the **live FDC
  medium** via `flux_save_to_file` (same rule as File ▸ Save Disk). Default
  format `dsk`. `ERR 409` when `flux_save_caps` forbids the format. Path
  traversal rejected like other IPC file commands.
- **R2** — IPC `disk eject <A|B>` unmounts the drive the same way the GUI
  confirm path does (`dsk_eject`). No GUI confirm for agents. Dirty media
  follows the existing flush-on-eject path; do not invent a second persist
  policy.
- **R3** — IPC `disk status <A|B>` reports presence and save caps
  (`present`, `backing=sector|flux|empty`, `can_dsk`, `can_scp`, `can_hfe`)
  so agents and tests can branch like the File menu.
- **R4** — `cpc_wait_quiescent` (and therefore `CpcPauseLease::wait`) has a
  bounded wait. Timeout must **fail closed**: do not touch shared machine
  state if the Z80 thread never went quiescent. IPC surfaces `ERR 504`.
- **R5** — A unit or board-level test proves a size-mismatched host DSK cannot
  `insert_disk` over an attached writable-flux SCP (the P0 fallthrough).
- **R6** — `test/integrated/ipc_harness.py` covers `disk new` → `put` → `cat`
  (and save/eject once R1–R3 exist) against a live emulator, not the inactive-
  bridge unit stub.
- **R7** — Harness: `profile load` of a missing name on a running machine
  returns ERR and the machine still advances (not left paused).

## Key Technical Decisions

- **Save goes through `flux_save_*`, never `dsk_save` on `driveA/B`, when the
  bridge is active.** That is already the GUI contract (`save_disk_to` in
  `src/imgui_ui.cpp`). Headless IPC always has a board in this tree.
- **`disk status` is part of save/eject, not a separate epic.** Caps are how
  the GUI greys menu items; agents need the same predicate.
- **Eject skips the ImGui confirm.** Agents pass an explicit command; the
  confirm exists because a click is easy to miss. Document dirty write-back.
- **Quiescence timeout is fail-closed, not “log and continue”.** Continuing
  after a missed quiescent flag is the UAF the lease was built to prevent.
  Suggested budget: 5 s (same order as `wait bp` 5000 ms). Headless stays
  immediate (`g_z80_quiescent` already stays true).
- **Tests before new verbs where possible.** R6 (put/cat on live board) and R7
  (profile ERR resume) lock in already-shipped review fixes and can land
  first. R5 may need a small FDC/bridge fixture; prefer extending
  `test/disk_media_sync_test.cpp` or `test/hw/fdc_flux_write_test.cpp` over a
  new harness if a live `g_bridge` cannot be started cheaply.
- **Do not** add IPC `disk save` by serializing the host `t_drive` — that
  reopens beads-lly6.

## Existing Patterns to Follow

- GUI save: `save_disk_to` / `flux_save_to_file` / `flux_save_caps`
  (`src/imgui_ui.cpp`, `src/flux_save.h`).
- GUI eject: `dsk_eject` after confirm (`src/imgui_ui.cpp` status-bar popup).
- IPC disk mutations: `with_synced_drive` + `CpcPauseLease` + rollback helper
  (`src/koncepcja_ipc_server.cpp`, `subcycle_bridge_rollback_host_view`).
- Path safety: existing IPC `..` / absolute-path rejects in
  `koncepcja_ipc_server.cpp`.
- Harness: `EmulatorRunner` + `KoncepcjaIPC` in
  `test/integrated/ipc_harness.py` (`test_profile_load_rebuilds_machine`,
  engine=1 bp-clear test).
- Flux attach: `fdc_attach_flux_writable` + `Machine::insert_flux`
  (`src/subcycle/machine.cpp`, `test/hw/fdc_flux_write_test.cpp`).

## Task List

### Phase 1: Lock in shipped review fixes (no new product surface)

#### Task 1: Live-board IPC disk put/cat harness (R6 first slice)

**Description:** Prove beads-lly6 under a real emulator process: create a
blank disc, write a file, read it back. Today `DiskMediaSyncBridge` only
asserts pull/push no-op when the bridge is inactive.

**Acceptance criteria:**
- [ ] Harness starts the emulator (default/`engine=1` board).
- [ ] `disk new` + `disk put` + `disk cat` round-trip a small file.
- [ ] Failure message distinguishes “command ERR” from “stale host view”.

**Verification:** `python3 test/integrated/ipc_harness.py` (or the named
test) against `EmulatorRunner`.

**Dependencies:** None (uses current `disk new`/`put`/`cat`).

**Files likely touched:** `test/integrated/ipc_harness.py`

**Estimated scope:** S

#### Task 2: `profile load` missing-name keeps the machine running (R7)

**Description:** The review applied `restore_run_state` on load ERR. Prove a
running machine that hits `profile load no-such-profile` still runs.

**Acceptance criteria:**
- [ ] `profile load` of a missing name returns `ERR`.
- [ ] A subsequent `wait vbl` (or PC motion) succeeds within the usual
      timeout — not a pause-stuck hang.

**Verification:** new function in `test/integrated/ipc_harness.py`.

**Dependencies:** None.

**Files likely touched:** `test/integrated/ipc_harness.py`

**Estimated scope:** S

### Checkpoint: Phase 1

- [ ] Harness tests pass locally with `SDL_VIDEODRIVER=dummy`.
- [ ] No product IPC contract change yet.

### Phase 2: Agent-native disk persist/unmount (R1–R3)

#### Task 3: `disk status` / `disk save` / `disk eject`

**Description:** One vertical slice matching File ▸ Save Disk and Eject Disk.
`status` exposes `flux_save_caps` so save format errors are predictable.

**Acceptance criteria:**
- [ ] `disk status A` reports caps; empty drive is `present=0`.
- [ ] `disk save A <path> dsk` writes a loadable image from the live medium.
- [ ] Flux drive: `disk save A <path> scp` works; `disk save B … scp` is
      `ERR 409` (flux is drive-A-only).
- [ ] `disk eject A` clears the drive; `disk ls A` no longer lists files.
- [ ] `help disk` and `docs/ipc-protocol.md` updated.

**Verification:** `test/ipc_server.cpp` (status/eject/save on the test
server) plus harness coverage from Task 1 extended with save/eject.

**Dependencies:** Task 1 (reuse the put/cat fixture).

**Files likely touched:**
- `src/koncepcja_ipc_server.cpp`
- `docs/ipc-protocol.md`
- `test/ipc_server.cpp`
- `test/integrated/ipc_harness.py`

**Estimated scope:** M

### Checkpoint: Phase 2

- [ ] Agent can create, mutate, save, and eject a disc without the GUI.
- [ ] Save of a flux disc does not go through `dsk_save(&driveA)`.

### Phase 3: Flux P0 regression + quiescence bound (R5, R4)

#### Task 4: Size-mismatched flux push must not drop SCP (R5)

**Description:** The review fix refuses `insert_disk` when flux is attached
and overlay size differs. Add a test that would have failed before
`13db3b7c`.

**Acceptance criteria:**
- [ ] Fixture attaches writable flux (SCP + DSK overlay).
- [ ] Host view serialized at a *different* size, push returns false.
- [ ] `fdc_media_flux_scp` still non-null afterwards (SCP not replaced).

**Verification:** gtest in `test/disk_media_sync_test.cpp` if the app bridge
can be started; otherwise a Device-level test next to
`test/hw/fdc_flux_write_test.cpp` that documents `insert_disk` →
`fdc_media{}` drops SCP, plus a focused test of the push size-guard.

**Dependencies:** None (can parallelize with Phase 1). Prefer after Task 3 if
`disk status` is the observation channel.

**Files likely touched:** `test/disk_media_sync_test.cpp` and/or
`test/hw/fdc_flux_write_test.cpp`; maybe `src/subcycle_bridge.cpp` only if a
test hook is unavoidable (avoid).

**Estimated scope:** S–M

#### Task 5: Bounded `cpc_wait_quiescent` (R4)

**Description:** Lease wait must not spin forever. Timeout fails closed.

**Acceptance criteria:**
- [ ] Wait uses a deadline (default ~5 s).
- [ ] On timeout, lease does not proceed into destructive work; IPC returns
      `ERR 504` (or equivalent) rather than mutating.
- [ ] Existing pause-lease tests still pass; add a test that a never-quiescent
      flag does not hang the suite (inject / stub).

**Verification:** `test/ipc_server.cpp` pause-lease suite + a new timeout
test.

**Dependencies:** None (independent). Land after product slices so a wait-API
change does not collide with disk/profile work.

**Files likely touched:**
- `src/kon_cpc_ja.cpp`
- `src/koncepcja.h`
- `src/koncepcja_ipc_server.cpp` (propagate failure)
- `test/ipc_server.cpp`

**Estimated scope:** M

### Checkpoint: Complete

- [ ] All R1–R7 acceptance criteria met.
- [ ] Targeted unit tests + ipc_harness pass.
- [ ] Ready for review on a follow-up PR (not mixed into unrelated beads).

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Starting a full `g_bridge` in gtest is heavy/flaky | Med | Prefer FDC Device fixture for R5; use `EmulatorRunner` for R6 |
| Eject dirty-disc policy surprise | Med | Same flush as GUI; document; no extra prompt |
| Quiescence timeout too short on slow CI | Med | 5 s with headroom; only fail when flag never sets |
| `disk save` accidentally uses host `t_drive` | High | Call `flux_save_to_file` only; test that CPC writes appear in the saved image |

## Out of Scope

- IPC `disk save`/`eject` GUI confirm dialogs.
- Drive-B flux.
- Rewriting `dsk_parse` to fully in-memory (tmpfile fallback is already in
  `13db3b7c`).
- Re-litigating ignored-pull (validator rejected).

## Suggested PR split

1. **Tests-only** PR: Tasks 1–2 (+ Task 4 if the fixture is ready).
2. **`disk status|save|eject`** PR: Task 3.
3. **Quiescence timeout** PR: Task 5.

Tracker: beads under epic created with this plan (`docs/plans/2026-09-08-001-pr45-review-gaps-plan.md`).
