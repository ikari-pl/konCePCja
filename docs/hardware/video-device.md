# Video (Gate Array pixel path) — clean-room Device implementation reference

Spec for the clean-room video renderer (`src/hw/video`), the Gate Array's video-fetch
and pixel-generation path. Companion to `gate-array-device.md`, `crtc-device.md`,
`memory-device.md`. It turns the CRTC's MA/RA + the GA's palette/mode into pixels.

## 1. Architecture (this slice)

Physically the GA does the video DMA and pixel shifting. Here the pixel math lives in
**pure, testable functions**; a thin renderer ties them to the CRTC state, palette, and
RAM. It **watches the GA's pen/ink/mode I/O independently** (like the memory Device) to
track its own copy of the mode + 16-pen + border palette.

**Deferred (documented):** the bus-accurate video DMA — on real hardware the video
fetch takes the µs half-cycle the CPU doesn't, driving MA onto the address bus. This
slice reads video RAM directly; the interleaved bus fetch is a later refinement.

## 2. MA/RA → video RAM byte address

Two bytes per displayed character. The CPC bit-shuffle (confirm exact masks from
video.cpp — TBD-from-research):

    byte_addr(MA, RA, k) = ((MA & 0x3000) << 2) | ((RA & 7) << 11)
                         | ((MA & 0x03FF) << 1) | k        (k = 0,1)

With the standard start (R12=0x30 → MA base 0x3000) the top bits give RAM 0xC000 — the
CPC screen. RA (0..7) contributes the 2K scanline stride.

## 3. Pixel decode per mode (exact bit arrangement — TBD-from-research)

The fetched byte(s) decode to pen indices; the arrangement is mode-specific and
interleaved. Placeholders to confirm against the legacy decode tables:

- **Mode 0** — 160 px/line, 16 colours, **2 px/byte** (4-bit pen). Interleave e.g.
  pixel0 pen = bits {b7,b3,b5,b1}, pixel1 = {b6,b2,b4,b0}. [confirm]
- **Mode 1** — 320 px/line, 4 colours, **4 px/byte** (2-bit pen). [confirm bit pairs]
- **Mode 2** — 640 px/line, 2 colours, **8 px/byte** (1-bit pen), MSB first. [confirm]

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
