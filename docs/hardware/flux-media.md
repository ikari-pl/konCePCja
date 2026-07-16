# Flux media — SCP container, software PLL, MFM decode (Stage 1)

Language-neutral spec for the **flux-to-DSK converter** (`src/hw/flux`): a set
of pure, heap-free functions that turn a Greaseweazle/SuperCard Pro **SCP flux
dump** into a **standard (or extended) DSK image** that the existing FDC Device
consumes unchanged via `fdc_attach_disk()` (see
[`fdc-device.md`](fdc-device.md)). Companion to
[`floppy-disc-system.md`](floppy-disc-system.md); see `docs/hw-spec.md` for the
house rules (pure functions, caller-owned buffers, no heap).

This is **Stage 1** of flux support: an *offline* converter. **Stage 2** (the
follow-on this feeds) replaces the FDC's parsed-DSK medium with a **rotating
flux medium** — the µPD765A Device then reads bitcells off the spinning surface
in simulated time, which is what makes copy-protection timing, weak/fuzzy bits
and index-torn sectors behave like hardware. Stage 1 deliberately collapses all
of that to sector data + status bytes.

Format details verified against the published SuperCard Pro image specification
(Jim Drew, cbmstuff.com, *SuperCard Pro image file* revision 2.x) and the
Greaseweazle reference implementation (`greaseweazle/image/scp.py`, keirf,
GitHub, retrieved 2026-07).

---

## 1. The SCP container

All header/track-header fields are **little-endian**; the flux words themselves
are **big-endian** (the one wrinkle everyone trips on).

### 1.1 File header (16 bytes at offset 0)

| off | size | field |
|---|---|---|
| 0x00 | 3 | magic `"SCP"` |
| 0x03 | 1 | version (creator nibbles; not interpreted) |
| 0x04 | 1 | disk type (not interpreted) |
| 0x05 | 1 | **number of revolutions** per track |
| 0x06 | 1 | start track |
| 0x07 | 1 | end track (≤ 167) |
| 0x08 | 1 | flags (bit 0 = index-cued; not interpreted here) |
| 0x09 | 1 | bitcell width: **0 or 16** = 16-bit cells (anything else rejected) |
| 0x0A | 1 | heads: 0 = both sides, 1 = side 0 only, 2 = side 1 only |
| 0x0B | 1 | resolution: tick = **25 ns × (resolution + 1)** |
| 0x0C | 4 | checksum of bytes 0x10..EOF (**not verified** — many tools write 0) |

### 1.2 Track offset table (0x2A0 bytes at offset 0x10)

168 × 32-bit absolute file offsets, one per SCP track slot; **0 = track absent**.
The standard slot convention is `slot = cylinder × 2 + side` (even slots =
side 0) for *both* single- and double-sided dumps — this is what Greaseweazle
writes. A **legacy consecutive layout** exists for old single-sided images
(heads ≠ 0, tracks packed at consecutive slots): detected by `heads == 1` with
any populated odd slot, in which case `slot = cylinder`.

### 1.3 Track Data Header

At each track offset:

| off | size | field |
|---|---|---|
| 0x00 | 3 | magic `"TRK"` |
| 0x03 | 1 | track (slot) number |
| 0x04 | 12 × revs | per-revolution table |

Each 12-byte revolution entry: **duration** (32-bit, ticks — one index-to-index
time), **length** (32-bit, number of 16-bit flux words), **offset** (32-bit,
relative to the start of the TDH).

### 1.4 Flux data

A revolution is `length` **16-bit big-endian** words; each word is the tick
count between two successive flux transitions. A word of **0x0000 means
overflow**: add 65536 ticks and continue accumulating into the next word (an
interval longer than 0xFFFF ticks is written as *n* zero words followed by the
remainder). A trailing unterminated overflow run is discarded.

---

## 2. The software PLL

The CPC's discs are ISO/IBM **MFM double density, 250 kbit/s data** = 500 kbit/s
raw MFM bitcells = a nominal **2 µs half-cell** (80 ticks at 25 ns). Legal MFM
flux intervals are **2, 3 or 4 half-cells** (4/6/8 µs); a real drive adds
spindle-speed error (±2 % is normal) and per-transition jitter. Decoding is
two-stage — a per-revolution cell estimate, then a tightly-clamped adaptive
clock:

**Stage a — cell estimate.** Spindle speed scales every interval equally, so
the interval population measures it directly. Two reclassification passes:

```
cell ← nominal                       # 2 µs in ticks
repeat 2×:
    over all intervals t with round(t / cell) = n ∈ [2, 4]:
        cell ← Σt / Σn               # only if ≥ 128 such intervals (signal)
clamp cell to nominal ± 15 %
```

Zero-mean jitter averages out over a revolution (~40 000 intervals), so the
estimate lands within a fraction of a percent of the true rate even at ±10 %
per-interval jitter; a constant speed error is captured exactly.

**Stage b — the PLL proper.**

```
clock ← cell
for each flux interval t (ticks, overflow-accumulated):
    n ← round(t / clock);  n ← max(n, 1)
    emit (n − 1) zero bitcells, then a 1 bitcell
    clock ← clock + ((t − n·clock) / n) × 0.02     # 2 % proportional adjustment
    clamp clock to cell ± 1.5 %
```

The tight clamp is the robustness argument: classification only fails when an
interval lands ≥ 0.5 cell from its true grid point, and the worst legal case —
a 4-cell interval jittered 10 % short read against a clock 1.5 % fast — still
yields 4 × 0.9 / 1.015 = 3.55 > 3.5. So worst-case ±10 % per-interval jitter
plus any speed error the estimator absorbed can never misclassify; a
single-stage PLL with a loose clamp, by contrast, random-walks its clock a few
percent off under sustained jitter and eventually misrounds (found by test).
Long unformatted / overflow gaps emit harmless runs of zeros that no sync ever
matches, and their huge per-cell error is damped by the ÷n before the clamp.

The decoder processes **revolution 0**, and when the dump has ≥ 2 revolutions
also **revolution 1**, concatenated into one bitcell stream (bounded — §5).
Decoding two revolutions makes every sector appear at least once *whole* even
when it straddles the index in revolution 0, and gives the second reading used
for weak-sector detection (§4).

---

## 3. IBM System 34 / ISO MFM track structure

What the bitcell stream contains, and what the scanner looks for:

```
gap 4a (4E…)  sync 12×00  3×C2* FC (index mark)  gap1 (4E…)
per sector:
  sync 12×00  3×A1* FE  C H R N  CRC16          ← ID address mark (IDAM)
  gap2 22×4E
  sync 12×00  3×A1* F B|F8  data[128·2^N] CRC16  ← DAM (FB) / deleted DAM (F8)
  gap3 (4E…)
gap 4b (4E…) to the index
```

- MFM encodes each data bit as (clock, data); clock = 1 only between two zero
  data bits. **A1 with a missing clock** (bit 5's clock suppressed) is the raw
  16-bit pattern **0x4489** — it can never occur in ordinary data, which is
  what makes it a byte-alignment sync. (`C2*` similarly = 0x5224; the index
  mark is ignored by this decoder — sector discovery needs only the A1 runs.)
- The scanner slides a 16-bit window over the bitcell stream; on **0x4489** it
  is byte-synced: it consumes any further consecutive 0x4489 words, then reads
  the mark byte. `0xFE` starts an ID field (read C/H/R/N + CRC); `0xFB`/`0xF8`
  within ~1280 bitcells of a CRC-valid IDAM starts that sector's data field
  (read `128·2^N` bytes + CRC; `0xF8` sets the DSK's ST2 Control-Mark bit
  0x40). A data mark with no pending header is ignored.
- **CRC-CCITT**: polynomial **0x1021**, init **0xFFFF**, MSB-first, covering
  **A1 A1 A1 + mark + payload** (i.e. the ID CRC covers `A1 A1 A1 FE C H R N`,
  the data CRC `A1 A1 A1 FB data…`), transmitted big-endian. The preset is
  always computed as if exactly three A1s were seen, per the standard, however
  many sync words the PLL actually delivered.
- A **header CRC failure** discards the header (the sector may still be
  recovered from revolution 1). A **data CRC failure** keeps the (suspect)
  payload but flags the sector (§4). A data field running past the end of the
  bitcell stream (index-torn, single-revolution dump) is discarded.

---

## 4. Weak-sector detection (multi-revolution rule)

When the SCP provides ≥ 2 revolutions, every sector is normally seen twice.
Duplicate sightings are matched by C/H/R/N; the **first** (revolution 0)
sighting supplies the DSK data. A sector is reported **weak/suspect** when:

- its (first-sighting) **data CRC fails** — reason bit `FLUX_WEAK_CRC`; also
  flagged on single-revolution dumps; the DSK marks it ST1 |= 0x20, ST2 |= 0x20
  (Data Error), matching how DSK images conventionally record bad-CRC sectors,
  **or**
- a later sighting's **payload differs byte-anywhere** from the first —
  reason bit `FLUX_WEAK_DIFFER`. The DSK carries the revolution-0 bytes and no
  status marks (both readings had valid structure): *true* weak-bit emulation —
  returning different data per read — requires the rotating flux medium and
  arrives with **Stage 2**.

The report is an optional caller struct (count + a small fixed list of
cylinder/side/sector-id entries); overflowing the list only saturates the list,
never the count.

---

## 5. Bounds and rejection rules (no heap, no silent truncation)

All working storage is fixed-size and function-local; anything that does not
fit is a **hard error**, never a silent truncation:

| bound | value | rationale |
|---|---|---|
| bitcell buffer | 262 144 cells (32 KB packed) | 2 revolutions × ~100 000 cells (200 ms at 500 kbit/s) + CPC speed tolerance |
| cylinders | ≤ 84 | SCP slot space (168/2); CPC target is 40/42 |
| sectors/track | ≤ 29 | what a DSK Track-Info block can hold (FDC limit) |
| sector size | N ≤ 5 (4 096 B) | larger N cannot fit a DD track |
| track payload | ≤ 8 192 B | a DD revolution carries ~6 250 raw bytes |

Error codes (negative returns of `flux_scp_to_dsk`, defined in `flux.h`):
not-SCP, unsupported geometry, file truncated (any offset/length outside the
buffer), revolution exceeds the bitcell buffer, output capacity exhausted, no
sector found on any track.

**Scope**: side 0 only is converted (CPC AMSDOS media are single-sided).
Two-sided SCPs are accepted — their odd slots are simply not read. A
side-1-only dump (`heads == 2`) is rejected. Absent slots inside the cylinder
range become empty (unformatted) DSK tracks; trailing absent slots shrink the
image.

---

## 6. DSK emission

One Track-Info block per cylinder, in the layout `fdc_attach_disk` parses
(§5 of [`fdc-device.md`](fdc-device.md)): 0x100-byte disc header, then per
track a 0x100-byte `Track-Info\r\n` header (track, side, size code, sector
count, gap3 0x4E, filler 0xE5, and 8-byte C/H/R/N/ST1/ST2/len entries) followed
by the sector payloads in the order they passed under the head. Track blocks
are padded to a 256-byte boundary (required by the extended format's size
table; harmless in the standard one).

- If **every** track block has the same size and **every** sector the same
  size code → **standard** DSK (`"MV - CPC"`, global track-size word, per-track
  size table zeroed, per-sector stored-length fields zeroed — both unused in
  the standard format).
- Otherwise → **extended** DSK (`"EXTENDED"`, per-track size table = block
  size ÷ 256, per-sector actual stored length).

The result is byte-stream-complete: writing it to a file yields a valid `.dsk`.

---

## 7. Stage 3 API — per-track, per-revolution decode (IMPLEMENTED substrate)

Stage 2 of the FDC made the medium rotate; Stage 3 makes it rotate **flux**. The
enabling primitives (in `src/hw/flux.h`, same pure/heap-free contract):

- `flux_scp_revolutions` / `flux_scp_cylinders` — cheap geometry probes.
- `flux_decode_track_rev(scp, len, cyl, rev, FluxTrack*, payload, cap)` — PLL +
  MFM decode of **one revolution of one cylinder** into a caller buffer:
  the sector map (C/H/R/N, DSK-convention ST1/ST2, payload offsets) plus each
  sector's **angular byte-cell positions** (`idam_cell`, `data_cell`, 0..6249)
  normalized from the sector's bitcell index over the revolution's measured
  length — so real gap geometry, nonstandard formats, and long tracks carry
  through instead of the synthesized System 34 layout the DSK path uses.
  An absent/unformatted cylinder decodes to `count = 0` (success: a head over
  nothing reads nothing); `rev` reduces modulo the capture count (the platter
  keeps turning; the captures repeat).

**The FDC integration this feeds** (the remaining Stage 3 work):
`fdc_attach_flux(dev, scp, len)` keeps the SCP as the media backend; on every
head-settle (seek/recalibrate completion) the FDC decodes the track under the
head into a fixed in-state cache — one `FluxTrack` + payload **per captured
revolution** (2 revs × 8 KB bounds it). READ ID / READ DATA then schedule by
the cached angular positions exactly as today, but serve data from the cache of
`(rotation count) mod (captured revolutions)` — the revolution physically
passing the head. **Weak/fuzzy bits therefore emerge with no special casing**:
re-reads of a protection sector return different bytes because different
captured revolutions pass by, which is literally what the original protection
exploited. CRC-failed sectors keep their ST1/ST2 error bits per revolution, so
"sometimes reads clean" protections behave statistically, like hardware.

Verified by tests: per-revolution byte-exactness against the encoder, angular
monotonicity + the ~38-cell ID→data lead + uniform pitch measured from the real
bitstream, per-revolution payload divergence on a weak sector, and modulo
wrap-around of the revolution index.
