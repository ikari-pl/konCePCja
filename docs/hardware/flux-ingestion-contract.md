# Flux ingestion contract — the shared seam for new flux-format loaders

Status: **audit / implementation-planning doc**. 2026-07-13. Read-only
survey of the existing code; defines the single target every new flux
loader (KryoFlux STREAM, SCP file, HFE, A2R) should aim at, and inventories
what already exists so new work reuses it instead of duplicating it.

Companion specs: [`flux-media.md`](flux-media.md) (the SCP container + PLL
decoder, Stage 1/3), [`fdc-device.md`](fdc-device.md) (the FDC Device),
[`ipf-format.md`](ipf-format.md) (the IPF/CAPS decoder spec — §1.2 already
scopes KryoFlux STREAM/CT-RAW as explicitly out of scope for the IPF
decoder and reserves a future `kryoflux-stream-format.md` slot, which does
not exist yet).

---

## 1. The shared flux-ingestion seam

There is exactly **one** point where flux reaches the running machine, and
it takes exactly one container format.

```c
// src/hw/fdc.h
int fdc_attach_flux(const Device* dev, const uint8_t* scp, size_t len);
```

```cpp
// src/subcycle/machine.h
bool Machine::insert_flux(const uint8_t* scp, size_t len);  // → fdc_attach_flux
```

**The container `fdc_attach_flux` consumes is raw SCP-file bytes** —
literally the on-disk SuperCard Pro container format (header, per-cylinder
track-offset table, per-revolution flux-word streams), passed as a
caller-owned buffer that must outlive the attachment (`src/hw/fdc.cpp:1234`,
`fdc_media::scp`/`scp_len`). There is no intermediate parsed/decoded struct
in the public API — the FDC stores the pointer+length and decodes
on-demand, one cylinder/revolution at a time, via
`flux_decode_track_rev()` in `src/hw/flux.cpp` (`ensure_flux_cache` in
`fdc.cpp`, called on every head-settle). This is what makes weak/fuzzy bits
"emerge" physically: the FDC serves whichever captured revolution is
passing the head (`docs/hardware/flux-media.md` §7).

Constraints on the buffer a loader must produce:
- Magic `"SCP"` at offset 0, header + 168-slot track-offset table through
  offset `0x2B0` (`kTlutEnd` in `flux.cpp`).
- 16-bit bitcell width only (`scp[0x09]` must be 0 or 16).
- ≥1 revolution (`scp[0x05]`), side-0-only dumps or `heads==2` are rejected
  (`FLUX_E_GEOMETRY`).
- Flux words are **big-endian** 16-bit ticks; `0x0000` is a 65536-tick
  carry into the next word (both `flux.cpp`'s `rd16be`/PLL and the two
  existing producers below follow this convention).
- Resolution byte `scp[0x0B]`: tick width = `25ns * (resolution+1)`. The
  decoder's nominal half-cell is `80/(resolution+1)` ticks, i.e. it expects
  a physically-2µs DD half-cell once resolution is accounted for — a loader
  either uses resolution 0 (25 ns/tick, matches the IPF mirror) or a
  different resolution byte as long as the tick math stays consistent (A2R
  uses resolution 4 = 125 ns/tick to avoid rescaling its native 125 ns
  ticks — see §2).
- Side-0 only: even TLUT slots (`slot = cyl*2`) unless the legacy
  single-sided layout is used (`ScpGeom::legacy`, only relevant to files
  written by very old SCP tools).

`flux_scp_probe(scp, len)` is the cheap "is this attachable" pre-check
(header + offset-table sanity only, no decode) — call it before
`fdc_attach_flux`/`insert_flux` to fail fast with a clear error rather than
relying on `fdc_attach_flux`'s own `cyls<=0 || revs<=0` rejection.

**Bottom line for a new format loader: decode/transcode your format into an
in-memory SCP byte buffer, then hand that buffer to
`Machine::insert_flux()` (build time) or
`subcycle_bridge_insert_media(bytes, /*flux=*/true, /*unit=*/0)` (runtime
hot-swap). Do not invent a second in-memory flux container — SCP bytes are
the interchange format, by construction of the two loaders that already
exist (§2).**

### 1.1 Second interchange level: pre-decoded MFM bitcells

If your source format already gives you decoded MFM bitcells per
revolution (not raw flux-transition timings) — which is the case for HFE —
there is a second, higher-level entry point that skips the SCP flux-word
encoding math and goes straight from bitcells to an SCP buffer:

```cpp
// src/ipf.h
struct t_mfm_rev  { std::vector<uint8_t> bits; uint32_t nbits = 0; };
using t_mfm_track = std::vector<t_mfm_rev>;  // one entry per revolution

std::vector<uint8_t> scp_from_mfm_tracks(const std::vector<t_mfm_track>& cyls);
```

`cyls[cyl]` is empty for an absent/unformatted cylinder; every present
cylinder must carry the same revolution count across the whole vector
(`scp_from_mfm_tracks` returns `{}` on mismatch — check for empty output).
Bits are packed MSB-first; a `1` bit is a flux transition, encoded at
`kTicksPerCell = 80` (2 µs cells / 25 ns ticks) per bitcell of gap. This is
exact for MFM at nominal density: no PLL round-trip loss, because you're
handing over the exact bitcell timeline instead of re-measuring it from
continuous flux intervals.

So the full reuse menu, cheapest-first:
1. **Already have raw SCP bytes** (an actual `.scp` file, or a format
   that's flux-timing-native like KryoFlux STREAM) → transcode container
   framing only, reuse the flux.cpp PLL/decode pipeline unchanged. This is
   what A2R does (§2.2).
2. **Already have decoded MFM bitcells per revolution** (HFE stores the
   MFM bitstream directly, not flux timing) → build `t_mfm_track` entries,
   call `scp_from_mfm_tracks()`. This is what the IPF mirror does (§2.1).
3. **Neither** (you have to decode your own bit-cell recovery from raw
   pulse data with a format-specific PLL) → not currently needed by any of
   the four target formats; would be new decoder work, not reuse.

### 1.2 Reaching the running machine and the host-side (disc-tools) view

Two distinct paths, both already generic over "flux vs DSK bytes":

- **Engine build / initial load** — `subcycle_bridge.cpp` (~line 253-270):
  reads `CPC.driveA.file`, decides `insert_flux` vs `insert_disk` by
  extension, and calls whichever `Machine::insert_*` matches. Drive-A-only
  (see §5 for why).
- **Runtime hot-swap** — `subcycle_bridge_insert_media(std::vector<uint8_t>
  bytes, bool flux, uint8_t unit)` (`subcycle_bridge.h`/`.cpp`) queues the
  bytes; the Z80 thread applies them at the next frame boundary via
  `apply_pending_media()`, which calls `b.machine.insert_flux(...)` when
  `kind == PendingMedia::kFlux`. **Flux hot-swap is rejected for `unit !=
  0`** (`"flux is drive-A-only — swap ignored"`) — the FDC's flux cache is
  drive-A-only by construction (`fdc.cpp` `sel_media`/`flux_backed`
  comments).

Neither path populates the **legacy `t_drive`/`t_track` host-side view**
that disc-tools, the M4 board's directory reader, and DSK export read
(`hw_views.h`). For IPF/CAPS media that view is filled by `ipf_load()`
*before* the flux mirror ever runs (`ipf.cpp:414-417`: "this decode feeds
the host-side t_drive surface"). **A new flux-native format loader (SCP
file, HFE, real KryoFlux) has no CAPS-equivalent decode step that
populates `t_drive`** — it would need to either (a) also decode into
`t_drive`/`t_track` (duplicate work, format-specific), or (b) leave
`t_drive` empty and accept that disc-tools/M4-directory/DSK-export show
nothing for flux-native media until Stage 3's `flux_decode_track_rev`
output is wired to a `t_drive`-shaped view (not done today — out of scope
for this audit, worth its own bead). This is the one real gap in the seam:
**the FDC-consumption side is fully generic (§1), the host-view side is
not.**

---

## 2. Reuse inventory — what already exists

### 2.1 `src/ipf.h` / `src/ipf.cpp` — the CAPS→SCP mirror (the template to copy)

- `t_mfm_track`/`t_mfm_rev` (§1.1) — the decoded-bitcells interchange
  struct.
- `scp_from_mfm_tracks()` — pure function, MFM bitcells → SCP bytes. Fully
  reusable by any loader that produces decoded bitcells (HFE).
- `ipf_mirror_to_scp(t_drive* drive)` — **not directly reusable** by new
  formats (it's IPF/CAPS-specific: re-locks the CAPS track object per
  revolution to advance the flakey-bit RNG), but it is the worked example
  of "decode N revolutions per cylinder into `t_mfm_track`, then call
  `scp_from_mfm_tracks`". A new loader's per-format function plays the same
  role.
- Gated behind `HAS_CAPSIMG`; the non-CAPS stub keeps the same public API
  so callers need no `#ifdef`s — worth mirroring for any format loader that
  might itself be optionally compiled.

### 2.2 `src/hw/a2r.h` / `src/hw/a2r.cpp` — a complete worked transcoder (the template for flux-timing formats)

Header's own framing: *"The flux decoder (src/hw/flux) speaks SCP only.
A2R is the same kind of artifact — raw flux transition timings — just a
different container, so we transcode rather than write a second decoder."*

```cpp
int a2r_to_scp(const uint8_t* a2r, size_t len, std::vector<uint8_t>& out);
```

Parses the A2R3 RWCP chunk (accumulated-byte flux encoding, `0xFF` = +255
no-transition, any other byte = final delta + transition), keeps up to two
side-0 (even-location) timing captures per cylinder, and re-emits them as
SCP big-endian flux words with a hand-built SCP header/TLUT/TDH — i.e. it
performs exactly step 1 of §1.1's menu: **container transcode only, no independent
PLL/MFM decode**. It sets the SCP resolution byte to 4 (125 ns/tick) so
A2R's native 125 ns ticks copy across unscaled, rather than rescaling into
flux.cpp's 25 ns convention — a cheap trick worth reusing for any other
125-ns-tick-native format (deliberately documented in `a2r.h`'s header
comment).

Error codes (`A2R_E_NOT_A2R`, `A2R_E_UNSUPPORTED` for A2R2/STRM-only or bit
captures, `A2R_E_TRUNCATED`, `A2R_E_NO_FLUX`) mirror `flux.h`'s
`FLUX_E_*` convention — worth matching in new loaders for consistent
caller handling.

### 2.3 `src/hw/flux.h` / `src/hw/flux.cpp` — the decoder every format loader ultimately feeds

- SCP container parse + geometry validation (`scp_geometry`).
- Software PLL (`pll_decode`/`estimate_cell`) — SCP flux words → packed
  MFM bitcells. This is the piece a flux-timing format loader (KryoFlux
  STREAM) does **not** need to reimplement, provided it can express its
  timings as SCP flux words (§1's tick/word contract).
- MFM byte/track scan (`scan_track`) — 0x4489 sync, IBM System 34 ID/data
  fields, CRC-CCITT, weak-sector comparison across revolutions.
- Two public surfaces: the Stage 1 offline `flux_scp_to_dsk` (SCP → DSK
  buffer, used by tooling/tests, not the live FDC) and the Stage 3
  `flux_decode_track_rev`/`flux_scp_revolutions`/`flux_scp_cylinders` that
  `fdc.cpp` calls on-demand for the rotating FDC (§1).
- **No heap**, fixed function-local scratch — house style new decode code
  should match if it lives in `src/hw/`.

### 2.4 `test/hw/fdc_test.cpp` — `FdcFlux.DecodesARealScpCaptureWhenProvided`

This gated test (skips when no fixture is present; looks for
`KONCEPCJA_REAL_SCP` env var or `test/hw/fixtures/real.scp` /
`real.a2r`) exercises the **decoder**, not a file loader: it calls
`read_file()` directly (bypassing `slotshandler.cpp` entirely), optionally
runs the bytes through `a2r_to_scp()` if the fixture is an A2R, then drives
`flux_scp_probe` → `flux_scp_to_dsk` → `flux_decode_track_rev` against a
real capture to prove the PLL+MFM+CRC chain holds up outside the
synthetic-encoder round-trip the other `FdcFlux.*` tests use (their
"encoder+decoder shared bug is invisible" caveat, cited verbatim in the
test's own comment). **It proves the A2R transcoder + flux decoder are
correct together; it does not prove any file-loading path exists** — see
§3.

---

## 3. A2R status verdict

**A2R already fully decodes/transcodes flux. It needs file-loading wiring, not a decoder.**

Evidence:
- `a2r_to_scp()` is a complete, tested implementation
  (`test/hw/a2r_test.cpp`: `RejectsNonA2r`, and a byte-for-byte check of
  `TranscodesFluxIntoAWellFormedScp` covering header fields, TLUT slot,
  revolution-entry layout, and the `0xFF`-carry accumulation case).
- It is validated end-to-end against real captures by the gated FDC test
  (§2.4).
- **`grep -rn "a2r\|A2R" src/slotshandler.cpp src/subcycle_bridge.cpp`
  returns zero matches.** Nothing calls `a2r_to_scp()` outside
  `test/hw/a2r_test.cpp` and `test/hw/fdc_test.cpp`. There is no `.a2r`
  entry in `files_loader_list`, `drive_extensions()`, `fillSlots()`'s
  target-extension lists, the drag-drop handler in `kon_cpc_ja.cpp`, or the
  IPC `load` command in `koncepcja_ipc_server.cpp`.

So the remaining work for A2R is purely §4's wiring checklist — no decoder
work. This makes A2R the cheapest of the four target formats to ship.

---

## 4. How to add a new format loader — checklist

Mirrors how DSK (direct) and IPF/RAW (CAPS mirror) do it. Follow in order:

1. **Decode/transcode to SCP bytes.** Write `<fmt>_to_scp(buf, len,
   std::vector<uint8_t>& out)` (flux-timing formats — KryoFlux STREAM,
   generic SCP-compatible containers) or build `t_mfm_track` and call
   `scp_from_mfm_tracks()` (bitcell formats — HFE). Put it in `src/hw/` if
   it's a pure, heap-free, no-dependency transcoder (matches `a2r.cpp`'s
   house style); put it alongside `ipf.cpp` if it needs the same
   `t_drive`/host-view integration IPF has. A2R needs **no new code here**
   — it's done (§3).
2. **Register a `file_loader` entry** in `src/slotshandler.cpp`'s
   `files_loader_list` for `DRIVE::DSK_A` and `DRIVE::DSK_B` (mirror the
   `.ipf`/`.raw` pair at lines 64-86). The loader's `load_from_filename`/
   `load_from_file` callback is the point that currently only populates the
   **legacy `t_drive` view** for DSK/IPF — decide whether your new format
   does the same (needs a decode-to-`t_drive` step; DSK has one natively,
   IPF gets one from CAPS) or intentionally leaves `t_drive` empty (flag
   this loudly — disc-tools/M4-directory/DSK-export will show nothing;
   §1.2's known gap).
3. **Add the extension** to:
   - `drive_extensions(DRIVE::DSK_A/DSK_B)` (`.dsk.ipf.raw` today) so
     zip-archive classification and the IPC/CLI extension gate accept it.
   - `fillSlots()`'s `targets[]` list (`".dsk.ipf.raw"` for drive A/B) so
     CLI slot-file args route the extension to a drive slot at all —
     **today an unlisted extension (e.g. `.scp`) is silently dropped by
     `fillSlots`, never even reaching `file_load`.** This is why `.scp`
     currently cannot be loaded via CLI args, drag-drop, or the IPC `load`
     command despite `subcycle_bridge.cpp` having dead/aspirational
     `ends_with(..., ".scp")` branches (§5) — wiring step 3 is what would
     activate them.
   - The drag-drop extension `if`-chain in `kon_cpc_ja.cpp` (~line 3649).
   - The IPC `load` command's extension dispatch in
     `koncepcja_ipc_server.cpp` (~line 1134) if IPC-driven loading is
     wanted (currently only `.dsk`/`.sna`/`.cdt`/`.voc`/`.cpr` are handled
     there — **not even `.ipf`/`.raw`/`.scp`**, so this is a pre-existing
     gap wider than just new formats; worth flagging to the caller, not
     necessarily this bead's job to fix wholesale).
4. **Bridge to the machine** in `src/slotshandler.cpp`'s `file_load()`
   (mirror the `else if (extension == ".ipf" || extension == ".raw")`
   block ~line 1024): call your transcoder, then
   `subcycle_bridge_insert_media(std::move(scp_bytes), /*flux=*/true,
   /*unit=*/0)` for drive A only (flux is drive-A-only end to end — FDC
   cache, hot-swap rejection, and the `subcycle_bridge_start()` initial
   load all agree on this; see §5). Log clearly and skip the mirror for
   drive B, matching the existing `"flux is drive-A-only"` message.
5. **Bridge at engine build time** in `subcycle_bridge.cpp` (~line 253-270,
   inside the function around `subcycle_bridge_start`): extend the
   extension check (`caps = ends_with(...)` today only tests
   `.ipf`/`.raw`) so your format's transcode path is picked instead of
   `read_file()` + `insert_disk`. The existing `ends_with(...,  ".scp")`
   branch there is a preview of this — once step 3 wires `.scp` through
   `fillSlots`/`drive_extensions`, that branch starts firing for real
   (currently it's reachable in principle but nothing upstream ever sets
   `CPC.driveA.file` to a `.scp` path, so it is effectively dead code
   today — verified: no `.scp` in `files_loader_list`, `drive_extensions`,
   `fillSlots` targets, the drag-drop chain, or IPC `load`).
6. **Tests**: a transcoder unit test in the style of `a2r_test.cpp`
   (hand-built minimal file, byte-for-byte checked SCP output — header,
   TLUT slot, revolution entries, tick math) plus a gated real-capture test
   added to the `FdcFlux.DecodesARealScpCaptureWhenProvided`-style harness
   in `test/hw/fdc_test.cpp` if a real fixture is obtainable (env var /
   `test/hw/fixtures/real.<ext>`, SKIP when absent — never commit
   possibly-unlicensed capture binaries).
7. **Docs**: a `docs/hardware/<format>-format.md` spec in the style of
   `ipf-format.md` (provenance rule, scope table, byte-level layout) if the
   format has any ambiguity worth recording; a short addition to this file
   otherwise.

---

## 5. Extension / dispatch map

| Extension | Drive A/B loader (`files_loader_list`) | `drive_extensions()` | `fillSlots()` targets | Drag-drop | IPC `load` | Flux mirror at load |
|---|---|---|---|---|---|---|
| `.dsk` | `dsk_load` | yes | yes | yes | yes | n/a — `insert_disk` |
| `.ipf` | `ipf_load` (CAPS) | yes | yes | yes | **no** | `ipf_mirror_to_scp` → `insert_flux`, drive A only |
| `.raw` | `ipf_load` (CAPS RAW variant) | yes | yes | yes | **no** | same as `.ipf` |
| `.scp` | **none** | **no** | **no** | **no** (falls to "Unknown file type") | **no** | dead code path in `subcycle_bridge.cpp` (checks `ends_with(..., ".scp")` but nothing upstream ever produces a `.scp` `CPC.driveA.file`) |
| `.hfe` | none | no | no | no | no | — |
| `.a2r` | none | no | no | no | no | — (decoder exists, §3) |
| KryoFlux STREAM (`.raw` *per-track set*, or a hypothetical single-file container) | **collides with `.raw` above** | — | — | — | — | would currently be misrouted to `ipf_load`, which gates on the 4-byte `"CAPS"` magic (`ipf.cpp:365`) and cleanly rejects non-CAPS input rather than misdecoding it — so today a genuine KryoFlux `.raw` fails loudly, it does not silently corrupt |

### 5.1 The `.raw` collision

`.raw` is **already claimed** by the CAPS/SPS "RAW" container variant
(`ipf_load`, gated on the `"CAPS"` 4-byte magic at `ipf.cpp:365`) — this is
a *different* format from KryoFlux's raw stream output despite the shared
extension. `docs/hardware/ipf-format.md` §1.2 already documents this
explicitly: *"KryoFlux STREAM (`.raw` sets) ... today's loader gates on the
4-byte `"CAPS"` magic before any decoding ... so only genuine IPF files are
ever decoded."*

Consequences for a KryoFlux loader:
- **Cannot dispatch by extension alone** — `.raw` already means "try
  CAPS/SPS RAW" in every dispatch table in §4 step 3. A genuine KryoFlux
  `.raw` file reaching `ipf_load` today fails cleanly (wrong magic →
  `ERR_DSK_INVALID`), so there's no silent-corruption risk, but there's
  also no path to success without disambiguation.
- **Disambiguate by content magic, not extension**, at the point extension
  routing happens (`file_load()`'s loop over `files_loader_list` in
  `slotshandler.cpp`, and the parallel checks in `kon_cpc_ja.cpp`/
  `koncepcja_ipc_server.cpp` if wired there too): peek the first bytes
  before picking a loader — `"CAPS"` → existing CAPS path, else try the new
  KryoFlux STREAM decoder. `flux_scp_probe`-style "cheap sniff, no full
  decode" is the pattern to match.
- **KryoFlux STREAM is also structurally different from the other three
  formats**: real KryoFlux dumps are typically *one file per track* (e.g.
  `track00.0.raw`, `track00.1.raw`, ...), not a single-file-per-disk
  container like SCP/HFE/A2R/DSK. A `.raw`-extension loader plugged into
  the single-file dispatch tables in §4 would need either (a) a
  multi-file-set loader entered via directory/first-file selection (new
  UX, not just a new `file_loader` entry), or (b) scoping v1 to a
  single-file KryoFlux-compatible container if one is in scope. Confirm
  which before implementing — this is a bigger structural difference than
  SCP/HFE/A2R, which are all single files today.
- The reserved spec slot `kryoflux-stream-format.md`
  (referenced but not yet created, per `ipf-format.md` §1.2) is the right
  place to record the disambiguation + multi-file-set decision once made.
