# Flux / disk-image formats — clean-room feasibility & spec skeletons

Status: **research + spec only** (no implementation). Author: scoping pass,
2026-07-13. Companion to
[`ipf-decoder-cleanroom-research.md`](ipf-decoder-cleanroom-research.md) and
[`ipf-format.md`](ipf-format.md), which cover IPF/CT-RAW/STREAM at the
capsimg-replacement level. This document **broadens** the flux-format survey
beyond IPF to the formats worth adding natively under our own licence:
**KryoFlux STREAM**, **SCP**, **HFE**, and an honest re-examination of **CT RAW**,
plus a short survey of other CPC-relevant containers.

## 0. Why this exists & the one architectural fact that makes it easy

We are replacing the non-commercial `src/capsimg/**` (SPS Decoder Library) and
adding native support for more disk-image formats under konCePCja's own licence.
IPF is already being implemented (spec: [`ipf-format.md`](ipf-format.md)).

The decisive architectural fact (from the IPF research, confirmed by reading the
seam here): **flux-native formats carry flux directly and feed our existing
flux seam — they do NOT need IPF's MFM stream-element reconstruction.** IPF is
the hard one *because* it stores a mastering description that must be replayed
into MFM bitcells; STREAM/SCP/HFE already contain the physical layer.

### The universal flux seam (read from the code)

The engine already has a complete, tested flux pipeline. Everything below plugs
into it:

- **`Machine::insert_flux(const uint8_t* scp, size_t len)`** (`src/subcycle/machine.h`)
  — the sub-cycle FDC's media entry point. It consumes an **in-memory SCP byte
  buffer**.
- **`scp_from_mfm_tracks(const std::vector<t_mfm_track>&)`** (`src/ipf.h`) — the
  IPF path's MFM→SCP encoder (25 ns ticks, `kTicksPerCell = 80`, capturing
  `kMirrorRevs = 3` revolutions). Already clean-room, already ours.
- **`flux_scp_to_dsk` / `flux_decode_track_rev`** (`src/hw/flux.h`, spec
  [`flux-media.md`](flux-media.md)) — the pure/heap-free SCP-container parser +
  software PLL + IBM System-34 MFM decoder that recovers sectors for the
  `t_drive` view. **The SCP container is already fully parsed here.**

**Consequence:** the **SCP container is, in effect, konCePCja's native in-memory
flux interchange format.** The cheapest possible way to add any flux-native
format is to write a small front-end that produces an **in-memory SCP buffer**
(16-bit big-endian flux words, 25 ns base tick) and hands it to
`Machine::insert_flux` / `flux_scp_to_dsk`. No new PLL, no new MFM decoder, no
new sector scanner — those already exist and are tested. This collapses "add a
flux format" to "parse the container and re-emit flux words."

### Clean-room provenance rules (unchanged, non-negotiable)

1. Assess and implement from **PUBLIC FORMAT DOCUMENTATION only**. Do **not**
   read, port, or transliterate `src/capsimg/**` internals (non-commercial SPS
   licence) — capsimg may exist **only** as an uncommitted local test oracle.
2. Do **not** base code on **GPL** readers (greaseweazle, keirf/disk-utilities,
   SAMdisk, libhxcfe, GPL emulator loaders). Learn the FORMAT from docs; copy
   CODE only from permissive/public-domain sources, and even then prefer
   independent implementation.
3. **fluxfox** (`github.com/dbalsom/fluxfox`, MIT) may be cited as an independent
   cross-check only; retain the MIT notice if any snippet were ever reused
   (discouraged — prefer the vendor docs + our oracle).
4. Every source below is annotated **DOC = safe to learn the format from** vs
   **CODE = licence-gated**.

---

## 1. Verdict table

| Format | Flux-native? | Clean-room verdict | CPC relevance | Best public doc | Doc licence |
|---|---|---|---|---|---|
| **SCP** (SuperCard Pro) | **Yes** (raw flux) | **GREEN — near-free** (container already parsed by `flux_scp_to_dsk`) | Medium-High | SCP Image File Spec v2.5, Jim Drew / CBM Stuff | Public spec — DOC-safe |
| **KryoFlux STREAM** (`.raw`) | **Yes** (raw flux) | **GREEN** | High (workhorse of CPC 3″ preservation) | Stream Protocol Rev 1.1, DrCoolZic | Copyleft doc — DOC-safe |
| **HFE** (HxC, v1) | **No** — fixed-rate **bitcells** | **GREEN** (bitcells → flux is deterministic, PLL-free) | Medium (Gotek/HxC on real CPCs) | HFE File Format Rev 3.1, HxC2001 | Public vendor spec — DOC-safe |
| **HFE v3** (`HXCHFEV3`) | No — bitcells + opcodes | **AMBER** (opcode stream; only for weak-bit/variable-rate dumps) | Low-Medium | same Rev 3.1 doc | DOC-safe |
| **CT RAW** (`.ctr`/`.ct`/`.raw`) | Yes (bitcell timing) | **RED — effectively blocked** (codec undocumented anywhere public) | Low (submission-only; end users get IPF) | *none exists* | — |
| **A2R** (Applesauce 3.x) | Yes (raw flux) | AMBER (feasible, niche) | Low-Medium (Applesauce added CPC support) | A2R 3.x reference, applesaucefdc.com | Public spec — DOC-safe |
| **MOOF** | Yes | RED for CPC (Mac GCR only) | None | applesaucefdc.com | DOC-safe but irrelevant |
| **DMK** | No (bitcells) | GREEN if ever wanted, but | ~None (TRS-80/CoCo/MSX) | openMSX `DMK-Format-Details.txt` | DOC-safe but irrelevant |
| **EDSK / DSK** | — | **Already native** — do not re-add | Core | — | — |

**One-line summary:** SCP is nearly free (we already parse it), STREAM is the
highest-value new decoder and fully documented, HFE v1 is easy once you accept
it is a bitcell (not flux) container, CT RAW is a genuine documentation black
hole and should be dropped, and A2R is a nice-to-have.

---

## 2. Per-format assessments

### 2.1 SCP (SuperCard Pro) — **GREEN, near-free. Do this first (smallest diff).**

- **Best public DOC:** *SuperCard Pro Image File Specification* **v2.5**
  (2024-02-11), © 2012–2022 Jim Drew / CBM Stuff, published freely for
  interoperability at `cbmstuff.com/downloads/scp/scp_image_specs.txt`. **DOC —
  safe to learn from.** Cross-checkable against **fluxfox** (MIT) and the
  greaseweazle *wiki prose* (the greaseweazle *code* is GPL — off-limits).
- **Why near-free:** [`flux-media.md`](flux-media.md) §1 already specifies the
  SCP container byte-for-byte, and `flux_scp_to_dsk`/`flux_decode_track_rev`
  already parse it (little-endian headers, 16-bit **big-endian** flux words, the
  `0x0000`→+65536 overflow rule, `slot = cyl×2 + side`, 25 ns × (resolution+1)
  tick). `scp_from_mfm_tracks` already **writes** SCP. A `.scp` file loader is
  therefore: validate `"SCP"` magic → hand the bytes straight to
  `Machine::insert_flux` (flux path) and/or `flux_scp_to_dsk` (sector view). The
  work is a **file-type hook in `slotshandler.cpp`**, not a decoder.
- **What the new research adds beyond `flux-media.md`** (fold into the spec if we
  broaden SCP support): the **FLAGS** bit table (INDEX/TPI/RPM/TYPE/MODE/FOOTER/
  EXTENDED/FLUX-CREATOR), the **extension footer** (present iff FLAGS bit 5;
  ends in ASCII `"FPCS"` as the last 4 bytes; when a footer exists the header
  version byte is `0` and the real version lives in the footer), and the
  **disk-type** byte (`0x7_` = AMSTRAD, `disk_CPC`). The footer detail matters
  for robustness (don't trust the version byte blindly).
- **Hard parts:** essentially none — they are already solved in our tree. The
  only footgun is the **data-offset field being relative to the TDH, not the
  file** (already handled correctly in `flux-media.md` §1.3), and remembering
  the flux words are the one big-endian exception.
- **Verdict:** GREEN, minimal effort. Ship as the first "new format" to validate
  the load-path plumbing end-to-end with a format we already fully understand.

### 2.2 KryoFlux STREAM (`.raw`) — **GREEN. Highest-value new decoder.**

- **Best public DOC:** *KryoFlux Stream File Documentation* **Rev 1.1**
  (2013-12-01), Jean Louis-Guérin (DrCoolZic),
  `kryoflux.com/download/kryoflux_stream_protocol_rev1.1.pdf`. Marked
  **"Copyleft"**, byte-level complete. **DOC — safe to learn from.** Corroborated
  by the **softpres.org `kryoflux:stream`** wiki and cross-checked (constants
  only) against **fluxfox** (MIT).
- **Completeness — total at the byte level.** The full In-Stream-Buffer block
  header table (byte `0x00`–`0xFF`): Flux1 (`0x0E`–`0xFF`, value = header),
  Flux2 (`0x00`–`0x07`, `(hdr<<8)+b1`), Flux3 (`0x0C`, `(b1<<8)+b2`), Ovl16
  (`0x0B`, add `0x10000` to next flux, stackable), Nop1/2/3 (`0x08`/`0x09`/`0x0A`,
  skip 1/2/3 bytes), OOB (`0x0D`). OOB header = `0x0D` + type + **16-bit
  little-endian size**; types: `0x00` Invalid, `0x01` StreamInfo (8B), `0x02`
  Index (12B: StreamPosition/SampleCounter/IndexCounter), `0x03` StreamEnd (8B:
  pos + result code), `0x04` KFInfo (ASCII `name=value` incl. `sck=`/`ick=`),
  `0x0D` EOF (stop; do not advance by size). **Clocks:** `mck =
  ((18432000×73)/14)/2 = 48054857.14`, `sck = mck/2 = 24027428.57`, `ick =
  mck/16 = 3003428.57`; flux time = `SampleCounter / sck`; prefer the `sck`/`ick`
  carried in KFInfo over the defaults. (Note: an early web summary quoted wrong
  clock values — the correct, cross-verified ones are here.)
- **Hard parts:** exactly one — **index/flux alignment**. OOB blocks are
  asynchronous to the in-stream buffer; the Index block's StreamPosition points
  at the *next* flux (measured in the pre-OOB stream buffer) and the Sample
  Counter measures from the *previous* flux. The doc's "Analysis of Index
  Information" gives the **two-pass algorithm** (pass 1: collect flux + index +
  KFInfo arrays with stream positions; pass 2: bind indices to fluxes by
  position). Three documented edge cases: (1) Sample-Counter overflow right
  before an index, (2) index pointing *after* the last flux (append an inactive
  trailing flux, activate on confirmation), (3) index before any flux. RPM
  correction: `corrected = original × Expected_RPM / Actual_RPM`.
- **Multi-file set:** one `.raw` file **per track and side**, named
  `<base>TT.S.raw` (`track00.0.raw`, `track00.1.raw`, …). Point the loader at
  one member, glob siblings in the directory. Each file holds **multiple
  revolutions** (SPS wants ≥5 revs / ≥6 indexes; our mirror needs ≥3). Budget
  for directory-set handling, not just a single file.
- **Mapping to our seam:** convert each per-revolution flux run (index-to-index)
  into SCP 16-bit big-endian words at 25 ns ticks. `sck`-tick → 25 ns-tick is a
  rational rescale (`ticks_25ns = sample_counter × (1e9/25) / sck`); emit an
  in-memory SCP buffer with N revolutions and feed `insert_flux`. Alternatively
  extend `insert_flux` to take flux directly, but re-emitting SCP reuses 100% of
  the existing pipeline.
- **Verdict:** GREEN. Fully documented, one well-specified hard part, and it is
  where most CPC 3″ flux dumps actually live. **The flagship new decoder.**

### 2.3 HFE (HxC Floppy Emulator) — **GREEN for v1; AMBER for v3.**

- **Best public DOC:** *SDCard HxC Floppy Emulator HFE File format* **Rev 3.1**
  (2019-05-22), J-F Del Nero / HxC2001,
  `hxc2001.com/download/floppy_drive_emulator/HxC_Floppy_Emulator_HFE_file_format.pdf`
  (and the matching hxc2001.com wiki page). Byte-exact. **DOC — safe to learn
  from.** (libhxcfe source is GPL — off-limits; the *doc* is not.)
- **Completeness — total.** v1 header (512 B, all LE, unused = `0xFF`):
  signature `"HXCPICFE"`, `formatrevision` (0=v1,1=v2), `number_of_track`,
  `number_of_side` (*ignored by the emulator*), `track_encoding`
  (`0x00` ISOIBM_MFM … `0xFF` UNKNOWN), `bitRate` (kbit/s), `floppyRPM`
  (*ignored*), `floppyinterfacemode` (`0x06` = `CPC_DD_FLOPPYMODE`),
  `track_list_offset` (in 512-B blocks, normally 1), `write_allowed`,
  `single_step`, and per-side-of-track-0 alternate-encoding overrides. Track LUT
  (`pictrack[]` at `track_list_offset×512`): per track `{offset (u16, in 512-B
  blocks), track_len (u16, bytes)}`. Track data: 512-B blocks split **256 B side
  0 / 256 B side 1**, repeating; **bit order LSb-first**; content = raw MFM/FM
  **cell** stream; `cell_time = 1 / (bitRate × 2 × 1000)` s (2 µs at bitRate 250).
- **The one thing to internalise — HFE is NOT flux-native.** It stores
  **bitcells at a fixed cell rate**, one abstraction level above flux. This is
  the crucial architectural difference from SCP/STREAM. But converting *into*
  flux is deterministic and **PLL-free**: walk the side's bitstream LSb-first;
  a `1` bit = a flux reversal at that cell boundary, a `0` = none; the interval
  since the previous `1` is `(zeros_between + 1) × cell_time`. That yields the
  classic 2/3/4 µs MFM intervals directly — no clock recovery needed on the way
  in. So HFE feeds our SCP/flux seam via a trivial bitcell→transition expander,
  or (if we ever expose a cell-level seam) plugs in almost directly.
- **Hard parts (v1):** minor — side de-interleave (256-B halves), LSb-first bit
  order, deriving side presence from the LUT (not `number_of_side`), and the
  CPC `single_step`/double-step nuance (affects physical head stepping only, not
  bitstream decoding — treat unset as single-step).
- **HFE v3 (`HXCHFEV3`, `formatrevision` reset to 0):** structurally identical
  to v1 **plus** in-band opcodes (a whole byte `0xF0`–`0xFF` = command:
  `F0` NOP, `F1` SET INDEX, `F2 BB` SET BITRATE, `F3 LL` SKIP BITS, `F4` RAND
  weak-cells). Needed only for weak-bit/variable-rate/protected dumps; the vendor
  itself recommends against v3 for conventional use. **Detect by 8-byte
  signature; implement v1 first, add v3 opcodes only if a real protected HFE
  appears.** → AMBER.
- **CPC relevance:** HFE is a **Gotek/HxC delivery** format, not a
  preservation-grade capture (it is bitcells, so weak-bit fidelity is limited
  outside v3). Worth adding as a *convenience* input, below STREAM/SCP in
  priority. Interface mode `CPC_DD_FLOPPYMODE (0x06)` only tells a physical
  emulator how to wiggle drive signals — a decoder treats CPC images as generic
  Shugart MFM.
- **Verdict:** GREEN (v1). Cheap, well-documented, useful for Gotek users.

### 2.4 CT RAW (`.ctr` / `.ct` / `.raw`) — **RED. Effectively blocked. Drop it.**

This is the honest, non-hand-waved deep-dive the task demanded. Verdict:
**(c) effectively blocked** — there is **no public byte-level documentation of
either the CT RAW container or its delta-compression codec, anywhere**, and every
independent reader delegates to the proprietary CAPS library rather than
implementing the format. Adding it clean-room means full black-box
reverse-engineering at high effort and legal risk.

**What public material *does* exist (prose only, DOC-safe):** forum posts
(Atari-Forum "New IPF decoder capslib supports loading of raw files", KryoFlux
forums) describe CT RAW *functionally* — ~5 complete revolutions sampled per
track, **bitcell-level** timing (contrast KryoFlux = flux-transition level),
"very heavy delta compression specifically designed for bitcell data" that is
further zip-compressible, a "domain conversion for timing", "designed to run on
68020 CPUs". **Zero byte offsets, zero codec structure.** DTC emits `.ctr`/`.ct`
(default `.raw` — a real trap, since this collides with KryoFlux Stream `.raw`
and generic sector `.raw`).

**What was confirmed NOT to exist (each avenue checked and negative):**
- **softpres.org** documents only the KryoFlux *Stream* protocol — no CT RAW spec.
- **DrCoolZic / info-coach.fr IPF documentation** (v1.5/v1.6): fetched — **zero**
  mentions of "CT Raw", "CTRaw", or ".ctr". IPF only.
- **Archive Team / Just Solve** file-format wiki: no CT RAW page exists (KryoFlux
  and DMK have pages; CT RAW does not).
- **fluxfox (MIT)** supports KryoFlux Stream (RAW), IPF, SCP, PFI, MOOF, HFE,
  86F, MFI … but **does NOT implement CT RAW at all** — so there is **no
  permissive cross-check to learn from**. (Its "RAW" is KryoFlux Stream, not
  CT RAW — don't be misled by the extension collision.)
- **keirf/disk-utilities (GPL)** advertises SPS/CTRaw read/write **but** requires
  **CAPS library v5** installed for CT RAW — it does **not** decode the format,
  it calls the proprietary lib. Even ignoring its GPL status, there is nothing to
  clean-room from it.
- **HxC Floppy Emulator** — same: recognises `.ct`/`.ctr` only by delegating to
  `capsimg` ("put the caps library in the hxc exec folder").
- **capsimg `Codec/CTRawCodec.cpp` + `CTRawCodecDecompressor.cpp`** — the *only*
  actual implementation, and it is **OFF LIMITS** (non-commercial licence).

**If we ever pursued reverse-engineering (we should not, for CPC):** artifacts
needed = a corpus of sample `.ctr` files **plus** a known-good decode of the
*same physical disk* (as an oracle) **plus** the CAPS lib as a black-box. The
blocker is the corpus: CT RAW was a **submission-only** artifact, rarely in
public circulation — the preserved *output* for those disks ships as **IPF**, not
CT RAW, so oracle pairs barely exist. The codec is a bespoke delta scheme over
bitcell timing (non-standard), plus ~5-revolution framing with non-trivial
revolution-wrap handling. Realistic effort ≈ **weeks** of focused RE with **no
guarantee of edge-case exactness**, and the obvious shortcut (disassembling
capsimg) is exactly what its licence forbids.

**CPC relevance: low.** Everything that went through CAPS/SPS reaches end users
as IPF. CT RAW has no public spec, no independent implementation, and no
meaningful public file corpus.

**Recommendation:** **de-scope entirely.** File it as "blocked pending a
published spec from SPS", not as a real backlog item. Do not let it gate any
other format. If a CT RAW file ever genuinely needs loading, convert it to
STREAM/SCP out-of-band with a licensed local tool during preservation and ship
only the flux-format reader.

### 2.5 Other CPC-relevant formats (brief)

- **A2R (Applesauce 3.x)** — **AMBER, worth a later look.** Applesauce added
  Amstrad CPC support, so A2R can legitimately carry CPC flux. Chunk-based,
  extensible, openly documented (`applesaucefdc.com/a2r`, LoC FDD000643,
  DOC-safe). Flux-native → feeds our seam like STREAM/SCP. Niche vs IPF/EDSK;
  add after STREAM/SCP/HFE if demand appears.
- **MOOF** — **RED for CPC.** Macintosh GCR (400K/800K) only. Doc-safe but
  irrelevant. Don't add.
- **DMK (David M. Keil)** — **RED for CPC.** TRS-80/CoCo/MSX, hard-sectored;
  CPC uses standard MFM. Clean independent doc exists (openMSX
  `DMK-Format-Details.txt`) but no CPC reason to add it.
- **EDSK / DSK** — **already handled natively** (`slotshandler.cpp`). Do not
  re-add; the flux formats above *complement* it (flux → sectors → DSK view via
  `flux_scp_to_dsk`).
- **KryoFlux single-file DRAFT** — same encoding as the multi-file set,
  concatenated; transient. Covered by the STREAM decoder for free.

**Naming trap to carry forward:** three unrelated formats share `.raw` —
KryoFlux Stream, CT RAW, and generic sector dumps. **Content-sniff, never trust
the extension.**

---

## 3. Recommended implementation order (easiest / highest-value first)

Ordered by (doc quality × CPC value ÷ effort), exploiting the shared SCP seam:

1. **SCP file loader** — *near-free.* The container is already parsed
   (`flux_scp_to_dsk`) and written (`scp_from_mfm_tracks`). Adds a
   `slotshandler.cpp` hook + the FLAGS/footer robustness from §2.1. Proves the
   load-path plumbing with a format we fully control. **(GREEN, days.)**
2. **KryoFlux STREAM decoder** — *highest value.* New parser (block table + OOB +
   two-pass index alignment) that re-emits an in-memory SCP buffer → `insert_flux`.
   Most CPC flux dumps live here. **(GREEN, the main effort — well-scoped.)**
3. **HFE v1 loader** — *cheap convenience.* Header + LUT + side-deinterleave +
   LSb-first bitcell→flux expander → SCP buffer. Serves Gotek/HxC users.
   **(GREEN.)**
4. **A2R** — optional, niche; only if CPC A2R files show up. **(AMBER.)**
5. **HFE v3 opcodes** — only when a real weak-bit/variable-rate HFE appears.
   **(AMBER.)**
6. **CT RAW** — **do not implement.** Blocked; convert out-of-band if ever
   needed. **(RED.)**

All of steps 1–4 converge on the **same output contract**: produce an in-memory
SCP byte buffer (16-bit BE flux words, 25 ns ticks, ≥3 revolutions where the
capture has them) and call `Machine::insert_flux` for the flux path /
`flux_scp_to_dsk` for the sector view. Define that "front-end → SCP buffer"
interface **once** and every format after SCP is an independent, parallelisable
work unit against it.

### De-risking (shared)

- **Oracle harness:** reuse the existing fixture-gated pattern
  (`KONCEPCJA_REAL_SCP` in `test/hw/fdc_test.cpp`,
  `FdcFlux.DecodesARealScpCaptureWhenProvided`). Add
  `KONCEPCJA_REAL_STREAM` / `KONCEPCJA_REAL_HFE` gated tests: decode a real
  capture → SCP buffer → `flux_scp_to_dsk` → compare recovered sectors to the
  title's DSK release. Never commit preservation dumps.
- **Cross-format oracle:** where the *same* disk exists as both STREAM and SCP
  (common — Greaseweazle/KryoFlux can emit either), decode both and assert the
  recovered sector sets match. This validates the STREAM index-alignment against
  an independently-produced SCP without any licensed code.
- **Round-trip vectors:** hand-build tiny in-code fixtures (a few flux words / a
  one-track HFE) with known sector content, decode, assert — always-on CI, no
  external files, mirroring how `ipf_mirror_test.cpp` hand-builds `t_mfm_track`.
- **Big-endian flux words** (SCP/STREAM emit) — explicit shift-assembly, unit-test
  the overflow (`0x0000`→+65536) accumulation.

---

## 4. Spec skeletons for the GREEN formats

Each is ready to expand into a full `docs/hardware/<fmt>-format.md` in the style
of [`ipf-format.md`](ipf-format.md) — self-contained enough to implement without
the source PDFs. Section headings + what each must pin down.

### `docs/hardware/scp-format.md` (or fold into `flux-media.md`)

Most of this already exists in [`flux-media.md`](flux-media.md) §1; a dedicated
spec would add the loader-facing details.

1. **Scope & provenance** — SCP spec v2.5 (Jim Drew / CBM Stuff), public,
   DOC-safe; fluxfox (MIT) cross-check; greaseweazle *code* GPL (off-limits).
   Note SCP is already our in-memory flux interchange format.
2. **File header (16 B)** — magic `"SCP"`, version nibbles, disk type
   (`0x7_` AMSTRAD/`disk_CPC`), revolution count (1–5), start/end track, FLAGS,
   bit-cell width (0/16), heads (0 both / 1 side0 / 2 side1), resolution
   (`(N+1)×25 ns`), 32-bit LE checksum (unverified).
3. **FLAGS byte** — bit table: 0 INDEX, 1 TPI, 2 RPM(300/360), 3 TYPE
   (normalized), 4 MODE (r/w), 5 FOOTER, 6 EXTENDED, 7 FLUX-CREATOR.
4. **Track offset table** — 168 × 32-bit LE absolute file offsets from `0x10`;
   `0` = absent; `slot = cyl×2 + side` (+ the legacy consecutive layout, per
   `flux-media.md` §1.2).
5. **Track Data Header** — `"TRK"` + track#; per-revolution 3×u32 LE
   (duration in 25 ns units, length in flux words, **offset relative to the
   TDH** — the classic bug).
6. **Flux data** — 16-bit **big-endian** words, `0x0000`→+65536 overflow.
7. **Extension footer** — present iff FLAGS bit 5; string-pointer table +
   timestamps + trailing `"FPCS"`; header version byte is `0` when footer
   present. Robustness: detect footer before trusting version.
8. **Mapping to our seam** — the file *is* an SCP buffer: validate + hand to
   `insert_flux` / `flux_scp_to_dsk`. Loader hook in `slotshandler.cpp`.
9. **Test vectors** — the existing `flux-media.md` fixtures + a footer-bearing
   sample.

### `docs/hardware/kryoflux-stream-format.md`

1. **Scope & provenance** — Stream Protocol Rev 1.1 (DrCoolZic), Copyleft,
   DOC-safe; softpres wiki corroboration; fluxfox (MIT) constants cross-check.
   Flux-native → re-emit SCP buffer, no MFM reconstruction.
2. **Clocks** — mck/sck/ick exact defaults (48054857.14 / 24027428.57 /
   3003428.57); flux time = SampleCounter/sck; KFInfo `sck=`/`ick=` override.
3. **Block header table** — the full `0x00`–`0xFF` map: Flux1/2/3, Ovl16
   (stackable +0x10000), Nop1/2/3, OOB — with byte layouts and pointer-advance
   lengths.
4. **Flux value reconstruction** — the three flux encodings + Ovl16 32-bit
   accumulator; encoder's shortest-form preference (decoder must accept all).
5. **OOB blocks** — 4-byte header (`0x0D`,type,16-bit **LE** size); field tables
   for Invalid/StreamInfo/Index/StreamEnd/KFInfo/EOF; EOF stops parsing.
6. **Index analysis** — StreamPosition (points at *next* flux, in the pre-OOB
   buffer) vs Sample Counter (from *previous* flux) vs free-running Index
   Counter; the **two-pass** algorithm; the **three edge cases**; RPM
   interpolation (`× Expected/Actual`).
7. **Multi-file `.raw` set** — `<base>TT.S.raw` naming; glob siblings;
   revolutions between indexes (≥3 for the mirror).
8. **Mapping to our flux structs** — sck-tick → 25 ns-tick rational rescale;
   per-revolution SCP words; N-revolution in-memory SCP buffer → `insert_flux`.
9. **Test vectors** — a small hand-built stream (a few Flux1/Flux2/Ovl16 blocks +
   one Index OOB) with the expected flux array and index binding.

### `docs/hardware/hfe-format.md`

1. **Scope & provenance** — HFE File Format Rev 3.1 (HxC2001), public vendor
   spec, DOC-safe; libhxcfe code GPL (off-limits). **Key caveat: HFE stores
   bitcells at a fixed cell rate, NOT flux** — one level above flux.
2. **v1 header (512 B, LE, unused `0xFF`)** — full `picfileformatheader` field
   table: signature `"HXCPICFE"`, formatrevision, track/side counts (side count
   *ignored*), track_encoding enum, bitRate, floppyRPM (*ignored*),
   floppyinterfacemode (`0x06` CPC), track_list_offset, write_allowed,
   single_step, track0 alt-encoding overrides.
3. **Track LUT (`pictrack[]`)** — at `track_list_offset×512`; per track
   `{offset (u16, ×512 = byte offset), track_len (u16, bytes)}`.
4. **Track data encoding** — 512-B blocks split 256 B side0 / 256 B side1,
   repeating; **LSb-first** bit order; raw MFM/FM cell content;
   `cell_time = 1/(bitRate×2×1000)` s (2 µs at 250).
5. **Bitcells → flux (the load-bearing section)** — deterministic PLL-free
   expansion: per side, LSb-first, `1`=reversal, interval =
   `(zeros_between+1)×cell_time`; emit SCP words at 25 ns ticks → `insert_flux`.
   State plainly that HFE needs this expansion step (unlike SCP/STREAM) and needs
   **no** clock recovery on input.
6. **CPC nuances** — derive side presence from the LUT, not `number_of_side`;
   `single_step`/double-step affects head stepping only; interface mode is
   emulator-only metadata.
7. **v3 (`HXCHFEV3`) appendix** — signature/formatrevision detection; in-band
   opcode table (`F0`–`F4`, `SKIP BITS` re-aligns sub-byte); scope: weak-bit /
   variable-rate only; deferred until a real protected HFE appears.
8. **Test vectors** — a one-track, one-side hand-built HFE with a known sector,
   decoded to the same sector via the bitcell→flux→`flux_scp_to_dsk` chain.

*(No spec skeleton for CT RAW: it is blocked (§2.4). A stub
`docs/hardware/ct-raw-format.md` should record only the provenance verdict, the
"no public spec exists" finding, and the de-scope decision — with an explicit
"RE placeholder, never from `Codec/CTRawCodec*`" note — so the question is not
re-litigated.)*

---

## 5. Bottom line

- **SCP: GREEN, near-free.** We already parse and write it; a loader is a
  file-type hook plus footer/FLAGS robustness. Ship first.
- **KryoFlux STREAM: GREEN, highest value.** Fully documented (Rev 1.1); one
  well-specified hard part (two-pass index alignment); it is where most CPC flux
  dumps live. The flagship new decoder.
- **HFE v1: GREEN, cheap.** Fully documented; the only conceptual step is
  accepting it is a **bitcell** container and expanding cells → flux (PLL-free).
  v3 opcodes are AMBER, deferred.
- **CT RAW: RED, blocked.** No public byte-level spec of container or codec
  exists; every independent reader delegates to the closed CAPS lib; fluxfox
  doesn't implement it; RE would be weeks with no exactness guarantee and low CPC
  payoff. **De-scope.**
- **A2R AMBER (niche), MOOF/DMK RED (not CPC), EDSK already native.**
- **The unifying win:** the **SCP in-memory buffer + `Machine::insert_flux` +
  `flux_scp_to_dsk`** is a universal flux seam. Every GREEN/AMBER flux format
  reduces to "parse container → emit SCP words", reusing the existing PLL, MFM
  decoder, sector scanner, and weak-bit machinery untouched.

---

## Appendix — public sources & licences

| Source | Format | Type | Licence / status | Use |
|---|---|---|---|---|
| *SuperCard Pro Image File Specification* v2.5 (Jim Drew / CBM Stuff), `cbmstuff.com/downloads/scp/scp_image_specs.txt` | SCP | DOC | Public spec | **Primary SCP spec — safe to learn.** |
| *KryoFlux Stream File Documentation* Rev 1.1 (DrCoolZic), `kryoflux.com/download/kryoflux_stream_protocol_rev1.1.pdf` | STREAM | DOC | "Copyleft" documentation | **Primary STREAM spec — safe to learn.** |
| softpres.org `kryoflux:stream` | STREAM | DOC | SPS public wiki | Corroboration. |
| *SDCard HxC Floppy Emulator HFE File format* Rev 3.1 (HxC2001), `hxc2001.com/download/floppy_drive_emulator/HxC_Floppy_Emulator_HFE_file_format.pdf` | HFE v1/v3 | DOC | Public vendor spec | **Primary HFE spec — safe to learn.** |
| *A2R 3.x reference*, `applesaucefdc.com/a2r`; LoC FDD000643 | A2R | DOC | Public spec | If A2R is pursued — safe to learn. |
| openMSX `doc/DMK-Format-Details.txt` | DMK | DOC | Public (independent) | Reference only; not CPC. |
| Archive Team / Just Solve, LoC FDD registry | various | DOC | Public wikis | Background / cross-check. |
| **fluxfox** (`github.com/dbalsom/fluxfox`) | SCP/STREAM/HFE/A2R (not CT RAW) | CODE | **MIT** | Independent cross-check only; prefer independent implementation + oracle. |
| greaseweazle, keirf/disk-utilities, SAMdisk, libhxcfe | various | CODE | **GPL** | **OFF-LIMITS** — copyleft. Read the *wikis*, not the code. |
| `src/capsimg/**` — SPS Decoder Library (incl. `CTRawCodec*`) | IPF/CT RAW/STREAM | CODE | **Non-commercial (KryoFlux/SPS)** | **OFF-LIMITS.** Local uncommitted test oracle only; never a code reference; the *only* CT RAW implementation and it stays sealed. |

**No public documentation of CT RAW's container or delta codec was found in any
DOC-class source — that is the definitive finding of §2.4, not an omission.**
