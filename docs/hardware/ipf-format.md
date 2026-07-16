# IPF container format — clean-room decoder specification

Status: **implementation spec** (authoritative build target). 2026-07-13.

This is the single spec a clean-room IPF decoder for konCePCja is built
against. It consolidates
[`ipf-decoder-cleanroom-research.md`](ipf-decoder-cleanroom-research.md)
(feasibility, provenance rules, phase plan) and
[`ipf-decoder-contract.md`](ipf-decoder-contract.md) (the internal API and
type contract) with the byte-level format extracted from the public
DrCoolZic documentation (§10). Companion specs:
[`flux-media.md`](flux-media.md) (SCP container + PLL the sub-cycle FDC
consumes) and [`fdc-device.md`](fdc-device.md).

**Provenance rule (non-negotiable):** everything in this document is derived
from *Interchangeable Preservation Format (IPF) Documentation* v1.6 by Jean
Louis-Guérin (DrCoolZic), April 2018 — a community format write-up — plus our
own glue code (`src/ipf.cpp`/`src/ipf.h`, ours) and independent verification
(the CRC check in §6 was verified computationally against the doc's worked
example, not against any library). `src/capsimg/**` internals were **not**
read and must never be read by implementers of this spec. GPL emulator
readers are equally off-limits. fluxfox (MIT) may be consulted as an
*independent cross-check* of an ambiguous point, cited when used — prefer
resolving ambiguity with the oracle harness (§8.3) instead. The DrCoolZic doc
itself warns it "inevitably must contain errors": treat this spec as a
hypothesis wherever marked *(oracle-verify)*, and the local capsimg oracle as
ground truth.

---

## 1. Overview & scope

### 1.1 What IPF is

IPF (Interchangeable Preservation Format) is the Software Preservation
Society's floppy-disk preservation container. Unlike DSK (decoded sectors) or
SCP/STREAM (raw flux timings), an IPF stores a *mastering description* of
each track: a list of blocks (≈ sectors), each described by stream elements
(sync marks, data bytes, gap fill, fuzzy regions) that a decoder replays into
the raw MFM bitcell stream as it was written by professional duplication
equipment. Timing is **not** stored as measurements; density is either
uniform ("Auto", cell size fitted to track size) or named by a protection
descriptor (Copylock, Speedlock, …).

The container is IFF-like: big-endian records, each with a 4-char type, a
length, and a CRC32.

### 1.2 v1 scope

**In scope (v1):**

- The IPF container: `CAPS`, `INFO`, `IMGE`, `DATA` records (§2), with
  unknown-record skip and IPX (`CTEI`/`CTEX`) tolerance.
- **SPS encoder only** (`INFO.encoderType == 2`) — the encoder used by all
  modern preservation output, covering essentially all CPC IPFs in
  circulation. `encoderType == 1` (CAPS, the historical first encoder, whose
  data-area encoding the public doc describes only thinly) is **detected and
  rejected** with a clear log + `ERR_DSK_INVALID` (fail-soft; Phase C
  work — §9).
- **Auto density** (`IMGE.density == 2`) fully modelled at uniform 2 µs
  cells. Noise (1) treated as unformatted. Densities 3–9 (explicit
  protection descriptors) decode with uniform cells + a warning (§8.2).
- Data-area stream elements: Sync/Data/Gap/Raw/Fuzzy, gap
  GapLength/SampleLength, forward/backward/default gap fill (§3).
- MFM reconstruction into `t_mfm_rev` raw bitcells (§4) and the legacy
  `t_drive` sector view via the existing (already ours) `ReadTrack` scanner
  (§5).
- Fuzzy (weak-bit) regions regenerated per revolution for the 3-revolution
  flux mirror (§4.5).

**Out of scope (v1), and why:**

- **KryoFlux STREAM** (`.raw` sets) and **CT RAW** (`.ctr`): today's loader
  gates on the 4-byte `"CAPS"` magic before any decoding
  (`src/ipf.cpp`, see contract §4), so *only genuine IPF files are ever
  decoded by this codebase* — STREAM/CT-RAW support would be new
  functionality, not parity. CT RAW's delta-compression codec is additionally
  publicly undocumented (research §2.2) and would be a reverse-engineering
  project. STREAM has its own future spec slot
  (`kryoflux-stream-format.md`, research §6).
- IPX analyzer payloads (`CTEI`/`CTEX` record *contents*) — parsed past, not
  interpreted.
- The CAPS FDC-emulator half of the old library API — never called by us
  (contract §3).
- Write-back / IPF authoring.

### 1.3 CPC relevance

Platform id 4 in `INFO.platforms` is **Amstrad CPC**. The overwhelming
majority of CPC preservation dumps in circulation are IPF; mainstream CPC
titles are SPS-encoded with Auto density, which is exactly the v1 target.
The sub-cycle engine (engine=1) plays the decoded track as flux via the
existing `ipf_mirror_to_scp` → `scp_from_mfm_tracks` → `Machine::insert_flux`
chain, which is what makes weak bits and read-track protections behave like
real hardware.

---

## 2. Container structure

### 2.1 File model, endianness, traversal

- The file is a sequence of **records**. A record = fixed 12-byte **record
  header** + a type-specific block + (DATA records only) an **Extra Data
  Block**.
- **All integer fields are 4-byte big-endian** unless stated otherwise.
  There are no alignment pads; blocks are packed.
- Record order as written by the analyzer: one `CAPS`, one `INFO`, then all
  `IMGE` records (one per track/side), then all `DATA` records (one per
  `IMGE`). **Do not rely on order for matching**: an `IMGE` and its `DATA`
  record pair via equal `dataKey` values. Parse pass: index IMGE records by
  `dataKey`, then attach each DATA record by lookup.
- **Traversal:** next record start = this record start + `header.length`,
  **except** DATA records, whose Extra Data Block lies *outside*
  `header.length`: next record start = (record start + 28) +
  `dataBlock.length` (§2.5).
- **Robustness requirements** (hostile input is a load path):
  - Every record header read must fit the remaining file; `length >= 12`.
  - Unknown record types (`DUMP`, `TRCK`, anything else) are skipped via
    `length`. `CTEI` (76 bytes) and `CTEX` (44 bytes) are skipped the same
    way — tolerate their presence (IPX files share the `CAPS` magic).
  - All offsets/sizes inside a DATA record's Extra Data Block must be
    bounds-checked against `dataBlock.length` before use.
  - A failed record CRC ⇒ reject the file (`ERR_DSK_INVALID`), log which
    record.

### 2.2 Record header (12 bytes, all records)

| off | size | field | notes |
|----:|-----:|-------|-------|
| 0 | 4 | `type` | 4 ASCII chars, uppercase: `CAPS`, `INFO`, `IMGE`, `DATA`, `CTEI`, `CTEX` (also reserved: `DUMP`, `TRCK`) |
| 4 | 4 | `length` | size in bytes of the **complete record** = header + type-specific block. Does **not** include a DATA record's Extra Data Block. |
| 8 | 4 | `crc` | CRC32 (§6) over the complete `length` bytes of the record, computed with this field temporarily = 0 |

### 2.3 CAPS record (12 bytes)

Header only (`type == "CAPS"`, `length == 12`), no block. Must be the first
record; its presence at offset 0 is the file-magic gate `ipf_load` already
applies (first 4 bytes literally `"CAPS"`).

### 2.4 INFO record (96 bytes = 12 header + 84 block)

Offsets below are relative to the start of the block (record start + 12).

| off | size | field | values / notes |
|----:|-----:|-------|----------------|
| 0 | 4 | `mediaType` | 1 = floppy disk (only defined value; 0 = unknown) |
| 4 | 4 | `encoderType` | 1 = CAPS encoder, 2 = SPS encoder. **Selects the block-descriptor variant (§2.7) and the data-area model.** v1 requires 2. |
| 8 | 4 | `encoderRev` | currently always 1 |
| 12 | 4 | `fileKey` | image database key (same for all disks of one game) |
| 16 | 4 | `fileRev` | file revision, initially 1 |
| 20 | 4 | `origin` | CRC32 of the original `.ctr` source file |
| 24 | 4 | `minTrack` | lowest cylinder, usually 0 |
| 28 | 4 | `maxTrack` | highest cylinder, usually 83 |
| 32 | 4 | `minSide` | lowest head, usually 0 |
| 36 | 4 | `maxSide` | highest head, usually 1 |
| 40 | 4 | `creationDate` | decimal-packed `yyyy*10000 + mm*100 + dd` *(inferred from the worked example, e.g. 2015/02/11; not consumed by us — non-load-bearing)* |
| 44 | 4 | `creationTime` | decimal-packed `hh*10000000 + mm*100000 + ss*1000 + tick` *(same caveat; example 13:33:38.935)* |
| 48 | 16 | `platforms[4]` | four 4-byte ids: 0 None, 1 Amiga, 2 Atari ST, 3 PC, **4 Amstrad CPC**, 5 Spectrum, 6 Sam Coupé, 7 Archimedes, 8 C64, 9 Atari 8-bit |
| 64 | 4 | `diskNumber` | disk # in a multi-disc release, else 0 |
| 68 | 4 | `creatorId` | image creator id |
| 72 | 12 | `reserved[3]` | |

Geometry consumed by us: `minTrack`, `maxTrack`, `minSide`, `maxSide`
(the loop bounds `ipf_load` currently pulls from `CAPSGetImageInfo`, contract
§3). Everything else is parsed for validation/logging only.

### 2.5 IMGE record (80 bytes = 12 header + 68 block)

One per (track, side). Block fields:

| off | size | field | values / notes |
|----:|-----:|-------|----------------|
| 0 | 4 | `track` | cylinder number |
| 4 | 4 | `side` | head number (0/1) |
| 8 | 4 | `density` | 0 Unknown; **1 Noise** (unformatted, random cell size); **2 Auto** (cell size fitted to track size — the v1 target); 3 Copylock Amiga; 4 Copylock Amiga (new); 5 Copylock ST; 6 Speedlock Amiga; 7 Old Speedlock Amiga; 8 Adam Brierley Amiga; 9 Adam Brierley density-key Amiga |
| 12 | 4 | `signalType` | 1 = 2 µs cells (only defined value; 0 = unknown) |
| 16 | 4 | `trackBytes` | rounded byte count of the decoded (clock+data) track |
| 20 | 4 | `startBytePos` | rounded `startBitPos` in bytes — redundant, ignore |
| 24 | 4 | `startBitPos` | start position **in raw (clock+data) bits from the index** of the first sync bit of the first block. The track write splice sits between the last block and this point. |
| 28 | 4 | `dataBits` | decoded data bits (clock+data) **including intra-block gap bits** (the gap inside a block, between ID and data areas). Equals the sum of `dataBits` over this track's block descriptors. |
| 32 | 4 | `gapBits` | decoded gap bits (clock+data) **between** blocks only (inter-block). Equals the sum of `gapBits` over the block descriptors. |
| 36 | 4 | `trackBits` | `dataBits + gapBits` — redundant, use for validation only |
| 40 | 4 | `blockCount` | number of blocks (≈ sectors) on this track = number of Block Descriptors in the matching DATA record |
| 44 | 4 | `encoderProcess` | 0 |
| 48 | 4 | `trackFlags` | bit 0 = **Fuzzy** (track contains fuzzy/weak bits — drives the multi-revolution behavior, §4.5; this is the `flakey` flag of the contract) |
| 52 | 4 | `dataKey` | matches the `dataKey` of exactly one DATA record |
| 56 | 12 | `reserved[3]` | |

### 2.6 DATA record (28 bytes = 12 header + 16 block, + Extra Data Block)

Block fields:

| off | size | field | notes |
|----:|-----:|-------|-------|
| 0 | 4 | `length` | size in bytes of the **Extra Data Block** that follows the 28-byte record (0 = none, e.g. an unformatted track) |
| 4 | 4 | `bitSize` | `length * 8` — redundant, ignore |
| 8 | 4 | `crc` | CRC32 (§6) of the Extra Data Block, computed plain (no field zeroing — the field is outside the hashed range) |
| 12 | 4 | `dataKey` | matches the owning IMGE record |

The record header's own `crc` covers only the 28 bytes (with the header CRC
field zeroed), *not* the extra block — the extra block has this separate CRC.

**Extra Data Block layout:** `blockCount` (from the matching IMGE) Block
Descriptors of 32 bytes each, back-to-back from offset 0, followed by the
**Data Area** (§3). All `dataOffset`/`gapOffset` values in block descriptors
are **relative to the start of the Extra Data Block**.

### 2.7 Block Descriptor (32 bytes)

One per block, inside the Extra Data Block. Fields 8–15 differ by
`INFO.encoderType`:

| off | size | field (SPS, encoderType=2) | field (CAPS, encoderType=1) |
|----:|-----:|------------------------------|------------------------------|
| 0 | 4 | `dataBits` — raw (clock+data) bit size of the decoded block data: sync + data + **intra**-block gap | same |
| 4 | 4 | `gapBits` — raw bit size of the decoded **inter**-block gap that follows this block | same |
| 8 | 4 | `gapOffset` — offset of this block's first gap stream element in the Extra Data Block (0 = no gap stream) | `dataBytes` — decoded data size, rounded |
| 12 | 4 | `cellType` — 1 = 2 µs cell | `gapBytes` — decoded gap size, rounded |
| 16 | 4 | `encoderType` (block-level) — 0 unknown, **1 MFM**, 2 Raw (test only) | same |
| 20 | 4 | `blockFlags` — bit 0 **ForwardGap** (forward gap stream list exists), bit 1 **BackwardGap** (backward list exists), bit 2 **DataInBit** (data-stream sample sizes are in **bits** when set, **bytes** when clear — applies to *data* stream elements only; gap sizes are always bits). Zero/ignored when INFO encoderType = 1. | same position, ignored |
| 24 | 4 | `gapDefault` — default gap fill value (used when no gap stream exists, §3.2 case 00) | same |
| 28 | 4 | `dataOffset` — offset of this block's first data stream element in the Extra Data Block | same |

Block ordering: descriptor 0 is the first block written after the write gate
turns on — normally the first block after the index pulse, but "shifted
track" protections may place it anywhere; `IMGE.startBitPos` gives its
angular position (§4.4). Validation identities (assert while parsing):
`Σ blocks.dataBits == IMGE.dataBits`, `Σ blocks.gapBits == IMGE.gapBits`,
`IMGE.trackBits == IMGE.dataBits + IMGE.gapBits`.

### 2.8 CTEI / CTEX records (IPX; skip)

For completeness (they can appear in `.ipx` files, which also start with a
CAPS record): `CTEI` = 76 bytes (12 + 64: `releaseCrc`, `analyzerRev`, 14×4
reserved), `CTEX` = 44 bytes (12 + 32: `track`, `side`, `densityType`,
`formatId`, `fix`, `trackSize`, reserved). We skip both via `header.length`
and never interpret the payloads.

---

## 3. Data Area — stream elements (SPS encoder)

The Data Area starts immediately after the last Block Descriptor and holds,
freely ordered (the analyzer writes all gap lists first, then all data
lists), one **data stream element list** per block (at `dataOffset`) and
zero/one/two **gap stream element lists** per block (at `gapOffset`). Never
scan linearly; always seek via the descriptor offsets.

### 3.1 Data stream elements

Present iff the block's `dataBits != 0`. `dataOffset` points at the first
element. Each element:

```
dataHead    1 byte:   bits 7-5 = dataSizeWidth  (byte count of dataSize, 1-7)
                      bits 4-0 = dataType:
                                 1 = Sync   2 = Data   3 = Gap
                                 4 = Raw    5 = Fuzzy
dataSize    dataSizeWidth bytes, big-endian:
                      sample size, in BYTES when blockFlags.DataInBit == 0,
                      in BITS when == 1
dataSample  dataSize bytes/bits: the samples. ABSENT when dataType == 5
                      (Fuzzy): dataSize then gives the length of the fuzzy
                      region and the consumer generates random data of that
                      size (§4.3).
```

The list terminates at an element whose `dataHead` is `0x00` (null). A
sample sized in bits that is not a multiple of 8 occupies
`ceil(bits/8)` bytes, MSB-first, remaining low bits of the last byte unused
*(oracle-verify — bit-sized samples are rare; the worked example uses byte
sizes throughout)*.

Element semantics (how each turns into raw MFM bits is §4.2):

| type | name | sample content | raw-bit contribution |
|-----:|------|----------------|----------------------|
| 1 | Sync | **already-encoded raw MFM bits** (e.g. `44 89` = `$4489`, the A1 sync mark with missing clock) | sample bits, verbatim |
| 2 | Data | decoded data bytes/bits (ID fields, DAMs, payload, CRCs) | 2 × sample bits (MFM) |
| 3 | Gap | decoded gap fill inside the block (intra-block gap, e.g. `4E…00` between a sector's ID and data fields) | 2 × sample bits (MFM) |
| 4 | Raw | raw MFM bits, verbatim (rare; block encoderType 2 "Raw" is test-only) | sample bits, verbatim |
| 5 | Fuzzy | none stored | 2 × dataSize bits of freshly generated random data, MFM-encoded (§4.3) |

Evidence for the "Sync/Raw verbatim, Data/Gap/Fuzzy ×2" rule: the worked
example's block-0 accounting (§7.4) — `48 + 2·56 + 2·272 + 48 + 2·4120 =
8992 = blockDescriptor.dataBits` — only balances under this rule. This is
the single most load-bearing inference in this spec; it is locked by the
regression vectors in §7.

### 3.2 Gap stream elements

Present iff `INFO.encoderType == 2` **and** the block's `gapBits != 0`
**and** `blockFlags & 3 != 0`. `gapOffset` points at the first element.
Each element:

```
gapHead     1 byte:   bits 7-5 = gapSizeWidth (byte count of gapSize)
                      bits 4-0 = gapElemType: 1 = GapLength, 2 = SampleLength
gapSize     gapSizeWidth bytes, big-endian (unit: BITS, always — DataInBit
                      does not apply to gap elements)
gapSample   gapSize bits, only when gapElemType == 2: the sample value
```

Each list terminates at a null `gapHead`. Elements pair up:

- **GapLength (1) followed by SampleLength (2):** repeat the sample until
  *GapLength's* `gapSize` bits (of decoded gap data, i.e. filling
  `2 × gapSize` raw bits) have been produced.
  - *Interpretation note:* the doc's prose says GapLength's size is "the
    number of times the gapSample must be repeated", but its own worked
    example prints `Gap_Length 192 bits (24 bytes)` for a repeated 8-bit
    `4E` sample and balances the descriptor's `gapBits = 512` only as
    `2·(192 + 64)` — i.e. the value is a **length in bits**, not a repeat
    count. (The two readings coincide only for 1-bit samples.) We adopt the
    length-in-bits reading. *(oracle-verify)*
- **SampleLength alone (no preceding GapLength):** repeat the sample *as
  many times as necessary* to fill whatever remains of the block's
  `gapBits`.

**Which gap a block's list fills:** the inter-block gap *after* this block's
data, up to the next block's data (wrapping from the last block to the first
across the index — the worked example's last block explicitly fills bytes
"in front of the first sector", §7.6).

Fill directions, selected by `blockFlags` bits 1..0:

| bits 1-0 | meaning | fill algorithm |
|---------:|---------|----------------|
| `00` | no gap stream | fill the `gapBits` region by repeating the descriptor's `gapDefault` byte (MFM-encoded), **forward and backward simultaneously**; the write splice lands at the exact middle of the gap |
| `01` | forward list only | decode the forward list; emit from the end of this block's data forward; if the list specifies fewer bits than `gapBits`, loop its (last unrepeated) sample until full |
| `10` | backward list only | decode the backward list; emit from the beginning of the **next** block's data backward; loop to fill as above (worked example §7.6: 512 bits specified out of 2280 — "the decoder needs to loop the gap samples toward the beginning of the track") |
| `11` | both lists | forward list fills from the front, backward list from the back; when both are fully counted they abut; when both are open-ended (no GapLength) they fill simultaneously and the **track write splice** is where they meet |

When both lists exist they are stored back-to-back at `gapOffset`: forward
list first, terminated by its null head, then the backward list, terminated
by its null head *(layout inferred from the single `gapOffset` field and the
worked example's dump order (forward elements printed before backward at
ascending file offsets); oracle-verify)*.

The write-splice position itself is not represented in our output — v1 emits
a fully filled gap; the splice matters only as the definition of where
forward/backward fills stop.

### 3.3 Decoding pseudo-code

```text
parse_block(desc, extra):
    elems = []
    if desc.dataBits:
        p = extra + desc.dataOffset
        while (head = *p++) != 0:
            width = head >> 5;  type = head & 0x1F
            size  = read_be(p, width); p += width
            if type == FUZZY:  elems.push({FUZZY, size});        // no sample
            else:
                nbytes = DataInBit ? ceil(size/8) : size
                elems.push({type, size, sample = p[0..nbytes)}); p += nbytes
    fwd, bwd = [], []
    if desc.gapBits and (desc.blockFlags & 3):
        p = extra + desc.gapOffset
        for list in selected_lists(desc.blockFlags):   // fwd first, then bwd
            while (head = *p++) != 0:
                width = head >> 5; kind = head & 0x1F
                size  = read_be(p, width); p += width
                if kind == SAMPLELENGTH: sample = read_bits(p, size)
                list.push(...)
    return {elems, fwd, bwd}
```

Every pointer advance bounds-checks against `extra + dataRecord.length`.

---

## 4. MFM reconstruction — elements → track bitcell stream

The decoder's product per (cylinder, head, revolution) is exactly what the
old library handed us: the **raw MFM bitcell stream as written on the disc**
— clock and data bits interleaved, MSB-first packed — as `t_mfm_rev`
(`bits[]` + `nbits`, `src/ipf.h`). One raw bit = one 2 µs bitcell; a `1`
means a flux transition in that cell. Nothing downstream changes:
`ReadTrack` scans this stream for the sector view, and
`scp_from_mfm_tracks` turns `1`-cells into flux transitions at
`kTicksPerCell = 80` × 25 ns.

### 4.1 MFM encoding rule (for Data/Gap/Fuzzy samples)

For each decoded data bit `D[n]` (MSB-first within each sample byte), emit a
clock bit then the data bit:

```
C[n] = !D[n-1] AND !D[n]        // 1 only between two zero data bits
raw  = C[0] D[0] C[1] D[1] ...
```

`D[-1]` (the "previous data bit" for the first bit of an element) carries
across element boundaries within a block, **and across the block/gap/block
sequence of the whole track**: initialize it from the last data bit already
emitted into the raw stream. For a preceding Sync/Raw element (whose sample
is already raw), the carried value is the element's final raw bit — raw
streams alternate clock/data positions, and a sync/raw sample of even length
ends on a data-position bit. Initialize `D[-1] = 0` at the very start of
track assembly *(boundary clocking is oracle-verify; a one-bit clock
discrepancy at element seams is invisible to sector decoding but would show
in a byte-exact oracle diff)*.

Sync marks never need synthesizing: IPF stores sync samples pre-encoded
(`$4489` = A1-with-missing-clock appears literally in the file), so the
notorious missing-clock patterns are emitted verbatim.

### 4.2 Track assembly

For one track (one IMGE + its DATA record):

1. Allocate `trackBits = IMGE.dataBits + IMGE.gapBits` raw bits (validate
   against the descriptor sums, §2.7; also sanity-cap: reject
   `trackBits > ~4·10⁶`, far beyond any real long track, before allocating).
2. For each block `b` in descriptor order: append the block's data area —
   each data stream element rendered per §3.1/§4.1, asserting the rendered
   size equals `desc[b].dataBits` — then the block's inter-block gap
   (`desc[b].gapBits` raw bits, filled per §3.2).
3. The result is the track as written, starting at block 0's first sync bit.
4. **Rotate to index alignment:** the emitted stream's bit 0 corresponds to
   angular position `IMGE.startBitPos` (in raw bits from the index). Rotate
   right by `startBitPos` so that `bits[0]` = the cell at the index pulse:
   `out[(i + startBitPos) mod trackBits] = built[i]`. This preserves
   data-over-index protections in the flux path. `ReadTrack`'s wrap handling
   makes the sector view alignment-insensitive either way. *(Whether the old
   library's `trackbuf` used index alignment is unknown — do not assume;
   compare against the oracle modulo rotation if a straight byte-diff
   fails.)*
5. `nbits = trackBits`; pack MSB-first into `ceil(trackBits/8)` bytes.

Unformatted tracks: an IMGE whose DATA record has `length == 0`, or
`blockCount == 0`, or `density == 1` (Noise) yields an **empty** decode
(`nbits = 0`) — the same signal the old path used (`tracklen == 0` ⇒ zeroed
`t_track`, absent SCP slot). Generating actual noise flux for
Noise-density tracks is deferred (§8.2).

### 4.3 Fuzzy (weak) regions

A Fuzzy element (`dataType 5`) contributes `2 × size` raw bits of
MFM-encoded **freshly generated random data** — random data bits pushed
through the §4.1 encoder so the clocking stays legal. Requirements:

- A per-image RNG generates the bits; each new decode pass of the same track
  advances the RNG (never reseeds), so consecutive passes yield different
  fuzzy bytes. This reproduces the *effect* the mirror relies on (contract
  §1.4): "N independent reads per track, flakey tracks vary, stable tracks
  don't". Exact RNG choice is not load-bearing — any decent PRNG
  (xoshiro/PCG/`std::minstd_rand`) seeded per image is fine.
- Deterministic option for tests: allow seeding the RNG explicitly so a
  fixture decode is reproducible.
- `IMGE.trackFlags` bit 0 (Fuzzy) is surfaced as the track's `flakey` flag;
  a track containing any Fuzzy element should have it set (validate, warn on
  mismatch, trust the element list).

*(Design note: a real fuzzy area is physically a no-flux or ambiguous-timing
zone — the worked example's underlying stream file shows an NFA — but
random-legal-MFM is what the consumer contract asks for, produces the
protection-visible effect (bytes differ between revolutions), and is exactly
what the doc prescribes: "it is the responsibility of the consumer to
generate random data with the specified size". Emitting genuinely empty flux
in fuzzy zones is a possible Phase C refinement.)*

### 4.4 Multi-revolution output

`ipf_mirror_to_scp` asks for `kMirrorRevs = 3` revolutions per cylinder
(side 0). For the clean-room decoder:

- Non-flakey track: decode once, reuse the same `t_mfm_rev` for all three
  revolutions (the contract explicitly blesses this optimization).
- Flakey track: three decode passes; only Fuzzy regions differ between
  passes; `nbits` is identical for every pass (unlike real flux captures,
  reconstructed revolutions are exactly equal length — allowed by the
  `t_mfm_rev` contract, which only requires the *revolution count* to match
  across cylinders).

### 4.5 Density and cell timing

v1 emits every track at uniform 2 µs cells (`signalType`/`cellType` = 1),
which `scp_from_mfm_tracks` converts at 80 ticks/cell. `density == 2`
(Auto) means the *average* cell already fits the track by construction —
long tracks simply have more cells (`trackBits` > the nominal ~100 k), and
the SCP revolution duration grows accordingly; the FDC's PLL and our
`flux-media.md` pipeline handle that naturally. Densities 3–9 encode
*intra-track* cell-width modulation that v1 does not model (§8.2): decode
at uniform width and log
`LOG_WARNING("IPF: density <n> (<name>) decoded with uniform cells — protection timing not modelled")`.

---

## 5. Mapping to konCePCja types

### 5.1 The four public functions (unchanged signatures)

Per the contract, the replacement lives entirely inside `src/ipf.cpp`;
callers (`slotshandler.cpp`, `subcycle_bridge.cpp`) do not change:

```cpp
int ipf_load(FILE*, t_drive*);                        // buffer/temp wrapper
int ipf_load(const std::string&, t_drive*);           // the loader
std::vector<uint8_t> scp_from_mfm_tracks(const std::vector<t_mfm_track>&);
                                                      // KEEP — already clean-room
std::vector<uint8_t> ipf_mirror_to_scp(t_drive*);     // flux path
```

The `FILE*` overload may drop the temp-file detour entirely (slurp the
stream and decode in memory) — its external contract is only "0 on success,
`ERR_DSK_INVALID` on failure, `drive` filled identically".

Internal shape (research §5.1, kept as the seam implementation units build
against):

```cpp
struct CleanImageInfo { int min_cyl, max_cyl, min_head, max_head;
                        int encoder_type; };
bool clean_open(/* bytes */, CleanImageInfo&);        // parse + validate all records
struct CleanTrackMFM { std::vector<uint8_t> bits; uint32_t nbits; bool flakey; };
bool clean_ipf_lock_track(/* image handle */, int cyl, int head,
                          CleanTrackMFM& out);        // one decode pass (advances RNG)
```

`drive->ipf_id` becomes an index/handle into the decoder's own open-image
table (opaque to everyone else); the decoder's eject hook function replaces
`ipf_eject_hook` **as the same identity check** — `ipf_mirror_to_scp` keeps
gating on `drive->eject_hook == ipf_eject_hook`. The image (parsed records +
RNG state) stays open after `ipf_load` returns, because `ipf_mirror_to_scp`
re-reads tracks through the same handle; the hook frees it.

### 5.2 Flux path (`t_mfm_track` / `t_mfm_rev`, engine=1)

`ipf_mirror_to_scp` behavior is unchanged from the contract (§1.4 there):
side 0 only, cylinders capped at `kScpMaxCyls = 84`, 3 revolutions per
cylinder via three `clean_ipf_lock_track` passes (§4.4), a track that
decodes empty mid-capture pads by repeating the last revolution, empty
vector on any failure/geometry mismatch. `scp_from_mfm_tracks` is **not
touched** — it is already clean-room and unit-tested
(`test/ipf_mirror_test.cpp`, 5 synthetic tests keep passing unmodified).

### 5.3 Sector view (`t_drive`/`t_track`/`t_sector`, disc tools & legacy FDC)

`ipf_load` decodes every (cyl, head) in
`minTrack..maxTrack × minSide..maxSide` and feeds each track's
`bits/nbits` to the **existing** `ReadTrack` MFM scanner in `src/ipf.cpp` —
that scanner is already ours and already implements the full IBM System-34
extraction contract (3×`$4489` alignment; `0xFE` ID fields; CRC-CCITT
seeded `0xcdb4` continuing over `A1 A1 A1`; `0xFB/FA/F8/F9` DAMs with
control-mark flags; `128 << N` sizes capped at `0x8000`; data accepted only
32–63 byte-times after its ID; wrap-around scanning; single-sector 4096-byte
overread; header-CRC failure drops the sector, data-CRC failure flags it via
`flags[0]|=0x20`, `flags[1]|=0x20`). Fill rules (contract §1.2/§2.1):

- per track: decoded `t_track` (`sectors`, `size`, `data = new byte[size]` —
  must be `new[]`, `dsk_eject` runs `delete[]`), or `memset`-zeroed when the
  decode is empty;
- single data version per sector (`setSizes(size, size)`) — the host view
  carries no weak-bit rotation; only the flux path does;
- `drive->tracks = maxTrack + 1`, `drive->altered = false`,
  `drive->eject_hook = <our hook>`, `drive->ipf_id = <handle>`;
- `drive->sides`: see §5.4.

### 5.4 The two flagged defects — prescribed behavior

**(a) `drive->sides = maxhead` (contract §1.2 called this an off-by-one).**
Investigation finding: the codebase convention for `t_drive::sides` is
**zero-based** — `disk_sector_editor.cpp:13` documents "0=single-sided,
1=double-sided", `slotshandler.cpp:380` decrements the DSK header's side
count (`drive->sides--`), and `dsk_save` re-adds 1. Under that convention
`sides = maxhead` is the *correct value* whenever `minSide == 0` (the
universal case), so the old code was right by accident and **must not** be
"fixed" to `maxhead + 1` — that would double-side every image. Prescribed
new behavior, made deliberate rather than accidental:

```
validate minSide == 0 (reject otherwise — no real image violates this)
drive->sides = maxSide - minSide      // zero-based, == maxSide in practice
```

Old vs new: byte-identical results for every well-formed image; the change
is the explicit validation + a comment stating the zero-based convention.

**(b) Missing geometry bounds check (real bug — fix).** The old loader
indexes `drive->track[cyl][head]` (`t_track track[DSK_TRACKMAX=102]
[DSK_SIDEMAX=2]`, `src/hw_views.h`) straight from the image's geometry
without validating it — a hostile IPF advertising `maxTrack ≥ 102` or
`maxSide ≥ 2` is an out-of-bounds write. Prescribed new behavior: validate
during `clean_open`, before touching `drive`:

```
reject unless: minTrack <= maxTrack
               maxTrack + 1 <= DSK_TRACKMAX   (102)
               minSide == 0 and maxSide + 1 <= DSK_SIDEMAX  (2)
```

Rejection = `ERR_DSK_INVALID` + `LOG_ERROR` naming the offending field.
Old: silent memory corruption. New: clean rejection. (Clamping was
considered and rejected: a >102-cylinder "floppy" is not a CPC medium.)

Also carried over deliberately (not bugs, but contract-mandated):
whole-load abort on any single track's decode failure (never return 0 with
a partially filled `drive`), and the temp-file TODO disappears with the
in-memory decode.

### 5.5 Integration & build seam

The decoder lands behind the `HAS_CAPSIMG` gating already specified in
contract §6: `KONCPC_HAS_CAPSIMG ?= 0` filters `src/capsimg/%` out of the
makefile glob and the clean-room implementation compiles unconditionally.
During the transition both implementations can coexist behind the flag for
oracle diffing (§8.3); after Gate-style sign-off the capsimg sources leave
the tree (they remain reachable in git history for the local oracle build).

---

## 6. CRC & endianness

- **Byte order:** every multi-byte integer in the container is
  **big-endian** (doc §3.1 states this explicitly, with a diagram). On
  little-endian hosts, byte-swap on read; use explicit shift-assembly
  (`(b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]`), never type-punning.
- **Record CRC (header field):** standard **CRC-32/ISO-HDLC** (IEEE 802.3,
  the zlib/PNG CRC): polynomial `0x04C11DB7`, reflected in/out
  (`0xEDB88320` table form), init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`.
  Computed over the record's full `length` bytes with the CRC field bytes
  (offset 8..11) treated as zero; the result is then stored there.
  **Verified byte-exact** against the worked example independently of any
  library: `CRC32("CAPS" ‖ 00 00 00 0C ‖ 00 00 00 00) = 0x1CD573BA`, the
  doc's recorded CAPS-record CRC. Use this 12-byte string as the unit-test
  vector for the CRC routine itself.
- **Extra Data Block CRC (DATA block field):** same CRC-32 over exactly
  `dataBlock.length` bytes of the extra block, no zeroing (the field lives
  outside the hashed range).
- The MFM sector CRC (CRC-16/CCITT `0x1021`) is *not* part of the container
  — it appears inside Data elements as recorded bytes and is only
  re-computed by the sector-view scanner (`ReadTrack`), which already has
  it.

---

## 7. Worked example — regression vectors (Theme Park Mystery, track 00.0)

Reproduced from the public doc (its §4.2; an Atari ST title with "Copylock
short" protection — 10 standard 512-byte sectors + 2 protection
pseudo-sectors). These values are license-free test vectors: they came from
documentation, not from any decoder run of ours. They pin the parser and
the reconstruction accounting without needing a real (non-redistributable)
IPF fixture. File spans below are `(first_byte - last_byte)` offsets in the
actual IPF file.

### 7.1 Record layer

| record | span | size | header CRC | key facts |
|--------|------|-----:|-----------:|-----------|
| CAPS | 0–11 | 12 | `0x1CD573BA` | magic; CRC vector verified in §6 |
| INFO | 12–107 | 96 | `0x2D97A5A5` | encoder **SPS** rev 1, fileKey 1 rev 1, origin `0xC0D3D4A8`, tracks 0–83, sides 0–1, date 2015/02/11, time 13:33:38.935, platform Atari ST, creator `0x00001001` |
| IMGE (T00.0) | 108–187 | 80 | `0xE35D87F2` | see §7.2 |
| … 167 more IMGE … | | | | 84 cyls × 2 sides |
| DATA (key 1) | 13548–13575 | 28 | `0xC07C042C` | see §7.3; extra block 13576–20227 |

**Parser assertions:** record traversal from offset 0 visits CAPS → INFO →
IMGE×168 → DATA×168 using `header.length`, with the DATA exception (next =
extra-block start + extra length; first DATA record's successor starts at
20228). IMGE(track 0, side 0) has `dataKey = 1` and pairs with the DATA
record above.

### 7.2 IMGE block values (track 00, side 0)

```
track=0 side=0  density=2 (Auto)  signalType=1 (2 µs)
trackBytes=12557   startBytePos=60   startBitPos=482
dataBits=93056     gapBits=7400      trackBits=100456
blockCount=12      encoderProcess=0  trackFlags=0   dataKey=1
```

Assertions: `trackBits == dataBits + gapBits`;
`trackBytes == ceil(trackBits/8)` (100456/8 = 12557); `startBitPos/8 ≈
startBytePos` (482/8 = 60.25 → 60).

### 7.3 DATA block values (dataKey 1)

```
length=6652   bitSize=53216 (=6652*8)   crc(extra)=0x53E54C0E   dataKey=1
```

Extra Data Block = 12 block descriptors (12 × 32 = 384 bytes, extra offsets
0–383) + Data Area (offsets 384–6651). Note the analyzer wrote the **gap
lists first** (block 0's `gapOffset = 384`, immediately after the
descriptors) and the data lists after (block 0's `dataOffset = 515`).

### 7.4 Block descriptor 0 (standard 512-byte Atari sector)

```
dataBits=8992  gapBits=512  gapOffset=384  cellType=1(2µs)
encoderType=1(MFM)  blockFlags=3 (FwGap|BwGap, DataInBit=0)  gapDefault=0
dataOffset=515
```

Gap stream (at extra offset 384; file bytes 13960–13975 area — element file
positions from the doc dump: forward pair at 13962/13965, backward pair at
13968/13971):

```
Forward:  GapLength   192 bits          — head width 1
          SampleLength  8 bits  = 0x4E
Backward: GapLength    64 bits
          SampleLength  8 bits  = 0x00
```

**Gap accounting assertion:** `gapBits 512 == 2·192 + 2·64` (post-data 24
bytes of 4E forward; pre-address 8 bytes of 00 backward; ×2 = MFM
clock+data). This is the vector that locks the §3.2 length-in-bits reading.

Data stream (at extra offset 515; sample file positions 14093, 14101,
14110, 14146, 14155):

| # | type | head width | size (bytes) | sample (leading bytes) | raw bits |
|--:|------|-----------:|-------------:|------------------------|--------:|
| 1 | Sync | 1 | 6 | `44 89 44 89 44 89` | 48 |
| 2 | Data | 1 | 7 | `FE 00 00 01 02 CA 6F` (ID: sector 01, C=0 H=0 N=2, CRC CA6F) | 112 |
| 3 | Gap | 1 | 34 | `4E`×22 then `00`×12 (intra ID/data gap) | 544 |
| 4 | Sync | 1 | 6 | `44 89 44 89 44 89` | 48 |
| 5 | Data | 2 | 515 | `FB 60 1E 00 …` … `… B7 2C` (DAM + 512 data + CRC) | 8240 |

**The load-bearing accounting assertion (§3.1 rule):**
`48 + 2·56 + 2·272 + 48 + 2·4120 = 8992 == dataBits`. Note element 5 needs
`dataSizeWidth = 2` (515 > 255) — a direct vector for the head-byte
decoding. All sizes are in bytes because `DataInBit = 0`.

### 7.5 Block descriptor 10 (Copylock "sgn" pseudo-sector 11) — gapless block

```
dataBits=800  gapBits=0  gapOffset=0  cellType=1  MFM
blockFlags=0 (no gap lists)  gapDefault=0  dataOffset=6315
```

Data stream (positions 19893/19901/19910/19940/19948):
Sync 6 bytes `44 89`×3; Data 7 bytes `FE 00 00 0B 02 25 A4` (sector 11);
Gap 28 bytes (`4E`×22 + `00`×6); Sync 6 bytes; Data 9 bytes
`FB 00 00 00 00 00 00 00 00`. Assertion:
`48 + 2·56 + 2·224 + 48 + 2·72 = 800`. A block with `gapBits == 0`
contributes no inter-block gap — the next block's data follows immediately.

### 7.6 Block descriptor 11 (Copylock "key" pseudo-sector 12) — backward-only, looping gap

```
dataBits=2336  gapBits=2280  gapOffset=504  cellType=1  MFM
blockFlags=2 (BwGap only)  gapDefault=0  dataOffset=6382
```

Gap stream (backward only, elements at 14082/14085/14087/14090):
`GapLength 192 · Sample 8 = 0x4E`, then `GapLength 64 · Sample 8 = 0x00` —
specifies only `2·(192+64) = 512` raw bits of the 2280-bit gap. **Assertion
(loop-to-fill rule):** the decoder loops the samples toward the beginning
of the track; as this is the last block, the filled gap lands *in front of
block 0* (track wrap), with the doc's own words: "Incomplete specification
512 bits out of 2280".

Data stream (positions 19960…20015): Sync 6 B (`44 89`×3); Data 7 B
`FE 00 00 0C 02 BC 33` (sector 12); Gap 8 B (`4E`×8); Data 8 B
`D9 23 76 C5 E6 D3 31 B2` (the Copylock key); Gap 8 B (`4E`×8); Data 6 B
`FF FF FF FF FF FE`; **Sync 212 B** starting
`89 12 89 12 89 12 AA 8A 55 55 …` — i.e. `$4489`-shifted-by-a-half-bit sync
marks (`$8912`), a half-clock-shifted DAM (`$AA8A`), 38× `$5555`, then 64×
`$0000` = a **No Flux Transition Area expressed as raw sync bits**.
Assertion: `48 + 2·56 + 2·64 + 2·64 + 2·64 + 2·48 + 1696 = 2336`. This
vector proves why Sync samples must be emitted verbatim: `$0000` sync words
(no transitions at all) and half-bit-shifted marks are *unrepresentable* as
MFM-encoded data.

### 7.7 Suggested always-on CI fixture

Beyond the vectors above (which test the parser/accounting layers as pure
unit tests on hand-typed byte arrays), synthesize a **minimal hand-built
IPF** in test code — CAPS + INFO(SPS, 1 track, 1 side, platform CPC) + one
IMGE + one DATA with one block: Sync(`44 89`×3) + Data(ID for C=0 H=0 R=1
N=2 + valid CRC) + Gap + Sync + Data(DAM + 512 bytes + CRC), gapDefault
`0x4E` — with CRCs computed by our own routine. Assert: `ipf_load`
succeeds, `ReadTrack` finds exactly sector R=1 with matching payload, and
`ipf_mirror_to_scp` yields an SCP that `flux_scp_to_dsk` decodes back to the
same sector. This closes the loop end-to-end in CI without any
preservation dump, mirroring how `ipf_mirror_test.cpp` hand-builds
`t_mfm_track` data today. A second fixture with a Fuzzy element asserts
revolution variance (§8.3.5).

---

## 8. Coverage & risks

### 8.1 What v1 covers

SPS-encoder (encoderType 2), Auto-density (2) images — essentially all
mainstream CPC preservation — including: multi-block tracks, gapless
blocks, forward/backward/default/looping gap fill, raw sync (NFA,
half-bit-shifted marks), fuzzy/weak regions with per-revolution variance,
data-over-index (via `startBitPos` rotation), long/short tracks (via
`trackBits`), both sides in the sector view, side 0 in the flux mirror
(pre-existing mirror limitation, unchanged).

### 8.2 Deferred / residual risk

| item | risk | v1 behavior | plan |
|------|------|-------------|------|
| Densities 3–9 (Copylock ST/Amiga, Speedlock, Adam Brierley) | protection timing checks may fail on exotic titles (mostly Amiga/ST; rare on CPC) | decode at uniform 2 µs + WARN | Phase C (§9 U-C2), oracle-driven per real protected titles |
| CAPS encoder (encoderType 1, old files) | old IPFs won't load | detect + reject with clear error | Phase C (U-C1): `dataBytes`/`gapBytes` reconstruction |
| Noise density (1) | a protection that *reads* an unformatted track expecting garbage sees "no data" instead | empty track | optional: synthesize noise flux in the mirror |
| Doc errors (self-declared) | subtle byte-level mistakes anywhere | — | every *(oracle-verify)* marker in this spec is a mandatory oracle diff before sign-off |
| Element-seam clock bits (§4.1) | 1-bit clock diffs vs original mastering | carried-bit rule | byte-exact oracle diff catches it |
| GapLength prose-vs-example ambiguity (§3.2) | wrong gap sizes | length-in-bits reading (example-backed) | locked by §7.4 vector + oracle |
| Dual gap-list layout (§3.2) | wrong backward-list start | fwd-then-bwd at `gapOffset` | oracle diff on any FwGap+BwGap image |
| Bit-unit samples (`DataInBit=1`) | rare path untested by the worked example | implemented per §3.1 | oracle diff on an image that uses it; else leave flagged |

### 8.3 Oracle strategy

capsimg remains a **local, uncommitted, test-only oracle** — its
non-commercial SPS licence forbids shipping; it must never be linked into a
release binary nor committed as a fixture dependency. The gating pattern
already exists and is reused unchanged:

1. `IpfMirror.MirrorsARealCapsImageWhenProvided`
   (`test/ipf_mirror_test.cpp`) skips unless `KONCEPCJA_REAL_IPF=<path>` or
   `test/hw/fixtures/real.ipf` exists (never committed). After the swap it
   exercises the clean-room decoder end-to-end with the same assertions
   (SCP probes valid, 3 revolutions, sectors found by `flux_scp_to_dsk`).
2. **Byte-exact MFM diff** (transition period, both decoders behind the
   build flag): for a real `.ipf`, assert clean `bits/nbits` ==
   capsimg `trackbuf/tracklen` per (cyl, head) — first modulo rotation
   (§4.2 step 4), then exact once alignment is established. Strongest
   signal; catches every *(oracle-verify)* item.
3. **Sector round-trip:** clean decode → `ReadTrack` → CHRN + flags + data
   byte-equal to capsimg-decoded sectors (and to the title's DSK release
   where one exists).
4. **Flux round-trip:** `ipf_mirror_to_scp` output SCP byte-diff, or where
   encodings legitimately differ, `flux_scp_to_dsk`-decoded sectors
   byte-diff (`FdcFlux.DecodesARealScpCaptureWhenProvided` pattern,
   `KONCEPCJA_REAL_SCP`).
5. **Weak-bit variance:** our 3 revolutions differ exactly where the track
   is flakey and are identical where it is stable; capsimg's revolutions
   show variance in the same regions (statistical, not byte-equal — fuzzy
   bytes are random on both sides).

The five existing synthetic `scp_from_mfm_tracks` tests are
decoder-independent and must keep passing unmodified throughout.

---

## 9. Implementation plan (ordered, dependency-aware)

Refines research-doc units U4–U8/U10 into IPF-scoped work packages. Shared
seams are frozen first: `t_mfm_rev`/`t_mfm_track` (exists, untouched), the
`CleanImageInfo`/`CleanTrackMFM` structs and the parsed-record structs
(defined in U-1's header on day one), and this spec's §3 element model.

| unit | deliverable | depends on | parallel? |
|------|-------------|------------|-----------|
| **U-1 Container parser** | bytes → validated record set: CAPS/INFO/IMGE/DATA structs, CRC32 routine (+ §6 vector), traversal incl. DATA exception, CTEI/CTEX/unknown skip, geometry validation (§5.4b), dataKey matching. Pure, no I/O side effects. | — | yes — with U-2 (against the frozen structs) and U-5 |
| **U-2 Stream-element decoder** | block descriptors + Data Area → per-block typed element lists (data elems; fwd/bwd gap lists), bounds-checked; §7 parser vectors as unit tests | U-1 structs (header only) | yes — with U-1 body, U-3 |
| **U-3 MFM reconstruction** | element lists → one `CleanTrackMFM` per pass: §4.1 encoder w/ carried bit, §4.2 assembly + rotation, §3.2 gap fill (all four modes + looping), accounting asserts; §7.4–7.6 accounting vectors as unit tests | U-2 element types (header only) | yes — with U-2 body |
| **U-4 Fuzzy multi-rev** | per-image RNG, Fuzzy rendering, deterministic seed hook, flakey flag | U-3 | after U-3; small |
| **U-5 Oracle harness** | uncommitted capsimg side-build + byte-diff runner + the §8.3 comparisons behind `KONCEPCJA_REAL_IPF` | — (uses old code as-is) | yes — fully parallel from day one |
| **U-6 Glue swap** | `ipf.cpp` integration: `clean_open`/`clean_ipf_lock_track` replace the CAPS calls; image table + eject hook identity; `t_drive` fill incl. §5.4 fixes; in-memory `FILE*` path; `HAS_CAPSIMG` build seam (contract §6) | U-1..U-4 | no — integration, single owner |
| **U-7 CI fixtures** | §7.7 hand-built minimal IPF + fuzzy fixture, end-to-end CI test | U-6 | after U-6 |
| **U-8 Oracle sign-off** | run §8.3 items 2–5 on real CPC IPFs (incl. one protected title), burn down every *(oracle-verify)* marker, then flip `KONCPC_HAS_CAPSIMG` default to 0 | U-6, U-5 | gate |
| **U-C1 CAPS encoder** (Phase C) | encoderType 1 `dataBytes`/`gapBytes` path | U-6 | deferred |
| **U-C2 Density modelling** (Phase C) | per-density cell-width modulation, oracle-driven | U-8 | deferred |

Fan-out shape: U-1/U-2/U-3 bodies proceed concurrently against
header-frozen interfaces; U-5 runs alongside; U-6 is the single serial
integration step; U-7/U-8 close it out. Each of U-1..U-4 ships with its §7
vectors as always-on unit tests, so correctness never depends solely on the
gated oracle.

---

## 10. Sources & licences appendix

**Safe to learn the format from (documentation):**

- *Interchangeable Preservation Format (IPF) Documentation*, Jean
  Louis-Guérin (DrCoolZic), **Version 1.6, April 2018**
  (`kryoflux.com/download/ipf_documentation_v1.6.pdf`; v1.5 also at
  info-coach.fr). The primary and near-sole source of §§2–4 and 7. A
  community reverse-engineered description whose author explicitly warns it
  "must contain errors" — hence §8.3. Its v1.5 changelog is itself
  load-bearing: it *corrected* §3.7.2.2 to state Fuzzy elements carry **no**
  `dataSample` (our §3.1).
- Archive-Team file-format wiki (IPF page), softpres.org glossary —
  background cross-checks only; nothing in this spec depends on them.
- Our own files: `src/ipf.h`, `src/ipf.cpp` (glue + `ReadTrack`, ours),
  `src/hw_views.h`, `docs/hardware/ipf-decoder-contract.md`,
  `docs/hardware/ipf-decoder-cleanroom-research.md`,
  `docs/hardware/flux-media.md`.
- Independent computation: the §6 CRC verification (12 known bytes → the
  doc's recorded value) was performed against zlib's public CRC-32, pinning
  the algorithm without consulting any decoder source.

**Permissive code, cross-check only:**

- **fluxfox** (`github.com/dbalsom/fluxfox`, MIT) — may be *consulted* to
  break a tie on an ambiguous point in this spec, with the consultation
  noted in the implementation PR. Prefer the oracle. If any code were ever
  reused (discouraged), the MIT notice travels with it.

**Off-limits for implementation (do not read):**

- `src/capsimg/**` — the SPS Decoder Library (© István Fábián / KryoFlux),
  **non-commercial licence**. Runtime/test use as the local uncommitted
  oracle (§8.3) is the only permitted role; its source is never a reference
  for this spec's implementers. (The DrCoolZic doc claims a later
  MAME-licensed release of the decoder exists; the copy in our tree carries
  the non-commercial licence, the discrepancy is a counsel question, and
  this plan does not depend on its resolution — research doc, Appendix A.)
- Any GPL IPF reader (emulators, flux tools) — copyleft we are shedding.

**Worked-example data (§7):** transcribed from the DrCoolZic doc's own
dumps of *Theme Park Mystery* track 00.0 — published documentation content
used as test vectors, not extracted from a preservation dump by us; no disk
image is committed.
