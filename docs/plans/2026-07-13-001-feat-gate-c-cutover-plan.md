# Gate C cutover plan — from "mostly ready" to "only OUR engines" (2026-07-13)

Parent epic: beads-o7fw (THE replacement). This plan turns the remaining
"mostly" — the pieces that still bind the Caprice32 inheritance after commit
6c5df526 — into ordered, gated phases ending in the delete-first burn-down and
the orphan-repo publish. It refines, and does not replace, the deletion order
in docs/replacement-ledger.md.

**Ground state (6c5df526):** machine.cpp is hw-pure (bench/sim/PGO link);
debugger semantics (conditional/pass-count/ranged watchpoints, old-value,
host pokes) are engine-identical via shared predicates; drive B has full
media lifecycle; .voc plays under engine=1 (deck block 0x15); serial
file/TCP/tty ride the rs232 Device; SNA v3 restores FDC/printer/PSG-env/
CRTC/GA state; harness is green under both engines (1585/1586, 1 optional
skip).

## Phase A — finish the Wave-1 consumer audit (beads-4q7r, in progress)

Every remaining reader of legacy globals either becomes a Device peek or is
tagged engine=0-only (and therefore dies in Phase E burn-down):

- **ExprContext**: conditional bp/wp evaluation binds `&CRTC/&GateArray/&PSG`
  (frame-boundary mirrors — stale mid-frame under engine=1). Back the
  context with machine peeks when the bridge is active.
- **DevTools surfaces** still reading `t_FDC`/`t_drive` (drive LED, disc
  tools) and `t_z80regs` beyond the synced view.
- **Telnet console hooks**: TXT_OUTPUT/BDOS hooks live in legacy z80.cpp;
  verify the engine=1 instr-hook adapter covers both, port if not.
- **Keyboard matrix through legacy PSG registers** (engine=0 path only —
  confirm and tag).

Exit gate: a grep inventory of `extern t_CRTC|t_GateArray|t_PSG|t_FDC|
t_drive|t_z80regs` in src/ where every hit is either peek-backed or sits in
a file on the ledger's deletion list.

## Phase B — video.cpp host split (the big one)

Author the new presentation layer over `subcycle::Machine`'s framebuffer:
frame ring, video plugin vtable (direct/GL/headless), scaling, scr_style
shader set (incl. the CRT GPU trio 17–19), scanlines, OSD/font. The bridge
blit is the existing seam — grow it into the module, then delete both halves
of video.cpp. Perf gate: present path adds no machine-thread stalls
(render-wait stays 0; FPS unchanged in the 4-cell interleaved bench).

## Phase C — media ingest parity (feature-parity commitment)

- **.voc**: author a clean VOC→TZX(0x15) converter (the deck side landed in
  6c5df526; `tape_insert_voc` itself is inherited code). ~200 lines + test.
- **IPF/RAW**: decision — capsimg is a *declared dependency* per the ledger,
  so the clean path is vendored capsimg glue → flux → `insert_flux`
  (engine=1), not a rewrite. Alternative (drop the formats) violates parity.
- **zip**: vendored minizip / clean unzip (ledger §2), feeding the byte-
  buffer loaders.

## Phase D — Wave-2 devices close-out  ✓ CLOSED 2026-07-13

All three items verified: the ASIC Device feature set was already complete
(beads-jkhg increments through soft scroll); the engine=1 Plus GUI boot
instability (beads-gbey) is fixed and stress-confirmed 6/6; the MF2 parity
checklist is satisfied by the hw Device's test suite. asic.cpp itself dies
in the Phase E burn-down with the other legacy-global readers.

- **ASIC/Plus**: beads-jkhg (in progress) — the one true §1 replace-and-
  delete left in the device column.
- **Multiface II**: already re-expressed as a `src/hw` Device (ledger §3);
  verify parity checklist, then the legacy path retires with engine=0.
- Light gun: done (`src/hw/light_gun.cpp`), modulo beads-g4jq tier bug.

## Phase E — Wave-3 shell + the delete-first burn-down (worktree)

In a parallel worktree on the branch:

1. `git rm` the ENTIRE ledger §1 + §2 inherited list in one commit —
   z80.*, crtc.*, psg.cpp, fdc.cpp, disk.*, tape.*, asic.*, video.*,
   kon_cpc_ja.cpp, slotshandler.*, keyboard.*, configuration.*, zip.*,
   cartridge.*, snapshot path, utilities.
2. The compiler now enumerates every remaining consumer — the audit is
   machine-verified, nothing hides. Burn the list down: re-author (host
   loop, media manager, input mapper, config store, SNA import/export
   against Device save/load) or re-point to `src/hw`/`src/subcycle`.
3. Harness goes engine=1-only (drop the dual-engine loop and engine=0
   gates); FUSE/timing suites already target `src/hw/z80`.

Exit gates: full unit suite green; IPC harness green; the CLAUDE.md
peripheral feature checklist demonstrably intact (parity is absolute); §8.3
bench — Fast ≥ the beat-bars (3582/923/2712/692 reference), interleaved
runs only.

## Phase F — provenance: orphan branch / fresh repo ("v6")

- Rebuild the history as the construction log (beads-mft5 / beads-6zx7):
  spec → device → tests → integration, device by device.
- **Dates policy: real dates only.** Each rebuilt commit carries the
  AuthorDate of the real commit(s) that landed that work — exactly what
  git itself does across rebases. The work genuinely spans Feb–Jul 2026,
  so the honest history already reads as evenings-and-weekends; no
  synthetic timestamps. (Fabricated dates would poison the provenance
  story the orphan repo exists to establish.)
- Mechanics: `git checkout --orphan` (or a fresh repo) whose first commit
  is the v6 root; `git filter-repo` on a clone is the reference tool for
  extracting authored-file history with true dates. The current private
  repo remains intact as the primary provenance record.
- LICENSE/NOTICE pass; third-party to `vendor/` (ledger "Continuous").
  **License DECIDED 2026-07-13: konCePCja Source License 1.0.0** — a custom
  source-available license based on **PolyForm Internal Use 1.0.0** with an added
  **Personal & Hobby Use** grant (from PolyForm Strict). Net effect: anyone may
  read, use, and modify the source for personal/hobby/internal purposes —
  **including as a tool to make their own works, commercial or not** — but may
  NOT distribute the software or publish forks/builds. (Strict was considered
  first but rejected: it's noncommercial-only and would block using konCePCja as
  a tool to make a commercial CPC game — the wrong axis; we care about
  distribution, not commerciality of use.) The completed cutover — no surviving
  GPLv2 Caprice32 file — is what unlocks a non-copyleft choice. Groundwork landed
  on `feat/sub-cycle-chip-sim`: `LICENSE.md` (the merged text, honest non-PolyForm
  naming), `NOTICE.md` (provenance + vendored-dep attributions + capsimg
  redistribution caveat), `CONTRIBUTING.md` (DCO + inbound grant — license grants
  use+modification but not distribution/relicensing, so the inbound grant is what
  lets the maintainer ship/relicense contributions), README rewrite. ROMs stay
  under `rom/ROM-LICENSE.txt`, unaffected. CAVEAT: custom/modified license is
  legally unvetted — worth a lawyer's glance if commercial stakes rise. OPEN
  before public v6: capsimg redistribution terms (optional/user-supplied in
  public builds?).

## Sequencing & tracking

A → B can proceed in parallel with C and D; E requires A–D complete;
F is last. Tracked in bd under epic beads-o7fw:
Phase A = beads-4q7r (existing) · Phase B/C/E-shell/E-burn-down/F-publish =
new beads filed 2026-07-13 · Phase D = beads-jkhg (existing) + MF2 parity
check. beads-xsdc (dual-drive engine=1) is satisfied by 6c5df526.
