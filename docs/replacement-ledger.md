# The replacement ledger — Caprice32 must not survive

**Doctrine (2026-07-03):** konCePCja is a complete, hardware-inspired clean
emulator — informed by silicon, not by other emulators. **No file not authored
here survives.** Every change that lands a replacement **deletes** the
inherited file(s) it supersedes; a replacement isn't done until the original
is gone. This ledger is the enforcement artifact: consult and update it
whenever the frontier moves.

Three commitments that shape the whole plan:

- **FEATURE PARITY IS ABSOLUTE.** No user-visible capability is ever severed,
  even temporarily. A legacy file dies only when its replacement covers every
  feature it carried — which makes the peripheral queue (tape, printer DAC,
  Multiface, AMX mouse, Symbiface, M4, silicon disc, SmartWatch, phaser,
  Plus/ASIC) the main road of the replacement, not an optional annex. Each
  gets the full treatment: spec in docs/hardware/ → bus Device → tests
  against the legacy behaviour as baseline → swap → delete. The original
  code is the BASELINE ORACLE; the new code is spec-driven and re-modeled
  into the real architecture (expansion peripherals live on the expansion
  bus, not in the CPU loop).

- **The engine seam stays — but every selectable engine is OURS.** The
  language-neutral specs in `docs/hardware/` exist precisely so the same
  machine can be reimplemented in other languages (SPARK/Ada, …) and swapped
  in and out. "Legacy vs subcycle" is a transitional state, not the design;
  the end state is "which of OUR implementations".
- **The hardware-insanity bar holds through the cutover.** Replacement
  surfaces (DevTools, IPC readback, snapshots) read pin-level Device truth —
  no emulator-style approximations may sneak in as cutover conveniences.

Classification by first-appearance date in git history: 2016–2022 files are
the Caprice32 inheritance; 2026 files are ours. Third-party libraries
(portable-file-dialogs, msf_gif, TextEditor) are declared dependencies, not
authorship debt — they move to `vendor/`, they don't get rewritten. (The
non-commercial SPS Decoder Library was the one exception: rather than vendor
it, its IPF support was replaced by a clean-room decoder and the library
deleted — see the `ipf.cpp/h` row in §2.) **Correction (2026-07-13 vendor relocation
sweep):** `argparse.cpp/h` was misclassified here as third-party by name
alone. `git log --follow` + a header read show it is bespoke command-line
parsing authored directly in this repo on 2017-03-06/07 (a Caprice32-era
contributor, built on POSIX `getopt_long()`, tightly coupled to
`CapriceArgs`/`KONCPC_KEYS`/`video_plugin_list`) — there is no upstream
project, no separate license, nothing to vendor. It moves to §2 (host layer,
Caprice32-era code awaiting re-authoring) instead; see `vendor/README.md`'s
"What does NOT belong here" section for the full writeup.

## 1. Emulation core (replacements EXIST — deletion gated on the cutover)

| Inherited file(s) | Replacement | Deletion blocked by |
|---|---|---|
| `z80.cpp/h`, `z80_macros.h`, `z80daa.h` | `src/hw/z80` (FUSE 1356/1356, 1872-case timing sweep) | **DELETED 2026-07-13** (Wave-1 burn-down). z80_view.cpp owns the surviving surface: view struct, bp/wp/IO lists + shared fire predicates, hooks, and the tool-facing memory accessors (machine peeks; host bank tables when no machine — unit tests) |
| `crtc.cpp/h` | `src/hw/crtc` (types 0–3, deep counter quirks) | **DELETED 2026-07-13** (Wave-1 burn-down). crtc_types.h keeps the model/chip lookups; the bridge mirrors chip state into the t_CRTC view each debug_sync |
| `psg.cpp` | `src/hw/psg` (+ the sim/app stereo bridge; Digiblaster/AmDrum now mix in the Machine's analog stage via `src/hw/printer` + `src/hw/amdrum`, drive sounds via the FDC event overlay) | **DELETED 2026-07-13** (Wave-1 burn-down). The t_PSG view is bridge-mirrored; g_psg_scope moved to hw_views.cpp (its sub-cycle filler is an open item — the DevTools audio scope was already dark under engine=1) |
| `fdc.cpp` | `src/hw/fdc` (rotating medium, flux, event ring, WRITE/FORMAT §10 + AMSDOS SAVE" acid test) | **DELETED 2026-07-13** (Wave-1 burn-down). Drive LED/status read the Device via the bridge; the FDC status constants live in hw_views.h |
| `disk.cpp/h` | DSK parsing lives in `src/hw/fdc` (`fdc_attach_disk`) | **DELETED 2026-07-13** (Wave-1 burn-down). The t_drive/t_track/t_sector view structs (disc tools' parse target) moved to the authored hw_views.h/.cpp; the legacy write/format path is gone (the Device writes in place, the bridge flushes) |
| `video.cpp/h` | **DELETED 2026-07-13** (plan Phase B). CPC frame generation → `src/hw/video` (done earlier); host presentation → `src/video_host.cpp/h` — 85% of the old file was already re-authored in place across the 2026 GPU rewrite phases (blame: 2654/3134 lines), so the split moved the authored plumbing to its own file. The 2x scaling kernels in `src/scalers/cpc_scalers.*` were first kept as a vendored GPL-2.0 dependency (SMS Plus/SDL, MAME, Caprice32 lineage), then **RE-AUTHORED CLEAN-ROOM 2026-07-16** from the public algorithm descriptions (EPX/Scale2x, AdvMAME2x, Eagle, TV2x, bilinear, Catmull-Rom bicubic, LCD dot-matrix) with `test/scalers_test.cpp` — the GPL source was deleted, so no GPL/inherited code survives and the tree is fully first-party (bead qsm6-adjacent; unblocks the v6 clean-license publish) | — |
| `tape.cpp/h` | `src/hw/tape` (deck Device + line-in; firmware acid test: a synthesized firmware-format CDT loads via `RUN"` end to end, test/hw/tape_acid_test.cpp) | **DELETED 2026-07-13** (Wave-1 burn-down). CDT/VOC ingest re-authored in slotshandler (raw TZX body host-side; VOC via the clean voc_to_tzx); tape_scan_blocks (hw_views.cpp) walks with the deck's exported tape_cdt_block_len so UI ordinals cannot drift; eject reaches the deck (deferred bridge eject) |
| `asic.cpp/h` (Plus) | spec: `docs/hardware/asic-device.md`; `src/hw/asic` Device + GA/CRTC/video Plus mode | **DELETED 2026-07-13** (Wave-1 burn-down). The asic view struct lives in hw_views.h and is peek-backed on demand (subcycle_bridge_refresh_asic_view: sprites, DMA debug regs, lock FSM, register-page snapshot) for the DevTools window and the IPC `asic` dumps |
| `phazer.cpp/h` | **implemented** (`src/hw/light_gun.cpp`, Faithful tier when plugged) | **DELETED 2026-07-13** (Wave-1 burn-down). PhazerType (which gun is plugged — config surface) moved to the authored phazer_type.h |

## 2. Host layer (inherited shell — to be re-authored on subcycle::Machine)

| Inherited file(s) | Replacement path |
|---|---|
| `kon_cpc_ja.cpp`, `koncepcja.h` | **koncepcja.h RE-AUTHORED 2026-07-13** (Wave 3): documented header, constexpr constants (dead CRTC flag set deleted), plain structs; chip-view/t_SNA layouts kept (bridge mirrors + SNA format depend on them — their field lines still carry old blame by design). **kon_cpc_ja.cpp: blame-guided burn-down 2026-07-13** — deleted the ~470-line SDLK/scancode debug name maps (SDL_GetKeyName/GetScancodeName instead), ga_init_banking re-done as a constexpr 8×4 PAL bank-layout decode, GPL preamble/macros re-authored. **Second pass (Gate C §2 close-out, same day):** ROM load/patch re-authored (RAII stdio, soft-vs-hard failure contract stated, pfileObject scratch global deleted; FIXED: AMSDOS-headered ROMs could never load — wrong remainder after the header skip), config read remainder re-done as read_flag/read_clamped helpers (bit-0 and clamp semantics preserved), OSD print() collapsed from four hand-unrolled bpp copies to one depth-generic renderer (8-bit self-clobber + signed-char font-index UB fixed, GUI-verified pixel-exact drop shadow), reset/init/dump residue burned (value-init over memset, dumpScreen/dumpSnapshot share one path builder, printer fclose checked). **928/4680 inherited lines remain** (git blame -w -M -C, pre-2026 author-time): extern/global declarations, the colour/green-luma data tables, koncpc_main's init sequencing, SDL event-loop and menu-action case bodies, and single lines inside already-2026 functions — coherent live logic where further rewriting would be renaming, not re-authoring; burn down opportunistically as those surfaces change |
| `slotshandler.cpp/h` | **RE-AUTHORED 2026-07-13** (Wave 3): one dsk_parse/dsk_load_track pair replaces the twin standard/extended parsers, checked-write dsk_save, goto-less dsk_format, table-driven fillSlots, dedup'd engine mirroring in file_load (dead .scp branch dropped); fixed FILE leaks (snapshot_load path, zip-extracted media) and the pbGPBuffer header sniff |
| `keyboard.cpp/h` | **RE-AUTHORED 2026-07-13** (Wave 3): English base table + constexpr French/Spanish override lists (auditable deltas) built by a constexpr patcher; X-macro master key lists single-source the enums and the .map name tables; mechanical map spans generated by loops; string-based layout parser (no strtok/80-char buffer); 1782→976 lines |
| `configuration.cpp/h` | **RE-AUTHORED 2026-07-13** (Wave 3): string_view scanning INI parser (no strtok, no 256-byte line cap, CRLF-safe, blanks around '=' tolerated), shared override-then-config read path; on-disk format unchanged |
| `zip.cpp/h` | **DELETED 2026-07-13** (plan Phase C close-out): clean unzip authored from the PKWARE APPNOTE in `src/zip_archive.cpp/h` (same `zip::dir/extract` API, plus stored-entry and data-descriptor support the old inflate-only path lacked); no minizip dependency — zlib does the inflate |
| `cartridge.cpp/h` | **RE-AUTHORED 2026-07-13** (Wave 3): in-memory bounds-checked RIFF parser (chunk-walk arithmetic kept bit-for-bit); rejected files no longer leave a zeroed 512K image attached |
| `snapshot (in kon_cpc_ja)` | **RE-AUTHORED** (Wave 1 moved it, Gate C §2 closed it 2026-07-13): the SNA codecs live in slotshandler as `snapshot_load_machine`/`snapshot_save_machine`, written against the Machine — restore replays chip state through DMA-style `io_write` cycles (any engine implementing the bus contract restores identically), save reads Device peeks. The `t_SNA_header` layout in koncepcja.h stays byte-frozen on purpose (on-disk .sna compatibility); the last residue (memset'd headers) is now brace-init. Round-trip verified over IPC (save → corrupt RAM → load restores) |
| `stringutils`, `fileutils`, `errors.h`, `memutils.h`, `log.cpp/h` | **RE-AUTHORED 2026-07-13** (Wave 3): stringutils on std::string_view (safe case-insensitive compare), fileutils on std::filesystem (no dirent fork), errors.h documented/grouped, memutils scope_exit as a [[nodiscard]] move-only guard, log macros single-write (no interleaved lines across threads) |
| `font.h`, `rom_mods.h`, `glfunclist.h`, `types.h` | **RE-AUTHORED 2026-07-13** (Wave 3): font.h/rom_mods.h constexpr byte tables with documented layout + MAX_ROM_MODS owned by rom_mods.h; types.h pragma-once std:: aliases + width static_assert; **glfunclist.h DELETED** (orphaned since the GPU presentation rewrite) |
| `z80_disassembly.cpp/h` | **RE-AUTHORED 2026-07-13** (Gate C Wave 3): table-driven decoder reading the shared master opcode table (`z80_opcode_lookup`), the same single source of truth the assembler consumes. Removed the per-call `std::map` rebuild and the duplicated legacy-key path; explicit prefix decode (NONE/CB/ED/DD/FD/DDCB/FDCB) → O(1) lookup → single stack-buffer format pass. Fixed a latent DDCB/FDCB bug (displacement byte precedes the opcode byte; the old accumulator keyed on the wrong byte). `z80_instruction_length`/`z80_is_call_or_rst` now alloc-free for the DevTools stepper hot path. Parametrized golden test covers every prefix group + relative targets + length |
| `symfile` | **RE-AUTHORED 2026-07-13** (Wave 3): std::from_chars whole-field hex parsing (the old std::stol threw out of main on malformed files), per-kind error messages, SaveTo reports I/O failures, const-ref accessors |
| `ipf.cpp/h` | **capsimg DELETED — clean-room decoder is the sole IPF path (Gate C capstone, 2026-07-13, bead qsm6).** The vendored, non-commercial SPS Decoder Library (the 55-file `src/capsimg/**`) and its oracle test (`test/ipf_mirror_test.cpp`) are gone; the `HAS_CAPSIMG`/`KONCPC_HAS_CAPSIMG` build gate is removed from `makefile` and `CMakeLists.txt` (there is now only one build config). `src/ipf.cpp`'s `ipf_load` (both the filename and `FILE*` overloads) reads the image into a byte vector and calls straight into the clean-room `src/ipf_decode.{h,cpp}` (`ipf::Image::open` → `fill_drive`) to populate the legacy `t_drive` sector view (disc-tools sector/file editors, "Save Disk A" DSK export, the M4 board's directory reader); `dsk_eject`'s `delete[]` frees the `new[]` track buffers (no `eject_hook`). A CAPS-encoder (encoderType 1) IPF is DROPPED with a clear error and no fallback — re-image as SPS/SCP/HFE. The engine=1 flux path is unchanged in spirit: `flux::to_scp` runs `ipf::Image::mirror_side0(3)` → `scp_from_mfm_tracks` into an in-memory SCP for `insert_flux`; the old `ipf_mirror_to_scp` capsimg mirror is deleted, `scp_from_mfm_tracks` + the `t_mfm_track`/`t_mfm_rev` types stay (decoder-independent, also fed by hfe/mfm_encode). The clean-room core (landed 2026-07-13, beads-14oq/cbxg/6tna, built ONLY from `docs/hardware/ipf-format.md`): container parser (CAPS/INFO/IMGE/DATA, big-endian, 12-byte header + CRC-32/ISO-HDLC, bounds-checked/hostile-input-safe, IMGE↔DATA `dataKey` match, CTEI/CTEX/unknown skip), SPS stream-element decoder (Sync/Data/Gap/Raw/Fuzzy; GapLength/SampleLength; all four gap-fill modes), MFM reconstruction into `t_mfm_rev`, and the §5.4-fixed `t_drive` fill (zero-based `sides = maxSide`; genuine `DSK_TRACKMAX`/`DSK_SIDEMAX` bounds check). Tests: `test/ipf_decode_test.cpp` (§7 Theme Park Mystery accounting vectors, §6 CRC self-check, hostile/truncated/bad-CRC/`RejectsCapsEncoder`/bad-geometry rejection, minimal-IPF round-trip, fuzzy variance) + the `flux::to_scp` end-to-end dispatcher test. Deferred per spec §8.2: CAPS encoder (rejected), densities 3–9 (uniform cells + warning), Noise density |
| `savepng` | **RE-AUTHORED 2026-07-13** (Gate C §2): clean C++17 PNG writer over libpng (libpng stays a declared dependency — no hand-rolled deflate). One RGBA32-normalised encode path replaces the driedfruit-derived four-branch wrapper; png_struct/png_info + surfaces RAII-owned (no error-path leaks), longjmp target frame holds only trivially-destructible locals, per-chunk write + close/flush results checked (full disk detected). Fixed the inherited null-check-after-convert bug and the degenerate `pal = pal` branch. Same public API (`SDL_SavePNG`), same output (8-bit RGBA, non-interlaced) |
| `argparse.cpp/h` | **RE-AUTHORED 2026-07-13** (Wave 3): self-contained C++17 table-driven parser (no getopt_long, no optind global), full CLI parity incl. clustering/attached values/long-prefix matching/`--`; fixes the uninitialized binOffset default and the throwing -o parse |
| `portable-file-dialogs.h` | **RELOCATED 2026-07-13** → `vendor/portable-file-dialogs/portable-file-dialogs.h` |
| `msf_gif.h` | **RELOCATED 2026-07-13** → `vendor/msf_gif/msf_gif.h` |
| `TextEditor.h`, `TextEditor.cpp`, `LanguageDefinitions.cpp` | **RELOCATED 2026-07-13** → `vendor/ImGuiColorTextEdit/` |

## 3. Already ours (2026) — no deletion obligation

IPC server, telnet console, ImGui UI + DevTools windows, autotype,
recorders (WAV/YM/AVI/GIF), trace, expr parser, disk editors, M4 board,
Symbiface, silicon disc, assembler, plotter, `src/hw/*`, `src/subcycle/*`,
`sim/*`, tests, docs.

These are authored here, so the "no inherited file survives" doctrine does
not require deleting them. Several PERIPHERALS in this list are nonetheless
being re-expressed as pin-level `src/hw` Devices — not to replace bad code
(these are clean), but so engine=1 has parity and the specs become
language-neutral. Done so far: tape, printer/Digiblaster, AmDrum, Multiface
II, AMX mouse, SmartWatch, Symbiface II; the `src/hw` Device is the
behavioural referee ("golden master" in these commits means reference-for-
behaviour, NOT code-to-be-ashamed-of). The ours-original keeps serving
engine=0 and is retired only when the Wave-3 host cutover removes engine=0
— a supersession, not a de-Caprice deletion. (`asic.cpp`/Plus IS Caprice32
inheritance — that one is a real §1 replace-and-delete.)

## Deletion order (matches the Milestone B beads)

1. **Wave 1 — the core** (beads-4q7r): **DONE 2026-07-13.** `z80.*`,
   `z80_macros.h`, `z80daa.h`, `crtc.*`, `psg.cpp`, `fdc.cpp`, `disk.*`,
   `tape.*`, `asic.*` and `phazer.*` are deleted; engine=1 is the only
   engine ([system] engine stays parseable — anything else logs and runs
   the board). Survivor surfaces: z80_view (debug model + accessors),
   hw_views.h/.cpp (drive/ASIC/tape views + TZX scanner), phazer_type.h,
   crtc_types. The legacy halves of `video.*` died earlier (plan Phase B).
   **Audit closed 2026-07-13** (plan 2026-07-13-001 Phase A): every extern
   `t_CRTC/t_GateArray/t_PSG/t_FDC/t_drive/t_z80regs` consumer outside the
   deletion-list files is peek-backed — chip-state mirrors + FDC mechanics
   refresh each debug_sync BEFORE the probe filter (so conditional bp/wp
   evaluate parked-machine truth; expr reads only CRTC regs + PSG regs, both
   synced), telnet TXT/BDOS mirrors ride machine taps (e2e-verified both
   engines), disc-tools `t_drive` serving is tagged dies-with-Wave-1.
   Remaining before the deletions: the media-ingest + video-host items
   (plan Phases B/C) that keep engine=0 as the fallback.
2. **Wave 2 — remaining devices**: `src/hw/tape` (+delete `tape.*`),
   `src/hw/asic` for Plus (+delete `asic.*`), phazer via beam position.
3. **Wave 3 — the shell**: new host loop, media manager, input mapper,
   presentation layer (+delete `kon_cpc_ja.cpp`/`koncepcja.h`/`slotshandler.*`/
   `keyboard.*`/`video.*` host half), utilities die with their last callers.
4. **Continuous**: third-party files out of `src/` into `vendor/`.
   `msf_gif.h`, `TextEditor.{h,cpp}`, `LanguageDefinitions.cpp` and
   `portable-file-dialogs.h` **RELOCATED 2026-07-13** (the non-commercial SPS
   Decoder Library, once vendored under `src/capsimg/`, was NOT relocated — it
   was replaced by a clean-room IPF decoder and deleted, Gate C capstone; see
   the `ipf.cpp/h` row in §2 and `vendor/README.md`; `argparse.cpp/h` turned
   out not to be third-party at all and moved to the §2 list instead).

**Before merge, the branch history is REWRITTEN**: restructured and
documented as building the entire emulator from scratch — spec by spec,
device by device — so the history itself reads as the construction log of
the new emulator, not the archaeology of the transition (tracked in bd).

**PR #146 carries the whole arc:** the replacements' foundation (the board,
the machine, the bridge) is in; the deletion waves land **on this same PR** —
Wave 1 retires the core files here, not in a follow-up. No inherited file is
feature-for-feature superseded until Wave 1 completes, and the PR is not
"done" by this doctrine until the ledger's Wave-1 row deletions are in its
diff.
