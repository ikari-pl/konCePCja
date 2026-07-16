# HFE (HxC Floppy Emulator) v1 format — clean-room decoder spec

Status: **implemented (transcoder only)**, 2026-07-14. Companion to
[`flux-formats-feasibility.md`](flux-formats-feasibility.md) §2.3 (the survey
verdict) and [`flux-ingestion-contract.md`](flux-ingestion-contract.md) (the
shared SCP seam). Implemented by `src/hfe.h` / `src/hfe.cpp`; tested by
`test/hfe_test.cpp`. **Not yet wired into the app** — the loader/dispatch
integration is a separate unified pass (see the ingestion contract §4
checklist).

## 1. Scope & provenance

- **Source doc:** *SDCard HxC Floppy Emulator HFE File format* **Rev 3.1**
  (2019-05-22, J-F Del Nero / HxC2001),
  `hxc2001.com/floppy_drive_emulator/HFE-file-format.html` and the matching
  PDF. **Public vendor spec — DOC-safe to learn the format from.**
- **Clean-room:** implemented from the vendor doc text only. `libhxcfe` (the
  reference decoder) is **GPL** and was never opened. `src/capsimg/**` is
  non-commercial and off-limits. fluxfox (MIT) is a permitted cross-check
  only; not consulted for this implementation.
- **Scope:** **HFE v1 only** (`formatrevision == 0`, signature `"HXCPICFE"`).
  **HFE v3** (`"HXCHFEV3"`, in-band `F0`–`F4` opcodes for weak-bit /
  variable-rate dumps) is **deferred** — detected and cleanly rejected
  (`HFE_E_UNSUPPORTED`), never mis-parsed. HFE v2 shares v1's `"HXCPICFE"`
  signature but sets `formatrevision == 1`; this module accepts only
  `formatrevision == 0`, so a v2 image is also rejected as unsupported (v2's
  differences are irrelevant to a CPC DD bitstream, but out of scope here).

## 2. The one architectural fact

**HFE stores fixed-rate *bitcells*, NOT flux** — one abstraction level above
SCP/KryoFlux. A `1` bit is a flux reversal at that cell boundary; a `0` is
none. Converting bitcells → flux is deterministic and **PLL-free**: the
interval since the previous `1` is `(zeros_between + 1) × cell_time`. No clock
recovery is needed on the way in.

Because the decoded bitstream *is* the exact bitcell timeline, this maps onto
the ingestion contract's **second reuse rung** (`flux-ingestion-contract.md`
§1.1): build `t_mfm_rev`/`t_mfm_track` (`src/ipf.h`) and hand them to
`scp_from_mfm_tracks()`, which already assembles the SCP container (header +
TLUT + per-revolution big-endian flux words, `kTicksPerCell = 80`) with test
coverage. **We reuse that rather than emitting SCP flux words a second time**
— see §5.

## 3. v1 header (`picfileformatheader`, 512 bytes, little-endian)

Unused bytes are `0xFF` by convention. Offsets are byte offsets from file
start.

| Off | Size | Field | Used? | Meaning |
|-----|------|-------|-------|---------|
| 0x000 | 8 | `HEADERSIGNATURE` | yes | `"HXCPICFE"` (v1/v2). `"HXCHFEV3"` → rejected. |
| 0x008 | 1 | `formatrevision` | yes | 0 = v1 (only accepted value). |
| 0x009 | 1 | `number_of_track` | yes | Cylinder count (LUT entry count). |
| 0x00A | 1 | `number_of_side` | **no** | Informational only — side presence is derived from the LUT instead (per the spec's CPC guidance). Never read. |
| 0x00B | 1 | `track_encoding` | **no** | `0x00` ISOIBM_MFM, `0x02` ISOIBM_FM, … A `1` bitcell is a reversal regardless of the higher-layer group encoding, so this is not needed to build flux. |
| 0x00C | 2 | `bitRate` | yes | kbit/s. Drives `cell_time` (§6). |
| 0x00E | 2 | `floppyRPM` | **no** | Emulator-only metadata. |
| 0x010 | 1 | `floppyinterfacemode` | **no** | `0x06` = `CPC_DD_FLOPPYMODE`. Tells a *physical* emulator how to wiggle drive pins; a decoder treats CPC media as generic Shugart MFM. |
| 0x011 | 1 | `dnu` | no | Reserved. |
| 0x012 | 2 | `track_list_offset` | yes | Track LUT position, **in 512-byte blocks** (normally 1). |
| 0x014 | 1 | `write_allowed` | **no** | Write-protect flag. |
| 0x015 | 1 | `single_step` | **no** | `0xFF` single / `0x00` double step — head-stepping only, not bitstream decoding. |
| 0x016 | 1 | `track0s0_altencoding` | **no** | `0x00` → use alt encoding for track 0 side 0. Ignored (see `track_encoding`). |
| 0x017 | 1 | `track0s0_encoding` | **no** | The alt encoding value. |
| 0x018 | 1 | `track0s1_altencoding` | **no** | Track 0 side 1 alt-encoding flag. |
| 0x019 | 1 | `track0s1_encoding` | **no** | The alt encoding value. |

## 4. Track LUT (`pictrack[]`)

At `track_list_offset × 512`, one 4-byte entry per track (`number_of_track`
entries), both fields little-endian `uint16`:

| Off | Size | Field | Meaning |
|-----|------|-------|---------|
| +0 | 2 | `offset` | Track data start, **in 512-byte blocks** (× 512 = byte offset). |
| +2 | 2 | `track_len` | Track data length **in bytes, BOTH sides combined**. |

`track_len == 0` ⇒ an absent/unformatted track (empty SCP slot). The LUT must
lie wholly inside the file (`track_list_offset×512 + 4×number_of_track ≤ len`)
or the input is rejected `HFE_E_TRUNCATED`.

## 5. Track data layout & side de-interleave

Track data (`track_len` bytes from `offset×512`) is a run of **512-byte
blocks, each split 256 bytes side 0 then 256 bytes side 1**, repeating. A
final partial block may be short; side 0 is always positioned first, so a
track under 256 bytes is entirely side 0.

**Side-0 extraction:** walk the `track_len` byte range; take the first up-to-
256 bytes of each 512-byte block as side 0, skip the next up-to-256 as side 1,
repeat. (Implemented as: take `min(256, remaining)`, then skip
`min(256, remaining)`.)

**Bit order — LSb-first.** The spec: *"The bits transmission order to the FDC
is LSb first: Bit0→Bit1→…→Bit7→(next byte)."* This is the **opposite** of
`t_mfm_rev.bits`'s MSb-first packing (`src/ipf.h`, matching
`scp_from_mfm_tracks`'s reader), so every extracted bit is re-packed: source
bit `b` of side-0 byte `i` → logical bitcell `i×8 + b`, stored MSb-first in
`t_mfm_rev.bits`.

Side 0 only is emitted — the sub-cycle FDC's flux cache is side-0-only end to
end (`flux-ingestion-contract.md` §1), matching `ipf_mirror_to_scp`. One
revolution per cylinder — HFE v1 has a single fixed bitcell stream per track
(weak-bit/variable-rate variation is HFE v3, deferred), so a second captured
revolution would only duplicate the first.

## 6. Timing arithmetic (bitcell → SCP tick)

The HFE spec gives the transmission (cell) rate as `bitRate × 2`:

```
cell_time (s) = 1 / (bitRate_kbit_s × 1000 × 2)
```

SCP flux words count 25 ns ticks. Ticks per HFE bitcell:

```
ticks_per_cell = cell_time / 25e-9
              = 1e9 / (bitRate × 2000 × 25)
              = 20000 / bitRate           (integer form; see hfe_ticks_per_cell)
```

At the CPC/IBM double-density standard **bitRate = 250 kbit/s**:
`20000 / 250 = 80 ticks/cell = 2 µs` — exactly the `kTicksPerCell = 80`
constant `scp_from_mfm_tracks` is built around, and the physically-correct DD
half-cell for the flux decoder's nominal `80/(resolution+1)` at resolution 0.

**Reuse decision & the bitrate gate.** `scp_from_mfm_tracks` hard-codes
`kTicksPerCell = 80`, so reusing it is only valid when `ticks_per_cell == 80`,
i.e. `bitRate == 250`. `hfe_to_scp()` therefore **accepts only bitRate 250**
(every CPC HFE; `CPC_DD_FLOPPYMODE` is DD-only) and rejects any other rate
`HFE_E_UNSUPPORTED` rather than feeding mismatched cells through the fixed
constant. This deliberately avoids writing a *second* flux-word emitter with
its own tick math that could drift from the tested one in `ipf.cpp`. If a
non-250 HFE ever needs support, the clean extension is a general emitter
parameterised by `hfe_ticks_per_cell(bitRate)` (already computed and unit-
tested here) rather than a hard-coded constant.

## 7. Public API (`src/hfe.h`)

```cpp
uint32_t hfe_ticks_per_cell(uint16_t bit_rate_kbit_s);      // 20000/bitRate
int      hfe_to_scp(const uint8_t* hfe, size_t len,
                    std::vector<uint8_t>& out);              // 0 or HFE_E_*
```

Error codes: `HFE_E_NOT_HFE` (bad signature), `HFE_E_UNSUPPORTED` (v3 /
non-v1 revision / non-250 bitrate / >84 tracks), `HFE_E_TRUNCATED`
(header/LUT/track data past EOF), `HFE_E_GEOMETRY` (0 tracks, or the
assembled bitcell set is degenerate — e.g. every track unformatted). On any
error `out` is left empty. All parsing is bounds-checked against `len`; no
allocation is performed before the truncation checks pass.

## 8. Test vectors (`test/hfe_test.cpp`)

- **`HfeTicksPerCell.ConcreteNumbers`** — 250→80, 500→40, 125→160, 0→0, and
  300→66 (an inexact/unsupported rate that truncates — proving the gate).
- **`Hfe.BitOrderIsLsbFirstWithinEachByte`** — side-0 byte `0xAA` (LSb-first
  logical cells `0,1,0,1,0,1,0,1`) → four 160-tick intervals; an MSb-first
  misread would start with an 80-tick interval, which the test rejects.
- **`Hfe.TranscodesTwoTrackImageIntoAWellFormedScp`** — a full 512-byte block
  (256 B side 0 + 256 B side-1 `0xFF` filler) → header/TLUT/revolution entry
  checked byte-for-byte, one 80-tick transition, side-1 filler proven
  de-interleaved out (a broken split would explode the word count), absent
  track 1 → empty slot, container checksum verified, engine-side
  `flux_scp_probe`/`_revolutions`/`_cylinders` agree.
- **`Hfe.DeinterleavesAcrossMultipleBlocks`** — a two-block (1024 B) track;
  transitions at logical cells 0 and 2048 exercise the repeating split loop
  *and* the SCP 16-bit overflow encoding (`163840 = 2×65536 + 32768`).
- **`Hfe.BothCylindersPresentUseEvenSlots`** — two formatted tracks → side-0
  slots 0 and 2, odd slots absent, `flux_scp_cylinders == 2`.
- Rejection: too-short buffer, bad signature, `HXCHFEV3`, non-v1 revision,
  zero tracks, >84 tracks, non-250 bitrate, LUT-past-EOF, track-data-past-EOF,
  all-unformatted.
- **`Hfe.DecodesARealHfeCaptureWhenProvided`** — optional oracle, gated on
  `KONCEPCJA_REAL_HFE` env var or `test/hw/fixtures/real.hfe`; SKIP when
  absent (never commit preservation dumps), mirroring the IPF/SCP harnesses.
  Runs the full `hfe_to_scp → flux_scp_to_dsk` chain and asserts sectors
  decode.

## 9. Limits / deferred

- **v1 only.** v3 opcode stream (`F0` NOP, `F1` SET INDEX, `F2 BB` SET
  BITRATE, `F3 LL` SKIP BITS, `F4` RAND weak-cells) deferred until a real
  protected HFE needs loading. v2 (`formatrevision == 1`) also out of scope.
- **bitRate 250 only** (see §6). Other rates rejected, not mis-timed.
- **Side 0 only**, one revolution — same constraints as the IPF mirror.
- **Not wired into the app.** Loader registration, extension dispatch, and
  `t_drive` host-view population are the separate integration pass
  (`flux-ingestion-contract.md` §4). Note the `.hfe` extension has no
  collision like KryoFlux's `.raw`, so dispatch is by extension + signature.
