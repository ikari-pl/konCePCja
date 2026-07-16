# KryoFlux STREAM (`.raw`) format — clean-room spec + transcoder notes

Status: **implemented** (transcoder module, no app dispatch yet). Companion to
[`flux-formats-feasibility.md`](flux-formats-feasibility.md) §2.2 and
[`flux-ingestion-contract.md`](flux-ingestion-contract.md). Implemented by
`src/kryoflux_stream.{h,cpp}`, tested by `test/kryoflux_stream_test.cpp`.

## 1. Scope & provenance

Clean-room, from **public documentation only**: *KryoFlux Stream File
Documentation* **Rev 1.1** (Jean Louis-Guérin / DrCoolZic, 2013-12-01, marked
"Copyleft"), corroborated by the softpres.org `kryoflux:stream` wiki, with
numeric constants only cross-checked against **fluxfox** (MIT). No GPL reader
(greaseweazle, disk-utilities, SAMdisk, libhxcfe) and no `src/capsimg` code was
consulted.

KryoFlux STREAM is **flux-native** (raw transition timings), so — exactly like
`src/hw/a2r.cpp` for Applesauce A2R — we **transcode the container into SCP
bytes** and reuse the existing `src/hw/flux` PLL + IBM System-34 MFM decoder
unchanged. No new PLL, no bit recovery, no sector scanner.

## 2. Clocks

- `mck = ((18432000 × 73) / 14) / 2 = 48054857.142857 Hz`
- `sck = mck / 2 = 24027428.571428 Hz`  (flux sample clock)
- `ick = mck / 16 = 3003428.571428 Hz`  (index sample clock)

Flux and index intervals are counted in **sck ticks**. A `KFInfo` OOB block may
carry explicit `sck=`/`ick=` `name=value` pairs; when present these **override**
the defaults (`kryoflux_decode_stream` parses `sck=`).

### sck → SCP 25 ns conversion

SCP flux words are 25 ns ticks (we set the SCP **resolution byte to 0**, so one
tick = `25 ns × (0+1) = 25 ns`, matching the IPF mirror; the decoder's nominal
half-cell is `80/(0+1) = 80` ticks = 2 µs).

```
ticks_25ns = sck_ticks × (1e9 / 25) / sck_hz = sck_ticks × 40000000 / sck_hz
```

For the **default** sck this is the **exact rational 4375 / 2628**, because
`40000000 / 24027428.571428 = 40000000 × 56 / 1345536000 = 4375 / 2628`.

Worked check: a nominal 2 µs DD flux interval is `2000 / (1e9/sck) ≈ 48.05` sck
ticks; `48 × 4375/2628 = 79.909 → 80`, and `80 × 25 ns = 2000 ns = 2 µs`. We
round to nearest and **difference cumulative** per-revolution times, so total
revolution time stays drift-free.

## 3. In-stream (flux buffer) block table

By header byte:

| Header | Block | Bytes | Value / effect |
|---|---|---|---|
| `0x0E`–`0xFF` | Flux1 | 1 | value = header |
| `0x00`–`0x07` | Flux2 | 2 | value = `(header<<8) | data[1]` |
| `0x0C` | Flux3 | 3 | value = `(data[1]<<8) | data[2]` |
| `0x0B` | Ovl16 | 1 | add `0x10000` to the **next** flux value (stackable) |
| `0x08` | Nop1 | 1 | padding, no flux |
| `0x09` | Nop2 | 2 | padding, no flux |
| `0x0A` | Nop3 | 3 | padding, no flux |
| `0x0D` | OOB | — | out-of-band block (§4) |

The **stream position** counter advances by the byte length of every flux/Nop
block, and is **not** advanced by OOB blocks (they are "out of band"). Overflow
via `Ovl16` is stackable: N consecutive `Ovl16` add `N × 0x10000` to the flux
value that follows. The transition's stream position is recorded at the **first
Ovl16** (the start of its encoding), not at the trailing flux byte.

## 4. OOB blocks

Header: `0x0D`, `type` (1 byte), `size` (2 bytes **little-endian**), `body`.

| type | Block | body | Use here |
|---|---|---|---|
| `0x00` | Invalid | — | skipped |
| `0x01` | StreamInfo | 8 | skipped |
| `0x02` | Index | 12 | `StreamPosition` (u32 LE), `SampleCounter` (u32 LE), `IndexCounter` (u32 LE) |
| `0x03` | StreamEnd | 8 | skipped |
| `0x04` | KFInfo | ASCII | parsed for `sck=` |
| `0x0D` | EOF | — | **stop parsing**; the size field is a sentinel — do **not** consume it |

## 5. Two-pass index / flux alignment (the one hard part)

OOB Index blocks are asynchronous to the flux buffer. `StreamPosition` points at
the **next** flux transition (in-band byte offset), and `SampleCounter` measures
sck ticks from the **previous** flux transition to the index pulse
(per feasibility §2.2).

- **Pass 1** (`scan_stream`): walk once, recording every flux transition tagged
  with its in-band start position and interval (sck), every Index
  (`StreamPosition`, `SampleCounter`), and any `sck=` override.
- **Pass 2** (`kryoflux_decode_stream`): bind each index to `fi` = the first
  flux whose start position `>= StreamPosition` (binary search). With
  `T(k)` = absolute time of transition `k` (prefix sum) and `T(-1) = 0`:
  - `I(i) = T(fi-1) + SampleCounter(i)` — absolute index time.
  - Revolution `i` spans flux `[fi(i) .. fi(i+1)-1]`; word `k` = differenced
    `(T(k) - I(i))` in 25 ns; the first word is `interval(fi) - SampleCounter(i)`
    = index-to-first-transition (the SCP per-revolution convention).
  - Revolution `i` duration = `I(i+1) - I(i)`.

M indices ⇒ up to **M−1** complete revolutions. Documented edge cases handled:
index before any flux (`fi == 0` ⇒ previous time 0), index after the last flux
(`fi == nflux` ⇒ closes a revolution, opens none), and flux-value overflow via
stacked `Ovl16` right before an index.

## 6. Mapping to the SCP seam

Output is an in-memory SCP byte image identical in shape to `a2r_to_scp`'s: a
16-byte header (`"SCP"`, revolutions at `0x05`, flags `0x01`, cell width 0,
heads 0, **resolution 0**), the 168-entry LE track-offset table at `0x10`, then
per-track `"TRK"`+slot, `nrevs × 12`-byte revolution entries
(`duration`, `words`, `offset-relative-to-TDH`, all u32 LE), and the flux data
as **big-endian** 16-bit words with the `0x0000` → +65536 carry rule. Track slot
= `cyl×2 + side` (side-0/even slots are what the decoder consumes).

Feed the buffer to `Machine::insert_flux` / `flux_scp_to_dsk` — no new decoder.

## 7. One file per track (the KryoFlux reality)

Real dumps are **one file per track and side** (`track00.0.raw`,
`track00.1.raw`, …), each carrying several revolutions of one physical track.
Accordingly:

- `kryoflux_decode_stream` / `kryoflux_stream_to_scp` decode a **single stream
  (one track)** — the primary entry point.
- `kryoflux_streams_to_scp` assembles a **multi-track SCP** from a
  `{data, len, cyl, side}` set, normalizing all tracks to the shared minimum
  revolution count (SCP carries one global revolution count). The dispatch layer
  (a later, separate pass) will glob the `.raw` siblings and supply this set.

## 8. API

```cpp
uint32_t kryoflux_sck_to_25ns(uint64_t sck_ticks, double sck_hz);   // pure
int kryoflux_decode_stream(const uint8_t* data, size_t len, KryoFluxTrack& out);
int kryoflux_stream_to_scp(const uint8_t* data, size_t len,
                           uint8_t cyl, uint8_t side, std::vector<uint8_t>& out);
int kryoflux_streams_to_scp(const std::vector<KryoFluxMember>& members,
                            std::vector<uint8_t>& out);
```

Errors (negative): `KFSTREAM_E_TRUNCATED`, `_NO_INDEX`, `_NO_FLUX`, `_BAD_OOB`,
`_GEOMETRY`. All parsing is bounds-checked; hostile/truncated input is rejected
without out-of-bounds reads.

## 9. Limits

- Side-0 only in practice: the flux decoder reads even slots (`cyl×2`), so side-1
  (odd) slots are stored but not decoded — matches the documented single-sided
  consumption limit; fine for single-sided CPC 3″ media.
- `SampleCounter` is interpreted as "sck ticks from the previous flux" per the
  feasibility spec; the boundary split is exact under that reading and the PLL
  re-locks regardless, so residual sub-tick imprecision is immaterial.
- No app dispatch wiring yet (extension collision on `.raw` with the CAPS RAW
  path must be resolved by content-sniff at the dispatch layer — see
  feasibility §5.1). This module is the transcoder only.
```
