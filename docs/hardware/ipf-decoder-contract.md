# IPF/CT-RAW/KryoFlux-STREAM decoder — clean-room replacement contract

Spec for what a clean-room decoder must provide to replace `src/capsimg` (the
non-commercially-licensed SPS Decoder Library) with **zero changes** to
`src/slotshandler.cpp` or `src/subcycle_bridge.cpp`. Traced from our own glue
(`src/ipf.cpp` / `src/ipf.h`) only — this is what *we* call, not the full
capsimg surface. Companion to
[`flux-media.md`](flux-media.md) (the SCP container/PLL spec the sub-cycle FDC
already consumes) and [`fdc-device.md`](fdc-device.md).

---

## 1. The minimal decoder contract

Everything upstream (`slotshandler.cpp`, `subcycle_bridge.cpp`) calls exactly
four free functions, declared in `src/ipf.h`. A drop-in replacement must keep
these signatures, return conventions, and side effects **identical** — only
the implementation inside `ipf.cpp` (and its use of `CapsLib.h`) changes.

```cpp
int ipf_load(FILE*, t_drive*);                 // temp-file wrapper, see 1.1
int ipf_load(const std::string& filename, t_drive*);  // the real loader

std::vector<uint8_t> scp_from_mfm_tracks(const std::vector<t_mfm_track>&);
std::vector<uint8_t> ipf_mirror_to_scp(t_drive* drive);
```

### 1.1 `ipf_load(FILE* pfileIn, t_drive* drive) -> int`

- Copies `pfileIn` to a temp file (`/tmp/.koncpc_tmp_XXXXXX` or `./` fallback
  via `mkstemp`, never deleted — a pre-existing TODO, not new scope) then
  calls the filename overload. **A clean-room decoder does not need this
  detour** — it exists only because capsimg's `CAPSLockImage` needs a path, not
  a buffer (capsimg does have `CAPSLockImageMemory`, unused by us). A
  from-buffer decoder can skip the temp file entirely and read `pfileIn`
  directly (or slurp it and go straight to an in-memory decode) — this
  overload's *external* contract is just "int, 0 on success" with `drive`
  filled the same way as the filename overload.
- Return: `0` on success, `ERR_DSK_INVALID` (22) on any I/O failure.

### 1.2 `ipf_load(const std::string& filename, t_drive* drive) -> int`

Inputs: a filesystem path. Behavior required:

1. `dsk_eject(drive)` first (releases any prior medium — must still call
   whatever eject/cleanup hook the *previous* medium had, generic contract of
   `dsk_eject`, not IPF-specific).
2. Open the file; **today's magic gate**: reject unless the first 4 bytes are
   `"CAPS"` (the IPF container signature) — see §4, this means CT-RAW/KryoFlux
   inputs never reach the decoder today regardless of a clean-room
   implementation's own capability.
3. Decode every track (`mincylinder..maxcylinder` × `minhead..maxhead`) into
   `drive->track[cyl][head]` — see §2 for the exact `t_track`/`t_sector`
   fields required.
4. Fill `drive->tracks`, `drive->sides`, `drive->altered = false`,
   `drive->eject_hook`, `drive->ipf_id`.
5. Return `0` on success, `ERR_DSK_INVALID` (22) on any failure (bad header,
   library init failure, lock failure, per-track lock failure — current code
   aborts the *whole load* on a single track-lock failure, mid-way through;
   a replacement may keep that behavior or do better, but must not partially
   fill `drive` and return `0`).

Output — `t_drive` fields filled (host view, feeds disc tools / DSK export /
M4 board directory reads / the legacy engine=0 FDC path):

- `drive->tracks = maxcylinder + 1`, `drive->sides = maxhead` (**note**: this
  is `maxhead`, not `maxhead + 1` — head count off-by-one exists in the
  current code; a drop-in replacement should reproduce it unless deliberately
  fixing it as a separate, called-out change).
- Per `(cyl, head)`: a fully decoded `t_track` (§2) or a zeroed one
  (`memset(pt, 0, sizeof(*pt))`) when the CAPS track has no data
  (`cti.tracklen == 0`, i.e. unformatted).
- `drive->eject_hook` — a function pointer whose **identity** (not just
  behavior) other code depends on: `ipf_mirror_to_scp` gates on
  `drive->eject_hook == ipf_eject_hook` to recognize "this drive holds a CAPS
  medium" (see §1.4). The replacement's own eject-hook function must be used
  the same way — any drive with a non-null, non-IPF eject hook is *not* CAPS
  media as far as `ipf_mirror_to_scp` is concerned.
- `drive->ipf_id` — an opaque `long` handle, meaningful only to the eject
  hook and to `ipf_mirror_to_scp`'s track re-lock loop. A clean-room decoder
  is free to make this an index into its own open-image table, a pointer cast
  to `long`, etc. — nothing outside `ipf.cpp` interprets its value.

### 1.3 `scp_from_mfm_tracks(const std::vector<t_mfm_track>&) -> std::vector<uint8_t>`

Pure function, **decoder-independent** — already clean-room, do not touch.
Converts an in-memory MFM-bitcell capture (per-cylinder, per-revolution) into
an SCP byte container. See §2.3. Kept as-is; the new decoder's job is only to
*produce* `t_mfm_track` data that feeds this function (via `ipf_mirror_to_scp`)
— it does not need its own SCP writer.

### 1.4 `ipf_mirror_to_scp(t_drive* drive) -> std::vector<uint8_t>`

This is the engine=1 (sub-cycle FDC) flux path. Inputs: a `t_drive*` that was
just filled by `ipf_load` (still "locked" — i.e. the decoder's underlying
image handle in `drive->ipf_id` must still be valid/open; `ipf_load` does
**not** close/unlock the image after decoding into `t_drive`, by design — the
image stays open for exactly this re-read).

Contract:

1. Guard: return `{}` immediately unless
   `drive->eject_hook == ipf_eject_hook && drive->tracks != 0` — i.e. this
   isn't CAPS media.
2. Side-0-only: SCP flux is side-0-only (comment logged if `drive->sides > 0`
   — side 1 stays on the legacy `t_drive`/`dsk_eject` decode path, unaffected).
3. For each cylinder (capped at `kScpMaxCyls = 84`), lock+decode
   `kMirrorRevs = 3` revolutions of raw MFM bitcells from the image (side 0
   only), each revolution a **fresh decode pass** — for tracks marked
   "flakey" the three passes must differ (weak-bit variability, matching real
   copy-protected media); for stable tracks all three passes are identical.
   Concretely (mirrors the current capsimg-flag semantics, §3 — the
   replacement decoder does not have to reproduce these exact flags, only
   the *effect*: N independent bitcell reads per track, where flakey tracks
   vary and stable tracks don't):
   - Re-locking the SAME track index (not unlock/relock) advances a per-image
     weak-data RNG; unlocking then relocking *resets* it. `ipf_mirror_to_scp`
     deliberately never calls the track-unlock op between its 3 revolutions —
     a replacement must preserve "3 independent reads, RNG advances each
     time" semantics if it wants matching weak-bit statistics, but exact
     RNG behavior is not load-bearing for correctness, only for realism.
   - A track that goes unformatted mid-capture (`bits == 0` or a null buffer)
     stops early and pads by repeating the last-captured revolution
     (`cyls[cyl].push_back(cyls[cyl].back())`) until `kMirrorRevs` is reached.
   - A **non-flakey** track's first read is repeated verbatim for the
     remaining revolutions (skip decoding twice more — an optimization, not
     a correctness requirement).
4. Each revolution becomes a `t_mfm_rev { bits, nbits }` (§2.2) and all
   cylinders' revolution-lists feed `scp_from_mfm_tracks`.
5. Returns `{}` (empty vector) on any geometry mismatch (mismatched
   revolution counts across cylinders, > 255 revolutions, > `kScpMaxCyls`
   cylinders) or on any track-lock failure.

---

## 2. Data structures the decoder must fill

### 2.1 `t_drive` / `t_track` / `t_sector` (legacy/host view — `src/hw_views.h`)

Used by: disc tools, DSK export, M4 board's virtual-SD directory reader, and
(when engine=0 / side 1) the legacy FDC read path.

```cpp
struct t_drive {
  unsigned int tracks;             // = maxcylinder + 1
  unsigned int current_track;      // NOT touched by ipf_load (head position)
  unsigned int sides;               // = maxhead (see §1.2 off-by-one note)
  unsigned int current_side, current_sector;  // not touched
  bool altered;                    // = false after load
  unsigned int write_protected;    // not touched by ipf_load (stays 0)
  unsigned int random_DEs, flipped; // not touched
  long ipf_id;                     // opaque handle, see §1.2
  void (*eject_hook)(t_drive*);    // = &ipf_eject_hook (identity matters, §1.4)
  t_track track[102][2];           // DSK_TRACKMAX=102, DSK_SIDEMAX=2 —
                                    // ipf_load does NOT bounds-check
                                    // maxcylinder/maxhead against these before
                                    // indexing; an image with >102 cylinders
                                    // or >2 heads is an out-of-bounds write in
                                    // the CURRENT code. A replacement should
                                    // at minimum preserve this bound (reject
                                    // or clamp) rather than silently corrupt.
};

typedef struct {
  unsigned int sectors;   // sectors found on this track (<= DSK_SECTORMAX=29)
  unsigned int size;      // = total decoded bytes for the track (uDecoded)
  unsigned char* data;    // owned buffer, `new byte[size]`; freed by
                           // `delete[] track.data` in dsk_eject — the
                           // decoder must heap-allocate with `new[]`, not
                           // malloc/mmap, or the free will UB.
  t_sector sector[29];
} t_track;

class t_sector {
  unsigned char CHRN[4];   // C, H, R, N as recorded in the ID field
  unsigned char flags[4];  // ST1/ST2-style DSK convention:
                           //   flags[0] |= 0x20  data CRC error
                           //   flags[1] |= 0x20  data CRC error (mirrored)
                           //   flags[1] |= 0x40  control mark (deleted DAM)
                           //   flags[1] &= ~0x01 "no data" (header w/o data)
  // setData(ptr): pointer into the track's `data` buffer (offset, not a copy)
  // setSizes(size, total_size): size = one version's bytes, total_size =
  //   all versions concatenated. IPF's legacy decode always passes
  //   size==total_size (weak_versions_ == 1) — the host view carries NO
  //   multi-version weak-bit rotation; only the flux/SCP path (§2.2-2.3)
  //   gets real weak-bit variability. Reproduce this (single version) unless
  //   deliberately extending the legacy path too.
};
```

Track decode requirements (System-34/IBM MFM, matches
[`flux-media.md`](flux-media.md) §3 exactly — same address-mark scanning
rules apply to a from-scratch IPF decoder's *sector extraction*, since IPF
tracks decode to the same bitcell-level MFM structure):

- Byte-align on 3× `A1` with missing clock (raw 16-bit pattern `0x4489`),
  read the address-mark byte: `0xFE` = ID field (`C H R N` + CRC16-CCITT,
  poly `0x1021`, init varies by convention used — current code seeds
  `s_wCRC = 0xcdb4`, the precomputed CRC of `A1 A1 A1` under init `0xFFFF`,
  then continues from there); `0xFB`/`0xFA`/`0xF8`/`0xF9` = data field
  (`0xF8`/`0xF9` set the control-mark flag).
- Sector size = `128 << N` for `N <= 7`, else `0x8000`.
  header CRC failure discards the sector attempt entirely (no partial
  sector recorded); data CRC failure keeps the sector but flags it.
- Data field is only accepted 32–63 MFM-byte-times after its header (else
  treated as "no data").
- Track wrap handling: the scan must continue past the physical end of the
  track buffer back to its start (`fWrapped`) to catch sectors that straddle
  the index, and keep going until a sector started before the wrap point
  completes.
- First sector on the track (read-track protections): if there's exactly one
  sector and it's under 4096 bytes, overread (CRC through) extra bytes up to
  4096 total.

### 2.2 `t_mfm_track` / `t_mfm_rev` (flux mirror source — `src/ipf.h`)

```cpp
struct t_mfm_rev {
  std::vector<uint8_t> bits;  // MFM bitcells, MSB-first packed
  uint32_t nbits = 0;          // valid bit count (bits.size() == ceil(nbits/8))
};
using t_mfm_track = std::vector<t_mfm_rev>;  // one entry per captured revolution
```

- `bits[i>>3] & (0x80 >> (i&7))` set = a flux transition at bitcell `i`, one
  cell = **2 µs nominal** (double-density MFM, 250 kbit/s data rate = 500
  kbit half-cells). This is a *transition-per-1-bit* encoding — no separate
  "0/1 data value" concept, only "did a flux reversal happen in this cell".
- All revolutions for a cylinder must share the same `nbits`... actually no:
  `nbits` may differ per revolution (real drives have slightly different
  per-revolution timing) — what must match across **cylinders** feeding one
  `scp_from_mfm_tracks` call is the **revolution count** (`t_mfm_track::size()`
  is the same for every non-empty cylinder in the vector — enforced, see §2.3).
- An empty `t_mfm_track` (zero revolutions) = an absent/unformatted cylinder
  slot in the resulting SCP.

Where this comes from in the current pipeline: capsimg's `CAPSLockTrack` with
`DI_LOCK_TRKBIT` returns `cti.trackbuf`/`cti.tracklen` already **as bits**
(raw MFM bitcell stream, clock+data bits included — this is the *encoded*
MFM, not the decoded data bytes `ReadTrack` extracts). Without
`DI_LOCK_TRKBIT` (old library), `tracklen` is bytes and must be `<<3`'d. A
clean-room IPF decoder must expose, per track/per revolution, exactly this:
**the raw MFM bitcell stream as originally recorded on the disc**, not a
resynthesized one — this is what makes weak bits and copy-protection timing
survive the mirror.

### 2.3 SCP container emitted by `scp_from_mfm_tracks` (unchanged, reference only)

Full byte-level spec already lives in [`flux-media.md`](flux-media.md) §1 and
is exactly what `scp_from_mfm_tracks` (untouched, already clean-room) emits:
16-byte header + 168×4-byte track-offset table (`slot = cylinder*2` for side
0, side-1 slots left absent) + per-track `"TRK"` header + 12-byte-per-revolution
table (duration ticks / word count / data offset) + big-endian 16-bit flux
words (25 ns tick, `kTicksPerCell = 80` per 2 µs MFM cell, `0x0000` = 65536-tick
carry). Revolutions per track: `kMirrorRevs = 3` (hardcoded in
`ipf_mirror_to_scp`, unrelated to whatever revolution count the source IPF
naturally has — the decoder is asked for 3 independent reads regardless).
Weak/unformatted handling: an absent `t_mfm_track` → absent SCP slot
(`offset = 0` in the table) → the FDC side (`hw/flux.cpp`,
`flux_decode_track_rev`) treats it as "count = 0, a head over nothing reads
nothing". Weak bits are NOT flagged out-of-band anywhere in the SCP — they
are *implicit*: revolution N's bitcell stream simply differs from revolution
M's wherever the source medium was physically flakey, and the FDC serves
`(rotation count) mod (captured revolutions)` on each read, so different
passes surface different bytes exactly like real hardware (`fdc-device.md`
Stage 3 integration, and `flux-media.md` §7).

---

## 3. capsimg API → what we actually extract

| CAPS call | Where | What we pull from it, and nothing else |
|---|---|---|
| `CAPSGetVersionInfo(&vi, 0)` | `ipf_load` | `vi.release >= 4` (library-version gate — **not applicable** to a clean-room replacement, drop it) and `vi.flag & (DI_LOCK_OVLBIT\|DI_LOCK_TRKBIT)` to decide whether `tracklen` is bits or bytes on lock calls. A clean-room decoder should just always report track lengths in **bits** (§2.2) and skip this negotiation entirely. |
| `CAPSInit()` | `ipf_load` | Library-global init; call-once semantics. A clean-room decoder likely needs no process-global init, or a trivial one. |
| `CAPSAddImage()` | `ipf_load` | Returns an opaque `long id` — becomes `drive->ipf_id`. Nothing else. |
| `CAPSLockImage(id, filename)` | `ipf_load` | Opens+parses the file at `filename`, associates it with `id`. Failure ⇒ `ERR_DSK_INVALID`. |
| `CAPSGetImageInfo(&cii, id)` | `ipf_load` | Exactly 4 fields: `cii.mincylinder`, `cii.maxcylinder`, `cii.minhead`, `cii.maxhead` — the geometry loop bounds. (`cii.type`, `cii.release`, `cii.crdt`, `cii.platform[]` are all read by the struct but **never used** by our code — do not bother reproducing them.) |
| `CAPSLockTrack(&cti, id, cyl, head, flags)` | `ipf_load` (once per track, `flags = DI_LOCK_UPDATEFD\|DI_LOCK_TYPE[\|DI_LOCK_OVLBIT][\|DI_LOCK_TRKBIT]`) and `ipf_mirror_to_scp` (3× per cylinder, head fixed at 0) | `cti.tracklen` (bit or byte count of the raw MFM stream — see §2.2), `cti.trackbuf` (pointer to the raw MFM bitcell/byte stream — **CAPS-library-owned**, our code never frees it, only reads through it before the next lock/unlock), `cti.type & CTIT_FLAG_FLAKEY` (whether this track has weak/random data — drives the multi-revolution re-lock behavior in `ipf_mirror_to_scp`, §1.4). `sectorcnt`/`sectorsize`/`timelen`/`timebuf`/`overlap` in `CapsTrackInfoT1` are populated by capsimg but **never read** by our code. |
| `CAPSUnlockTrack(id, cyl, head)` | `ipf_load` (after every track decode) | No return value used; releases `cti.trackbuf`. **Note**: `ipf_mirror_to_scp` deliberately does *not* call this between its 3 revolutions per cylinder (§1.4) — only `ipf_load`'s track-decode loop unlocks per track. |
| `CAPSUnlockImage(id)` | `ipf_eject_hook` | Releases the whole locked image; no return value used. |
| `CAPSRemImage(id)` | `ipf_eject_hook` | Frees the image container `id`. |
| `CAPSExit()` | `ipf_eject_hook` | Global library teardown paired with `CAPSInit()`. |

Everything else in `CapsLib.h` (`CAPSLockImageMemory`, `CAPSLoadImage`,
`CAPSGetPlatformName`, `CAPSFdcGetInfo`/`Init`/`Reset`/`Emulate`/`Read`/`Write`/
`InvalidateTrack` — the CAPS-FDC-emulation half of the API, unrelated to
image decoding — `CAPSFormatDataToMFM`, `CAPSGetInfo`, `CAPSSetRevolution`,
`CAPSGetImageType(Memory)`, `CAPSGetDebugRequest`) is **not called anywhere**
in this codebase. A clean-room decoder does not need to implement any of it.

---

## 4. Extensions & dispatch

- **`.ipf`** and **`.raw`** both route to `ipf_load` for drive A and drive B
  (`src/slotshandler.cpp` `files_loader_list`, lines ~64–86) and both appear
  in `fillSlots`'s drive-A/B extension list (`.dsk.ipf.raw`) and the zip
  classifier's extension list (`.dsk.sna.cdt.voc.cpr.ipf.raw`).
- **Important finding**: `ipf_load(const std::string&, t_drive*)` gates on a
  literal 4-byte `"CAPS"` magic at file offset 0 before doing anything else
  (`ipf.cpp` line 353). That magic is the **IPF container signature**
  specifically — CT-RAW and KryoFlux STREAM files (the other two formats
  capsimg's own internal `CAPSGetImageType`/`CAPSGetImageTypeMemory` can
  auto-detect, per `citCTRaw`/`citKFStream`/`citKFStreamCue` in
  `src/capsimg/LibIPF/CapsAPI.h`) do **not** start with `"CAPS"` and are
  rejected by our gate before `CAPSLockImage` — which itself *could* have
  auto-detected them — is ever called. **In practice, only genuine IPF files
  are ever decoded by this codebase today**, despite `.raw` being wired to
  the same loader and despite capsimg's own multi-format support. A
  clean-room replacement therefore only strictly needs to decode the **IPF
  container format** to be a correct drop-in; CT-RAW/KryoFlux-STREAM support
  would be new functionality, not parity, and would additionally require
  relaxing or extending `ipf_load`'s magic-byte gate (and probably renaming
  `.raw`'s association, since "CT Raw" traditionally uses `.raw`/`.ctr`
  extensions that are NOT IPF-signed).
- The engine=1 (sub-cycle) side dispatches purely on **filename extension**,
  independent of the magic-byte gate above: `subcycle_bridge.cpp` line 257
  (`ends_with(CPC.driveA.file, ".ipf") || ends_with(..., ".raw")`) decides
  whether to call `ipf_mirror_to_scp` vs. treat the file as a raw DSK/SCP
  buffer; `slotshandler.cpp` line 1024 does the same for the runtime hot-swap
  path (`extension == ".ipf" || extension == ".raw"`). Both call sites are
  downstream of `ipf_load` having already succeeded (so the magic gate has
  already run) and both are **drive-A-only** — a `.ipf`/`.raw` in drive B
  logs and stays on the legacy per-`t_drive` decode, no flux mirror
  (`slotshandler.cpp` line ~1036).
- `.dsk`, `.sna`, `.cdt`, `.voc`, `.cpr` are unrelated loaders (`dsk_load`,
  `snapshot_load`, `tape_insert`, `cartridge_load`) — out of scope here.

---

## 5. Test/oracle inventory

- **`test/ipf_mirror_test.cpp`** — five always-run unit tests exercise
  `scp_from_mfm_tracks` directly with synthetic `t_mfm_track` data (no CAPS
  involved at all): exact byte-level SCP encoding (`ScpContainerFromMfmBitsIsExact`),
  the 65536-tick carry/overflow-word encoding (`LongGapsUseOverflowWords`),
  absent-cylinder handling (`AbsentCylindersBecomeEmptySlots`), mismatched
  revolution-count rejection (`MismatchedRevolutionCountsAreRejected`), and
  empty-input edge cases including `ipf_mirror_to_scp(nullptr)` and a
  freshly-zeroed `t_drive` with no eject hook (`EmptyInputsYieldEmpty`).
  These are **decoder-independent** — they test `scp_from_mfm_tracks` only
  and will keep passing unmodified after a decoder swap.
- **`IpfMirror.MirrorsARealCapsImageWhenProvided`** (same file) — the one
  test that actually exercises a decoder end-to-end: `ipf_load` a real `.ipf`
  file, `ipf_mirror_to_scp` it, assert the SCP probes valid
  (`flux_scp_probe`), has exactly 3 revolutions (`kMirrorRevs`) and
  `drive.tracks` cylinders, and that `flux_scp_to_dsk` (the independent
  `hw/flux.cpp` PLL decoder) finds real sectors in the mirrored MFM (`n >
  0x100`). **Gated / skips by default**: looks for `$KONCEPCJA_REAL_IPF`, then
  `test/hw/fixtures/real.ipf`, then `../test/hw/fixtures/real.ipf`; none are
  committed to the repo (no fixture exists anywhere under `test/hw/fixtures`
  today — confirmed empty), so this test **always SKIPs in CI** and only runs
  when a developer supplies a real (possibly non-redistributable) IPF
  out-of-band.
- **`FdcFlux.DecodesARealScpCaptureWhenProvided`** (`test/hw/fdc_test.cpp`,
  line ~1008) — decoder-independent (operates on a raw `.scp`/`.a2r` fixture,
  not IPF), but is the natural end-to-end oracle for whatever SCP a new IPF
  decoder's `ipf_mirror_to_scp` output would also need to satisfy. Same
  gating pattern (`$KONCEPCJA_REAL_SCP`, `test/hw/fixtures/real.scp`, `.a2r`
  transcoded via the already-clean-room `a2r_to_scp`), also skips
  unconditionally today (no fixture committed).
- **Validation plan for a replacement decoder**:
  1. Keep the 5 synthetic `scp_from_mfm_tracks` tests passing unmodified
     (they don't touch the decoder).
  2. Build **both** the old (capsimg) and new decoder behind a compile-time
     switch (§6) during the transition; run `ipf_load` + `ipf_mirror_to_scp`
     against the same real `.ipf` fixture(s) with each, and diff: `t_drive`
     track/sector CHRN+flags+data byte-for-byte (legacy path), and
     `ipf_mirror_to_scp` output SCP byte-for-byte OR (if flux word encoding
     legitimately differs in inconsequential ways) at least
     `flux_scp_to_dsk`-decoded-sector byte-for-byte. This makes capsimg a
     **local, non-shipped test oracle** — exactly the ask in the task: never
     linked into the shipping binary, only pulled in for an opt-in
     comparison test/tool gated the same way the real-fixture tests already
     are (env var or `test/hw/fixtures/`, never committed media).
  3. Since no IPF/SCP fixtures are committed to this repo at all right now,
     the replacement work should also either (a) synthesize a minimal
     hand-built IPF file (mirroring how `ipf_mirror_to_scp`'s unit tests
     hand-build `t_mfm_track` data) to get an always-on CI check of the new
     decoder's IPF parsing, or (b) accept that IPF-decoder correctness stays
     gated on an out-of-band fixture like today, and rely on manual/local
     verification before merging.

---

## 6. `HAS_CAPSIMG` gating sketch (stopgap: build without capsimg today)

Goal: make `src/capsimg` **optional** at build time so a v6 publish can ship
without the non-commercial-licensed SPS library, while `ipf.cpp`'s public
functions keep existing (returning "unsupported" gracefully) so callers need
no `#ifdef`s of their own. Sites needing a guard:

1. **`makefile`** (the actual build-source-discovery, confirmed via
   `SOURCES:=$(shell find $(SRCDIR) -name \*.cpp)` at line 189 — this glob
   currently sweeps up **every** `.cpp` under `src/capsimg/` unconditionally;
   `VENDORED_EXCLUDE` only excludes it from `clang-format`/`clang-tidy`, not
   from compilation). Needed: a `KONCPC_HAS_CAPSIMG ?= 1` variable mirroring
   the existing `KONCPC_MODERN_UI` pattern (lines ~213–230) that, when 0:
   - Filters `src/capsimg/%` out of `SOURCES` (same `filter-out` idiom
     already used for `MODERN_UI_SOURCES`).
   - Drops `$(CAPS_INCLUDES)` from `IPATHS` (line 97).
   - Does **not** define `HAS_CAPSIMG` in `COMMON_CFLAGS` (so `ipf.cpp`
     compiles its stub branch, §6.4).
   - `CMakeLists.txt` needs the mirror flag per the existing convention noted
     at makefile line 213 ("Mirror in CMakeLists.txt
     (KONCPC_BUILD_MODERN_UI)") — check for an equivalent capsimg toggle
     there and add one if absent.
2. **`src/ipf.cpp`** — the file `#include "CapsLib.h"` (line 14) and every
   function body from `ipf_eject_hook` through `ipf_mirror_to_scp` that
   touches `CapsTrackInfoT1`, `CapsImageInfo`, `CapsVersionInfo`, or any
   `CAPS*` call needs to be inside `#ifdef HAS_CAPSIMG` / `#else` /`#endif`.
   The `#else` branch must still define:
   - `ipf_eject_hook` (a no-op stub — nothing to unlock) so `t_drive`'s
     function-pointer identity check in `ipf_mirror_to_scp` keeps compiling
     and keeps working (a drive can never *acquire* this hook if
     `ipf_load` always fails in the stub build, so the check is moot but
     must still link).
   - `ipf_load(FILE*, t_drive*)` and `ipf_load(const std::string&, t_drive*)`
     — return `ERR_DSK_INVALID` immediately (optionally `LOG_ERROR("IPF
     support not built in this binary")`), touch nothing in `drive`.
   - `ipf_mirror_to_scp` — the existing guard
     (`drive->eject_hook != ipf_eject_hook`) already returns `{}` for any
     drive that was never IPF-loaded, which is now *always* true in a stub
     build, so **this function needs no `#ifdef` at all** — it degrades
     correctly for free as long as `ipf_eject_hook`'s stub exists and
     `ipf_load` never sets `drive->eject_hook` to it.
   - `scp_from_mfm_tracks` needs **no guard** — it's already
     decoder-independent (§1.3), keep it compiled unconditionally either way.
3. **`src/slotshandler.cpp`** — no `#ifdef` needed. `ipf_load` keeps its
   signature in the stub build (§6.2) so the `files_loader_list` entries for
   `.ipf`/`.raw` (lines 64–86) keep compiling and now simply always return
   `ERR_DSK_INVALID` at runtime — acceptable stopgap behavior ("format not
   supported in this build" via the existing error path, no new UI needed
   immediately, though a nicer user-facing message is a follow-up).
4. **`src/subcycle_bridge.cpp`** — no `#ifdef` needed for the same reason;
   `ipf_mirror_to_scp(&driveA)` (line 259) just returns `{}` in a stub build,
   which the existing `b.media.empty()` check (line 261) already treats as a
   load failure with an `LOG_ERROR("cannot attach ...")`.
5. **`test/ipf_mirror_test.cpp`** — the 5 synthetic `scp_from_mfm_tracks`
   tests need no guard (§1.3, decoder-independent). Only
   `IpfMirror.MirrorsARealCapsImageWhenProvided` calls `ipf_load` on a real
   file and asserts success (`ASSERT_EQ(ipf_load(path, &drive), 0)`) — in a
   `HAS_CAPSIMG=0` build this test needs its own
   `#ifndef HAS_CAPSIMG` / `GTEST_SKIP()` guard (or equivalently, since it
   already skips whenever no fixture is present, it degrades safely as long
   as CI never sets `KONCEPCJA_REAL_IPF` in a capsimg-less build — but an
   explicit compile-time skip is more robust than relying on that).
6. **License/attribution files** (`src/capsimg/LICENCE.txt`,
   `docs/replacement-ledger.md`'s capsimg entry) are unaffected by the
   `#ifdef` — this is a build-time toggle, not a source deletion; capsimg
   stays in the tree (and in git history) as the local test oracle (§5) even
   after a clean-room decoder ships as the default.

None of `src/hw/flux.h`/`flux.cpp` (the SCP→DSK/flux-track decoder) or
`src/hw/a2r.h`/`a2r.cpp` (Applesauce A2R→SCP transcoder) need any gating —
both are already fully clean-room and have zero dependency on
`src/capsimg`.
