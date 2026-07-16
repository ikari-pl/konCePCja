# Video (Gate Array pixel path) — sub-cycle, pin-level Device simulation reference

Spec for the sub-cycle video renderer (`src/hw/video`), the Gate Array's video-fetch
and pixel-generation path. Companion to `gate-array-device.md`, `crtc-device.md`,
`memory-device.md`. It turns the CRTC's MA/RA + the GA's palette/mode into pixels.

## 1. Architecture (this slice)

Physically the GA does the video DMA and pixel shifting. Here the pixel math lives in
**pure, testable functions**; a thin renderer ties them to the CRTC state, palette, and
RAM. It **watches the GA's pen/ink/mode I/O independently** (like the memory Device) to
track its own copy of the mode + 16-pen + border palette.

**Bus-accurate video DMA (implemented):** the renderer holds **no memory pointer** —
every displayed byte travels over the RAM fetch bus (`Bus.ram`). Each µs the Gate
Array drives the shuffled MA/RA address during its two video slots (drive at phase 12
and 14); the memory Device answers with the display byte one master cycle later. The
renderer latches byte 0 when the bus carries `clk.phase == 13`, latches byte 1 and
paints the whole character cell (or the border when DISPEN is low) at `phase == 15`,
then advances the beam column. The CPU's accesses occupy the complementary slot
(phases 0–11), so the two never contend — see `gate-array-device.md` §2.

## 2. MA/RA → video RAM byte address

Two bytes per displayed character. The CPC bit-shuffle (confirm exact masks from
video.cpp — TBD-from-research):

    byte_addr(MA, RA, k) = ((MA & 0x3000) << 2) | ((RA & 7) << 11)
                         | ((MA & 0x03FF) << 1) | k        (k = 0,1)

With the standard start (R12=0x30 → MA base 0x3000) the top bits give RAM 0xC000 — the
CPC screen. RA (0..7) contributes the 2K scanline stride.

## 3. Pixel decode per mode (exact bit arrangement)

The fetched byte decodes to pen indices, MSB→leftmost pixel. The **pen-plane bit
significance** is the trap: getting it backwards swaps pens (e.g. pen 1 ↔ pen 2, so
mode-1 yellow text renders as cyan). **Authority: the cpcwiki Gate Array "Video
memory structure" table** (`references/cpcwiki/Gate_Array.txt:369`), which pins the
mode-0 byte (bit7→bit0) as `A0 B0 A2 B2 A1 B1 A3 B3` — A = left pixel, B = right, the
digit is the pen-bit index. (An earlier version of this section swapped the A1/A2
planes, i.e. `b3↔b5` — the exact mode-0 decode bug fixed in commit `2313200`; the
tests `video_test.cpp::ModeDecode0Hardware` are the ground truth, not any prior text.)

- **Mode 0** — 160 px/line, 16 colours, **2 px/byte** (4-bit pen). From the table,
  pixel A's pen = `A0|(A1<<1)|(A2<<2)|(A3<<3)` = `b7|(b3<<1)|(b5<<2)|(b1<<3)`:
  `px0 = (b1<<3)|(b5<<2)|(b3<<1)|b7`, `px1 = (b0<<3)|(b4<<2)|(b2<<1)|b6`.
- **Mode 1** — 320 px/line, 4 colours, **4 px/byte** (2-bit pen). For pixel *k*
  (0..3): **high plane = b(3-k)**, **low plane = b(7-k)** →
  `pen_k = (b(3-k)<<1) | b(7-k)`.
- **Mode 2** — 640 px/line, 2 colours, **8 px/byte** (1-bit pen), MSB first:
  `px_k = b(7-k)`.

Each pen (0..15) → GA ink (0..31 hardware colour) → RGB (§4). The border uses pen 16's
ink and paints where DISPEN is inactive.

## 4. Hardware colour → RGB (TBD-from-research)

The GA ink registers hold a 0..31 hardware-colour index; the CPC has 27 distinct
colours (3 levels per channel). A 32-entry RGB table maps index → (R,G,B). [copy the
exact table from the legacy palette.]

## 5. Verification (golden pixel data)

Unlike the cycle/state unit tests elsewhere, validate against **known byte→colour**
mappings:
- `byte_addr` for representative MA/RA (incl. the 0x3000→0xC000 base) matches the table.
- Decode a known byte in each mode → expected pen sequence.
- A pen→ink→RGB round-trip for a small palette.
- A full active-line render: given RAM + palette + mode, the emitted pixel colours match
  the expected row.

## 6. Device model

Pure functions: `vid_byte_addr`, `vid_decode_mode0/1/2`, `vid_ink_rgb`. The renderer
`tick(in,out)` watches palette/mode I/O and, on DISPEN char cycles, fetches + decodes
into a framebuffer. Caller-owned framebuffer + no-heap state.

## Batch contract (RunTier::Fast)

- **Catch-up renderer**: keeps `rendered_until`; renders CRTC characters in
  scanline batches via vid_render_line / vid_decode_lut (already built and
  byte-identity proven). THE INVARIANT: every GA/CRTC/ASIC register write
  catches rendering up to the write's timestamp BEFORE the write applies —
  that is what keeps µs-level raster tricks (mode/ink splits, RUPTURE-class
  demos) pixel-exact at instruction granularity. Scanline-lazy alone is NOT
  sufficient; catch-up-on-write is.
- **Inputs at render time**: CRTC edge/geometry events (crtc-device.md),
  GA mode/ink state, RAM read directly (video port ignores overlays).
- **Plus compositing**: per-scanline, SPRITE-MAJOR with a per-line active
  list (the measured lesson: pixel-major × 16 was the hot path we culled;
  SIMD there was 28% slower — beads-hvgg). 12-bit palette + hscroll carry
  per the sections above.
- **Frame completion**: the VSYNC-rise event IS the frame boundary — no
  per-cycle peeking exists in this tier.
- Bestiary audit: hsync/vsync edge latches become real events (class-safe);
  no free-running counters; no publishes; drives nothing.

Implementation: `video_batch_cells` (`video.h` §batch) consumes CRTC
character views (`crtc_advance_view` + the chain-stamped GA mode) and drives
the SAME `video_state` beam the per-cycle `video_tick` does, painting through
the shared `render_cell_classic` — one cell-paint definition for both shapes.
Inks are read once per render slice (any write forces catch-up first); the
byte-0 fetch latch is kept bus-exact per char. Uniform apply point (one-hop
latency; memory-write T1s are µs-aligned): everything with T1 in µs j applies
after char j's CRTC advance, before cell j's render. Cell k paints at master
16k+16 — the comparison anchor. A write-triggered VSYNC rise patches the
pending char-j view (level + edge), mirroring the mid-µs beam reset.
ORACLES: `FastVideoRender.*` — static screen, 300 Hz ink/mode raster bands,
and ~19 µs sub-scanline ink flips, all pixel-identical vs the per-cycle
device. The Plus compositor batches in F7.
