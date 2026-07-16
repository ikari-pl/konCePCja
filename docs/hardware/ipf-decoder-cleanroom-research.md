# Clean-room replacement for capsimg (IPF / CT RAW / KryoFlux STREAM) — feasibility & spec skeleton

Status: **research + spec only** (no implementation). Author: scoping pass, 2026-07-13.

## 0. Why this exists

`src/capsimg/` is the **SPS Decoder Library** (© 2001–2014 István Fábián, under
exclusive licence to KryoFlux Products & Services Ltd). It decodes three
disk-preservation formats — **IPF**, **CT RAW**, **KryoFlux STREAM** — but its
licence (`src/capsimg/LICENCE.txt`) is **explicitly non-commercial**:

> *"Redistributions may not be sold, nor may they be used in a commercial
> product or activity … Using SPS DECODER LIBRARY as a 'freebie' … still
> constitutes commercial usage and is forbidden."*

That conflicts with konCePCja's new source licence (which permits commercial
tool-use). We are replacing capsimg with our own decoder, implemented **from
public FORMAT DOCUMENTATION only**, so IPF/RAW/STREAM support becomes ours under
our licence.

### Clean-room provenance rules (non-negotiable)

1. **Do not** read, port, or transliterate `src/capsimg/**` internals
   (`Codec/`, `CAPSImg/`, `Core/BitBuffer`, `CTRawCodec*`, `DiskEncoding*`,
   `CapsFormatMFM*`). Our own glue — `src/ipf.cpp`, `src/ipf.h` — is ours and
   was read for this scoping. The CAPS **API surface** we call is public
   (`CapsLib.h`); knowing *which functions we call* is fine, but the format
   internals behind them must come from docs.
2. **Do not** base code on GPL readers (many emulators, some flux libs). GPL
   would re-impose the copyleft we are shedding. Learn the FORMAT from
   documentation; only copy CODE from permissive/public-domain sources, and even
   then prefer independent implementation.
3. Every source below is annotated: **DOC = safe to learn the format from**;
   **CODE = licence-gated, see note**.

---

## 1. What our engine actually needs (scope floor)

Read from `src/ipf.cpp`, `src/ipf.h`, `src/subcycle_bridge.cpp`,
`test/ipf_mirror_test.cpp`, `test/hw/fdc_test.cpp`.

We consume a **tiny slice** of capsimg's generality. We do **not** need the CAPS
FDC emulator (`CapsFDCEmulator.*`), write-back, IPX analyzer records, or the
full multi-platform density model. Two consumers only:

1. **Host `t_drive` sector view** (`ipf_load` → `ReadTrack`): decode each track's
   MFM bitstream into `t_sector` CHRN + data for disc tools, DSK export, and the
   M4 board directory reader. Today this runs our *own* MFM scanner in
   `src/ipf.cpp` (`ReadByte`/`ReadWord`/`ReadTrack`) over the bitstream CAPS
   hands us in `cti.trackbuf` (`cti.tracklen` bits, clock+data interleaved).
   **This scanner is already ours and stays** — a clean-room decoder just has to
   produce the same `trackbuf`/`tracklen` it currently gets from `CAPSLockTrack`.
2. **Sub-cycle flux FDC** (`ipf_mirror_to_scp` → `scp_from_mfm_tracks` →
   `Machine::insert_flux`): the sub-cycle engine plays media as **flux**. Today
   we take CAPS's decoded MFM bitcells (2 µs DD cells), treat a `1` bitcell as a
   flux transition, and synthesise an in-memory SCP container (25 ns ticks,
   `kTicksPerCell=80`), capturing `kMirrorRevs=3` revolutions so weak bits vary.

**The minimal data product** a clean-room decoder must yield, per cylinder/head:

| Product | Used by | Notes |
|---|---|---|
| MFM bitcell stream: `bits[]` + `nbits` (clock+data interleaved, MSB-first) | `ReadTrack` (sector view) **and** `scp_from_mfm_tracks` (flux) | Exactly what `t_mfm_rev` in `ipf.h` already models. |
| `flakey` flag (per track) | mirror decides whether to re-capture per rev | CAPS surfaces this as `CTIT_FLAG_FLAKEY`. |
| Multiple revolutions (≥3) for flakey/weak tracks | mirror | For flux formats each rev already exists in the capture. |
| Geometry: min/max cylinder, min/max head | `ipf_load` | From INFO/IMGE records (IPF) or file set (flux). |

Crucial simplification: **flux-native formats (STREAM, CT RAW, SCP) do not need
the IPF stream-element reconstruction at all.** They already contain flux; we
feed flux straight into the existing SCP/flux layer, and the FDC/`hw/flux.cpp`
recovers MFM cells and sectors. The MFM-reconstruction work is **IPF-only**.

`t_mfm_rev` / `t_mfm_track` in `src/ipf.h` and `scp_from_mfm_tracks()` are the
stable seam: **any clean-room decoder plugs in above them and the rest of the
pipeline is unchanged.**

---

## 2. Per-format feasibility verdicts

### 2.1 KryoFlux STREAM — **FEASIBLE FROM DOCS (green). Realistic first target.**

- **Primary DOC:** *KryoFlux Stream File Documentation*, Jean Louis-Guérin
  (DrCoolZic), Rev 1.1, 2013-12-01
  (`kryoflux.com/download/kryoflux_stream_protocol_rev1.1.pdf`). Marked
  **"Copyleft"** — documentation, byte-level complete, **safe to learn the
  format from**. Acknowledges info from István Fábián (SPS) but is a
  description, not code.
- **Completeness:** essentially total at the byte level. The block-header table
  is fully enumerated:

  | Header | Block | Len | Meaning |
  |---|---|---|---|
  | `0x00–0x07` | Flux2 | 2 | flux = `(hdr<<8)+val1` |
  | `0x08` | Nop1 | 1 | skip 1 byte |
  | `0x09` | Nop2 | 2 | skip 2 bytes |
  | `0x0A` | Nop3 | 3 | skip 3 bytes |
  | `0x0B` | Ovl16 | 1 | add `0x10000` to next flux |
  | `0x0C` | Flux3 | 3 | flux = `(val1<<8)+val2` |
  | `0x0D` | OOB | var | out-of-band block (header 4B: `0x0D`,type,size16) |
  | `0x0E–0xFF` | Flux1 | 1 | flux = header value |

  OOB types: `0x00` Invalid, `0x01` StreamInfo, `0x02` Index, `0x03` StreamEnd,
  `0x04` KFInfo (ASCII `name=value`, carries `sck`/`ick`), `0x0D` EOF. Flux
  timing = `sample_counter / sck`; `sck`/`ick` come from the KFInfo block
  (fallback defaults in the doc). Index handling, sample-counter-overflow edge
  cases, "index after last flux", "index before any flux", and RPM interpolation
  are all documented.
- **Hard parts:** none structurally. The subtlety is **index/flux alignment**
  (OOB blocks are asynchronous to the in-stream buffer; the Index block's
  StreamPosition points at the *next* flux and the Sample Counter measures from
  the *previous* flux). The doc's "Analysis of Index Information" section spells
  out the two-pass algorithm. A multi-file `.raw` **set = one file per track**,
  each holding several revolutions between index pulses (SPS wants ≥5 indexes/6
  revolutions for IPF mastering; we need ≥3 for the mirror).
- **Verdict:** implementable from the Rev 1.1 PDF alone. Lowest risk, highest
  doc quality. **Do this first** to stand up the flux path and the oracle
  harness.

### 2.2 CT RAW (`.ctr` / `.raw`) — **PARTIALLY FEASIBLE (amber). Undocumented compression.**

- **What it is:** the *pre-KryoFlux* format everyone submitted to CAPS/SPS. A
  compact container holding **~5 complete revolutions of bitcell timing per
  track**, using **heavy delta compression purpose-built for bitcell data**
  (and further zip-friendly). Implemented in capsimg by
  `Codec/CTRawCodec.cpp` + `CTRawCodecDecompressor.cpp` (**off-limits**).
- **DOC status:** **poor.** The KryoFlux STREAM Rev 1.1 doc does **not** cover
  CT RAW — they are different containers. No public byte-level spec of the CT
  RAW delta-compression scheme was found. Community forum posts describe *what*
  it is (5 revs, delta+zip) but not the codec.
- **Hard parts:** the delta-compression/bitstream codec is effectively
  **undocumented** → would require reverse-engineering (from sample files +
  behavioural diffing against the capsimg oracle), which is exactly the kind of
  work clean-room provenance makes slow and risky.
- **CPC relevance:** low. The overwhelming majority of CPC preservation dumps in
  circulation are **IPF**; CT RAW is largely a submission/interchange format.
- **Verdict:** **de-prioritise / possibly drop from v1.** If needed later, treat
  it as an RE mini-project with its own oracle, or convert CT RAW→STREAM/SCP
  out-of-band with a licensed local tool during preservation and only ship the
  flux-format reader. Do **not** let it block IPF.

### 2.3 IPF (CAPS container) — **FEASIBLE FROM DOCS (amber-green). Trickiest but well-documented. Highest CPC value.**

- **Primary DOC:** *Interchangeable Preservation Format (IPF) Documentation*,
  Jean Louis-Guérin (DrCoolZic), **Version 1.6, April 2018**
  (`kryoflux.com/download/ipf_documentation_v1.6.pdf`). A **reverse-engineered**
  description — the author repeatedly warns *"the information … comes from
  different sources and inevitably must contain errors."* Still **safe to learn
  the format from** (it is documentation, not the SPS library code). It contains
  a **complete worked example** (Atari "Theme Park Mystery" track 00.0) mapping
  records → bytes.
- **Completeness at the container level: high.** Records are fully specified:
  - **File** = big-endian records, each = `Header(type[4],length,CRC32)` +
    type-specific block + optional extra block. `CAPS`(12B) → `INFO`(96B) →
    `IMGE`(80B, one per track/side) → `DATA`(28B header + extra block, one per
    IMGE, matched by `dataKey`). IPX adds `CTEI`/`CTEX` analyzer records (we can
    ignore these).
  - **INFO:** mediaType, `encoderType` (**1 = CAPS**, **2 = SPS**), min/max
    track & side, platforms (04 = Amstrad CPC), dates, `fileKey`.
  - **IMGE:** track, side, `density` (0–9: 01 Noise, 02 Auto, 03 Copylock
    Amiga, 05 Copylock ST, 06 Speedlock Amiga, …), `signalType` (01 = 2 µs
    cells), `trackBytes`, `startBitPos`, `dataBits`, `gapBits`, `blockCount`,
    `trackFlags` (**bit 0 = Fuzzy**), `dataKey`.
  - **DATA extra block** = `blockCount` **Block Descriptors** (32B each) followed
    by the **Data Area**.
  - **Block Descriptor:** `dataBits`, `gapBits`; then encoder-specific — for
    **CAPS** (encoderType 1): `dataBytes`,`gapBytes`; for **SPS** (encoderType
    2): `gapOffset`, `cellType`, `encoderType`(00/01 MFM/02 Raw), `blockFlags`
    (fwd/back gap, DataInBit), `gapDefault`, `dataOffset`.
- **The genuinely hard part — the Data Area (§3.7.2):** for SPS-encoded files the
  track is reconstructed from **stream elements**, not stored as a bitstream:
  - **Data Stream Elements:** `dataHead` byte = `dataSizeWidth`(bits 7-5) +
    `dataType`(bits 4-0: **1 Sync, 2 Data, 3 Gap, 4 Raw, 5 Fuzzy**), then
    `dataSize` (big-endian, `dataSizeWidth` bytes), then `dataSample`
    (`dataSize` bits or bytes per the block's DataInBit flag). List terminates
    on a null `dataHead`. **Fuzzy (weak) bits:** `dataType=5` carries a size but
    **no sample** — the decoder must *generate random data* of that length
    (this is exactly the weak-bit variation our 3-revolution mirror needs).
  - **Gap Stream Elements** (fill the inter-block gap; fwd/back per blockFlags):
    `gapHead` byte = `gapSizeWidth`(bits 7-5) + `gapElemType`(bits 4-0:
    **1 GapLength, 2 SampleLength**), then `gapSize`, then (for type 2) the
    `gapSample` bits. A `GapLength` + `SampleLength` pair means "repeat this
    sample value N times"; an unrepeated pair means "repeat as many times as
    needed to fill `gapBits`", and fwd+back streams meet at the **write splice**.
  - The decoder must then **MFM-encode** each element (Sync = raw `$4489`-type
    marks with missing clock; Data/Gap = MFM-encoded per the cell type) and
    concatenate into the final track bitcell stream — the same
    clock+data-interleaved `trackbuf` our existing `ReadTrack` already scans.
- **Documented gaps / uncertainty (the real risks):**
  - The **CAPS encoder** (encoderType 1, older files) stores `dataBytes`/
    `gapBytes` — the doc describes it more thinly than the SPS path; most modern
    IPFs use SPS (encoderType 2), so v1 can target SPS first.
  - **Density / protection descriptors** (density 3–9: Copylock, Speedlock, Adam
    Brierley, …) are **named but the exact cell-size / bit-density modelling per
    type is not fully specified** in the doc. For most CPC titles density = Auto
    (02) and the stream elements fully define the track, so this is a
    tail-risk, not a blocker — but exotic protections may read wrong.
  - The doc's own "must contain errors" caveat means **byte-exact validation
    against real images is mandatory** (see §5 oracle).
- **Verdict:** feasible from the v1.6 doc for the **SPS-encoder, Auto-density**
  majority — which covers essentially all mainstream CPC preservation. Copy
  protections riding on explicit density descriptors are the residual risk.

**Confirms the task's hypothesis with one correction:** STREAM is indeed the
best-documented and easiest. IPF's *container* is well documented (DrCoolZic
v1.6) and its stream-element data area — the "hard part" — **is** documented with
a worked example, so IPF is tractable. The genuine documentation black hole is
**CT RAW's compression codec**, which is *less* documented than IPF.

---

## 3. Recommended implementation order & parallelisable work units

Order by (doc quality × CPC value ÷ risk):

**Phase A — Flux path + oracle harness (STREAM).** Lowest risk, unlocks
everything else.

**Phase B — IPF SPS-encoder decoder.** Highest CPC value; the bulk of the work.

**Phase C — IPF CAPS-encoder + density/protection hardening.** Tail coverage.

**Phase D (optional) — CT RAW.** Only if a real need appears; RE-gated.

### Work-unit decomposition (each is an independent subagent task)

| Unit | Phase | Depends on | Deliverable | Parallel? |
|---|---|---|---|---|
| **U1** STREAM container parser | A | — | `.raw` → per-track flux ticks (per rev, index-aligned) | yes |
| **U2** Flux→SCP/insert_flux adapter | A | U1 (interface) | reuse `scp_from_mfm_tracks` pattern OR emit flux directly to `Machine::insert_flux` | yes (against interface) |
| **U3** Oracle harness + fixtures | A | — | local-only capsimg build + `KONCEPCJA_REAL_*` fixtures + byte-diff runner | yes |
| **U4** IPF record layer | B | — | CAPS/INFO/IMGE/DATA parse + CRC32 verify → structured records | yes |
| **U5** IPF Data-Area / stream-element decoder (SPS) | B | U4 (structs) | block descriptors + data/gap stream elements → element list | yes (against U4 structs) |
| **U6** MFM encoder (elements → clock+data bitcells) | B | U5 (interface) | element list → `t_mfm_rev` (`trackbuf`/`tracklen`) | yes |
| **U7** Weak-bit / fuzzy multi-rev generator | B | U6 | Fuzzy elements → N differing revolutions | yes (against U6) |
| **U8** IPF glue swap | B | U4–U7 | replace `CAPSLockTrack` calls in `ipf.cpp` with clean-room; keep `ReadTrack` + `scp_from_mfm_tracks` | no (integration) |
| **U9** CAPS-encoder path | C | U4 | encoderType=1 dataBytes/gapBytes reconstruction | yes |
| **U10** Density/protection modelling | C | U6, oracle | per-density cell-size handling, oracle-driven | yes |
| **U11** CT RAW codec RE | D | U3 | reverse-engineer delta compression against oracle | isolated |

Interfaces (`t_mfm_rev`, a `FluxRev`/tick vector, an IPF-record struct set) are
defined **once up front** so U-units fan out against stable seams. U4 and U5/U6
can proceed concurrently because U4 hands U5 typed records and U5 hands U6 a
typed element list.

---

## 4. Coverage risks (what capsimg does that a clean-room v1 might miss) & de-risking

| Risk | Impact | De-risk |
|---|---|---|
| **Weak/fuzzy bits** (IPF `dataType=5`, `trackFlags` bit 0; density Noise) | Protections that read differing data per rev fail | Implement U7 to regenerate random fuzzy data per revolution; oracle-diff that our N revs differ where capsimg's do. Our mirror already captures 3 revs. |
| **Density/protection descriptors** (Copylock, Speedlock, Adam Brierley — density 3–9) | Exotic protected tracks decode with wrong cell density | Doc under-specifies these. Gate as Phase C; drive purely by oracle diffs on real protected CPC titles; ship SPS+Auto first. |
| **CAPS encoder (encoderType 1)** older files | Some older IPFs won't load | U9; detect `encoderType` in INFO and fail-soft (fall back to legacy/capsimg locally, or clear error) until implemented. |
| **CT RAW compression** undocumented | `.ctr`/`.raw` CT files unreadable | De-scope from v1 (§2.2). Convert out-of-band if ever needed. |
| **DrCoolZic doc errors** ("must contain errors") | Subtle byte-level mistakes | Byte-exact oracle diff on every sample; treat the doc as a hypothesis, the oracle as ground truth. |
| **CRC32 / CRC16 details** (record CRC32 with field zeroed; MFM CRC-CCITT `0x1021`) | False "corrupt" rejections | We already have CRC-CCITT in `ipf.cpp`; add record-level CRC32 (zero the CRC field, compute over whole record). Verify against oracle. |
| **Big-endian field parsing** | Wrong offsets on little-endian host | Mirror the doc's explicit BE note; unit-test against the worked example bytes in the v1.6 PDF. |
| **Index/flux async alignment** (STREAM) | Wrong revolution boundaries → bad flux | Follow Rev 1.1 "Analysis of Index Information" two-pass; test RPM ≈ 300 and index count on real captures. |
| **Second side** | CAPS images with side 1 | Current mirror is side-0-only (logged). Keep that limitation explicit in v1; sector view already handles both sides via `ReadTrack`. |

---

## 5. Minimal decoder API/contract & oracle strategy

### 5.1 API contract (function-level; plugs in above the existing seam)

Keep `t_mfm_rev`/`t_mfm_track` (`src/ipf.h`) and `scp_from_mfm_tracks()`
**unchanged**. Introduce a clean-room backend with two entry shapes:

```cpp
// Geometry probe (replaces CAPSGetImageInfo).
struct CleanImageInfo {
  int min_cyl, max_cyl, min_head, max_head;
  int encoder_type;      // 1=CAPS, 2=SPS  (IPF only)
  bool is_flux;          // true for STREAM / CT RAW / SCP
};
bool clean_open(const std::string& path, CleanImageInfo& out);

// IPF path: reconstruct one track's MFM bitcells (replaces CAPSLockTrack +
// cti.trackbuf/tracklen). rev selects a weak-bit variation (0..N-1).
struct CleanTrackMFM { std::vector<uint8_t> bits; uint32_t nbits; bool flakey; };
bool clean_ipf_lock_track(const std::string& path, int cyl, int head,
                          int rev, CleanTrackMFM& out);

// Flux path (STREAM / CT RAW): hand back flux directly, per revolution, in
// SCP-compatible 25 ns ticks — then reuse the SCP/insert_flux layer.
struct CleanFluxRev { std::vector<uint32_t> ticks; uint32_t index_ticks; };
bool clean_flux_lock_track(const std::string& path, int cyl, int head,
                           std::vector<CleanFluxRev>& revs);
```

Integration (`src/ipf.cpp`, unit **U8**): `ipf_load` calls `clean_open` +
`clean_ipf_lock_track` in place of the CAPS calls; feeds `trackbuf`/`tracklen`
into the **existing** `ReadTrack` (sector view) and into `t_mfm_rev` for
`scp_from_mfm_tracks` (flux). For flux-native inputs, skip MFM reconstruction:
build the SCP bytes from `CleanFluxRev` (or call `Machine::insert_flux`
directly) and let `hw/flux.cpp` + the FDC recover sectors for the `t_drive` view.

This deletes our dependency on the CAPS API (`CAPSInit`, `CAPSAddImage`,
`CAPSLockImage`, `CAPSGetImageInfo`, `CAPSLockTrack`, `CAPSUnlockTrack`,
`CAPSUnlockImage`, `CAPSRemImage`, `CAPSExit`, `CAPSGetVersionInfo`) — the entire
list currently called from `src/ipf.cpp`.

### 5.2 Oracle & test strategy (we can't ship capsimg)

capsimg stays as a **local, TEST-ONLY oracle** — never committed, never
redistributed (its non-commercial licence forbids shipping; keep it behind a
build flag + `.gitignore`, or in a separate uncommitted worktree). The existing
gated tests already model this and need no redesign:

- `IpfMirror.MirrorsARealCapsImageWhenProvided` (`test/ipf_mirror_test.cpp`):
  skips unless `KONCEPCJA_REAL_IPF=<path>` or `test/hw/fixtures/real.ipf` exists
  — "so no (possibly unlicensed) preservation dump lands in the repo." CI is
  unaffected.
- `FdcFlux.DecodesARealScpCaptureWhenProvided` (`test/hw/fdc_test.cpp`): same
  pattern with `KONCEPCJA_REAL_SCP` (SCP or Applesauce `.a2r`).

Validation layers to add (all local, fixture-gated):

1. **Byte-exact MFM diff (IPF):** for a real `.ipf`, assert our
   `clean_ipf_lock_track` `bits`/`nbits` == capsimg's `cti.trackbuf`/`tracklen`
   for every cyl/head. This is the strongest signal and catches doc errors.
2. **Worked-example unit test (no oracle):** encode the v1.6 PDF "Theme Park
   Mystery" track-00.0 descriptor bytes and assert the documented block/data/gap
   values — a licence-free regression using only the public doc.
3. **Sector round-trip:** clean-room IPF → `ReadTrack` → `t_sector` CHRN+data ==
   capsimg-decoded sectors == the title's DSK export (where one exists).
4. **Flux round-trip (STREAM):** STREAM → flux → FDC → sectors; compare to the
   same disc's SCP/DSK; assert RPM ≈ 300 and index count.
5. **Weak-bit variance:** assert our N revolutions differ exactly where the track
   is flakey/fuzzy and are identical where it is stable (mirrors the current
   `kMirrorRevs` logic).

---

## 6. Spec-skeleton to author under `docs/hardware/` (one file per format)

Each future spec should be self-contained enough to implement without the source
PDFs. Suggested section headings and what each must pin down:

### `docs/hardware/kryoflux-stream-format.md`
1. **Scope & provenance** — Rev 1.1 doc, Copyleft, DOC-safe.
2. **Clocks** — mck/sck/ick; default values; KFInfo override.
3. **Block header table** — all `0x00–0xFF` cases (Flux1/2/3, Ovl16, Nop1/2/3,
   OOB) with byte layouts.
4. **Flux value reconstruction** — Ovl16 accumulation; `flux = counter/sck`.
5. **OOB blocks** — Invalid/StreamInfo/Index/StreamEnd/KFInfo/EOF field tables.
6. **Index analysis** — StreamPosition vs Sample Counter semantics; two-pass
   algorithm; the three edge cases; RPM interpolation formula.
7. **Multi-file `.raw` set** — one file/track, revolutions between indexes.
8. **Mapping to our flux structs** — `CleanFluxRev`, tick conversion to 25 ns.
9. **Test vectors** — small hand-built stream + expected flux array.

### `docs/hardware/ipf-format.md`
1. **Scope & provenance** — DrCoolZic v1.6, reverse-engineered, "may contain
   errors", DOC-safe; SPS-encoder + Auto-density is the v1 target.
2. **File & record model** — big-endian; `Header(type,length,CRC32)`; record
   CRC32 computation (zero-field-then-hash); record-to-record traversal (+ the
   DATA-record exception).
3. **CAPS / INFO / IMGE / DATA / (CTEI/CTEX ignored)** — full field tables with
   byte sizes (12/96/80/28…), encoderType, density, signalType, trackFlags,
   dataKey matching.
4. **Block Descriptor** — 32B; CAPS vs SPS variant fields; blockFlags bits
   (ForwardGap/BackwardGap/DataInBit).
5. **Data Area — Data Stream Elements** — `dataHead` decode
   (dataSizeWidth/dataType 1–5); Sync/Data/Gap/Raw/**Fuzzy** semantics; null
   terminator; Fuzzy = generate-random.
6. **Data Area — Gap Stream Elements** — `gapHead` decode
   (gapSizeWidth/gapElemType 1–2); GapLength+SampleLength repeat semantics;
   forward/backward fill; write-splice meeting point.
7. **MFM encoding of elements** — Sync marks (`$4489`/missing clock), Data/Gap
   MFM cell encoding, 2 µs cellType; producing the clock+data interleaved
   bitcell stream that `ReadTrack` scans.
8. **Weak bits & multi-revolution** — fuzzy → N differing revs.
9. **Density / protection appendix** — enumerate density 0–9; mark which are
   fully modelled vs oracle-driven TODO (Copylock/Speedlock/Adam Brierley).
10. **Worked example** — the Theme Park Mystery track-00.0 byte walk-through as a
    regression vector.

### `docs/hardware/ct-raw-format.md`
1. **Scope & provenance** — pre-KryoFlux CAPS submission format; ~5 revs/track;
   delta-compressed bitcell timing. **DOC status: incomplete — compression codec
   undocumented publicly.**
2. **Container layout** — what *is* known (track directory, revolutions).
3. **Compression codec** — **RE placeholder**: to be filled only from sample
   files + oracle behaviour, never from `Codec/CTRawCodec*`.
4. **De-scope note** — not in v1; conversion-to-flux workaround.

---

## 7. Bottom line

- **STREAM: green.** Fully documented (Rev 1.1). Build it first with the oracle
  harness; it also validates the flux seam we already ship.
- **IPF: amber-green.** Container fully documented; the "hard" stream-element
  data area **is** documented (v1.6) with a worked example. Feasible for the
  SPS-encoder/Auto-density majority that covers essentially all mainstream CPC
  titles. Residual risk = explicit density/protection descriptors → oracle-drive
  Phase C. Highest value for CPC.
- **CT RAW: amber.** The one real documentation black hole (custom, undocumented
  delta compression). Low CPC value. **De-scope from v1**; RE later if needed.
- **We need far less than capsimg provides** — per-track MFM bitcells (IPF) or
  flux (STREAM), a flakey flag, and ≥3 revolutions. `t_mfm_rev` +
  `scp_from_mfm_tracks()` are the stable seam; the clean-room decoder plugs in
  above them and the FDC/flux/sector pipeline is untouched.
- **Provenance is safe:** the two DrCoolZic PDFs and fluxfox are documentation /
  permissive; capsimg internals and GPL readers stay off-limits; capsimg remains
  only as an uncommitted local oracle behind the existing fixture-gated tests.

---

## Appendix A — Public sources & licences

| Source | Type | Licence / status | Use |
|---|---|---|---|
| *KryoFlux Stream File Documentation* Rev 1.1 (DrCoolZic), `kryoflux.com/download/kryoflux_stream_protocol_rev1.1.pdf` | DOC | "Copyleft" documentation | **Primary STREAM spec — safe to learn format.** |
| *IPF Documentation* v1.6 Apr 2018 (DrCoolZic), `kryoflux.com/download/ipf_documentation_v1.6.pdf` (also info-coach.fr v1.5) | DOC | Reverse-engineered documentation; author warns of errors | **Primary IPF spec — safe to learn format.** |
| Archive-Team file-format wikis (`fileformats.archiveteam.org/wiki/IPF`, `/KryoFlux`), softpres.org glossary | DOC | Community wiki | Background / cross-check. |
| Archivists' Guide to KryoFlux (GitHub) | DOC | Educational | Practitioner context on STREAM/CT RAW. |
| **fluxfox** (`github.com/dbalsom/fluxfox`) — Rust flux lib (IPF, STREAM, SCP) | CODE | **MIT** (permissive) | Independent cross-check; MIT is commercial-OK, but **prefer independent implementation**, and if any code is reused, retain the MIT notice. Not a substitute for the format docs. |
| `src/capsimg/**` — SPS Decoder Library | CODE | **Non-commercial (KryoFlux/SPS)** | **OFF-LIMITS as clean-room source.** Local test-only oracle; never commit/ship. |
| GPL IPF readers in various emulators / flux libs | CODE | **GPL** | **OFF-LIMITS** — copyleft. Do not read for implementation. |

> **Legal note to verify (do not rely on):** the DrCoolZic IPF doc states the
> "IPF decoder source was released under the MAME licence" (modern MAME =
> BSD-3-Clause, commercial-OK). The copy we hold in `src/capsimg/` clearly
> carries the **non-commercial** SPS Decoder licence. These may be different
> releases. This is a question for the licence owner / counsel — **the
> clean-room-from-docs plan does not depend on it** and should proceed
> regardless.

## Appendix B — Local files touched/read during this scoping
- Read (ours, in-scope): `src/ipf.cpp`, `src/ipf.h`, `test/ipf_mirror_test.cpp`,
  `test/hw/fdc_test.cpp`, `src/capsimg/LICENCE.txt`, `src/capsimg/README.md`.
- The CAPS API functions we currently call (to be removed): `CAPSInit`,
  `CAPSAddImage`, `CAPSLockImage`, `CAPSGetImageInfo`, `CAPSLockTrack`,
  `CAPSUnlockTrack`, `CAPSUnlockImage`, `CAPSRemImage`, `CAPSExit`,
  `CAPSGetVersionInfo`.
- Downloaded PDFs (scratchpad, not committed): IPF v1.6, KryoFlux STREAM Rev 1.1.
