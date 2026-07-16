# Spatial maps — the beam, the address shuffle, the memory geography

**Representation:** spatial/2-D maps.
**The state it makes legible:** state that is *physically* spatial — where the
beam is, how a character cell scatters into RAM, which chip answers each address
band — but which every classic debugger flattens into a column of hex.

> A register table tells you `MA = 0x3050`. It does not tell you that the beam is
> two thirds of the way down the screen, one character past the left border, and
> that the byte it is about to fetch lives at RAM `0xE0A1` — nowhere near
> `0x3050`. The numbers *encode* a geometry the CPC's designers laid out in
> silicon. These maps draw the geometry back.

Three maps, each grounded in a real peek surface:

1. **The raster map** — the beam as a moving cursor over the framebuffer +
   border, the CRTC `MA` it is fetching, and (on the Plus) the sprite-compositing
   stack.
2. **The address-shuffle map** — `vid_byte_addr`: why screen memory is scattered
   the way it is.
3. **The memory-geography map** — the Z80's 64K as horizontal bands, each labelled
   with the physical page/ROM that answers it, and the `/ROMDIS`·`/RAMDIS`
   overlays punching through.

All three read from peek surfaces that already exist: `VideoRegs`
(`video_peek`), `CrtcRegs` (`crtc_peek`), `AsicRegs` (`asic_peek`), and
`MemRegs` (`mem_peek`) + `mem_peek_cpu`. Nothing here needs a new back-channel:
every value shown travelled over the real bus.

---

## 1. The raster map — the beam as a cursor of causality

### 1.1 The canvas the code actually paints

`src/hw/video.cpp` does not render a frame in one pass — it paints one character
cell per two RAM fetches, wherever the beam happens to be. The geometry is fixed
by three constants:

```
kVisChars   = 48      // visible character columns (the monitor window)
kVBackPorch = 36      // scanlines from the VSYNC edge to the top visible line
char_w      = fb_w / kVisChars   // = 16 px for the default 768-wide canvas
```

The beam lives in `video_state` as two integers:

```
beam_col : 0 .. kVisChars-1     (visible column; advances every phase-15 char)
beam_row : -kVBackPorch .. fb_h (scanline; -36 at the VSYNC edge, climbs)
```

So the visible framebuffer is a **48 × N grid of 16-px-wide cells**, and the beam
is a single cell-cursor sweeping it left-to-right, top-to-bottom. That grid is
the natural canvas for the map — not the 780×288 pixel buffer, but the 48-column
*character* lattice the hardware fetches on.

### 1.2 The map

Each cell is classified from the committed `Bus` at the moment the beam passed
over it: `in->vid.hsync`, `in->vid.vsync` (visible gate), and `in->vid.dispen`
(active display vs border). That is exactly the three-way branch in
`render_char()`:

```
                         beam_col →   (0 .. 47, each cell = 16 px = 2 bytes)
      0        8                       24                              40      47
    ┌──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
 -36│##│##│##│##│##│##│##│##│##│##│##│##│##│##│##│##│##│##│##│##│##│##│##│##│ ← beam_row < first_active_row
  ↓ │##│##│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│##│##│  TOP BORDER (dispen=0)
    ├──┼──┼──╆━━╈━━╈━━╈━━╈━━╈━━╈━━╈━━╈━━╈━━╈━━╈━━╈━━╈━━╅──┼──┤
  b │##│##│##┃                ACTIVE DISPLAY               ┃##│##│  dispen=1
  e │##│##│##┃  cell(col,row) ← fetch0·fetch1 from RAM     ┃##│##│  fill_run() per pen
  a │##│##│##┃  MA advances +1 per cell, +R1 per char-row  ┃##│##│
  m │##│##│##┃                                             ┃##│##│
    │##│##│##┃━━━━━━━━━━━━━ SPLIT LINE (asic.split_line) ━━┃##│##│ ← MA base ⇄ split_addr
  r │##│##│##┃  cells below fetch from split_addr + …      ┃##│##│
  o │##│##│##┃                                             ┃##│##│
  w │##│##│##┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛##│##│
    │##│██│◀─ BEAM (beam_col=2, beam_row here) — the live cursor  │  BOTTOM BORDER
    └──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘
      ##=border(pen16)   ▓=border  ┃…┃=active   ██=beam   ━━=split seam
```

Legend maps 1:1 to code paths:

| Glyph | Cell state | Code |
|---|---|---|
| `##` / `▓` | visible but `dispen=0` → border | `fill_run(x0, char_w, g.ink[16])` |
| interior | `dispen=1` → active, 2 decoded bytes | `vid_decode` → `fill_run` per pen |
| `██` | the beam this instant | `beam_col`, `beam_row` |
| `━━` | Plus split seam | CRTC swaps base to `asic.split_line`/`split_addr` |
| off-grid | `hsync`/`vsync` or outside `0..kVisChars` | `visible` gate false → nothing painted |

### 1.3 Each active cell knows its own MA

The map's real power is that every active cell can be labelled with the CRTC
address that fed it. During a frame the address walks exactly as `vid_render_frame`
documents: `ma_base = (ma_start + row*R1) & 0x3FFF`, then `+1` per column. Hover a
cell and the map shows the tuple the fetch actually used:

```
   cell (col=17, row=42)   MA = 0x3051   RA = 3
        │
        ├── byte k=0 → vid_byte_addr(0x3051, 3, 0) = 0xD8A2  ← RAM read
        └── byte k=1 → vid_byte_addr(0x3051, 3, 1) = 0xD8A3  ← RAM read
        decoded in mode 1 → pens [1,1,2,0, 1,3,0,0] → ink[] → RGB
```

This is the causal link a hex dump severs: the pixels you see at column 17 came
from RAM `0xD8A2/0xD8A3`, and the map draws the arrow. Click the cell → jump the
Memory window to `0xD8A2`; click the MA → jump the disasm/GFX finder. The beam is
literally *the cursor of causality* — it points at the RAM byte currently being
turned into light.

### 1.4 The Plus compositing stack — layers, not a number

On `model=3` the active-cell branch changes to `render_char_plus()`, and a single
"pen index" becomes a **stack of layers** resolved per pixel. The map draws that
stack as an exploded Z-order for the pixel under the cursor:

```
   Beam pixel (active-relative px_x=276, px_y=130), Plus mode
   ───────────────────────────────────────────────────────────
   layer 3  SPRITE          plus_sprite_at(px_x,px_y)
            sprite 3, cell(4,7) = 0x5  → index 16+5 = 21  ┐ opaque (s≠0)
                                                          │ wins → 21
   layer 2  SPRITE 0..15    priority high→low, first hit  │
                                                          ┘
   layer 1  BACKGROUND PEN  plus_decode_cell → cell_pen[lx] = 2   (loses)
   ───────────────────────────────────────────────────────────
   layer 0  12-bit PALETTE  plus_index_rgb(index=21)
            pal_set[21]=1 → RRRR·GGGG·BBBB = 0xF·0x8·0x0 ×17
                          → RGB (255,136,0)   ← the pixel emitted
```

The resolution rule is the code, verbatim: `plus_sprite_at` scans sprites 0→15
and returns `16 + s` for the first opaque hit (sprite 0 highest priority),
otherwise the background `cell_pen[lx]` survives; then `plus_index_rgb` maps the
winning index — a programmed entry (`pal_set[index]`) expands its 4-bit channels
`×17`, an unprogrammed screen/border index (`≤16`) falls through to the classic
`ink[]` colour, and an unprogrammed sprite index is black. Rendering the stack as
tiles you can toggle (hide sprite layer, show background only) turns "why is this
pixel orange?" into a glance.

Because sprite attributes and the palette are re-latched **once per scanline**
(`plus_refresh_line` on `hs_fall`), the map can show the split visually: sprites
snapping to new positions at a raster boundary is a *row* discontinuity in the
grid, not a mystery. Overlay `first_active_row` (the sprite Y origin, re-set each
VSYNC) as the grid's row-0 datum so the sprite coordinate system lines up with
what you see.

### 1.5 Worked frame — a mode-1 screen with a split at row 100

```
 CrtcRegs: R1=40 (chars/line displayed), R6=25 rows, R9=7 → 200 active lines
 VideoRegs.mode = 1     AsicRegs.split_line = 100, split_addr = 0x0000

 beam_row  region        MA base for the row           fetch source
 ────────  ──────────    ─────────────────────────     ─────────────
   -36..-1 TOP BORDER     (no fetch, dispen=0)          pen 16
     0..99 ACTIVE (top)   0x3000 + (row/8)*40           screen at &C000-ish
   100     ── SPLIT ──    base ⇄ split_addr = 0x0000    screen at &0000-ish
   100..199 ACTIVE (bot)  0x0000 + ((row-100)/8)*40     split screen
   200..    BOTTOM BORDER (no fetch)                    pen 16
```

The split line is a horizontal rule across the grid where the address column's
value teleports from the `0x3000` neighbourhood to `0x0000` — the CRTC's
`crtc_attach_asic` base swap, made visible as a seam.

### 1.6 Live beam overlay on the emulated screen

Because `beam_col`/`beam_row` are live in `video_state`, the DevTools can draw the
cursor **directly on the real emulated display** — a crosshair or a fading comet
tail marking the last N cells the beam painted. Pause mid-frame and the half-drawn
framebuffer already tells the truth (the renderer paints as it goes); the overlay
just names the frontier. This is the single most visceral view: *stop time and see
exactly how far the electron gun got.* Raster-split effects, `dispen` overscan
tricks, and mid-frame `MODE` changes all become watchable as the beam crosses the
boundary you set them at.

---

## 2. The address-shuffle map — why screen RAM is scattered

### 2.1 The one line that scrambles the screen

```c
uint16_t vid_byte_addr(uint16_t ma, uint8_t ra, uint8_t k) {
  return ((ma & 0x3000) << 2)   //  bits 13:12 of MA → address bits 15:14
       | ((ra & 0x07)   << 11)  //  scanline 0..7    → address bits 13:11
       | ((ma & 0x03FF) << 1)   //  bits  9:0 of MA  → address bits 10:1
       | (k & 1);               //  which of 2 bytes → address bit 0
}
```

This is the whole reason CPC screen memory is "weird". A hex dump of `&C000`
shows you a wall of bytes with no hint that **consecutive scanlines of the same
text row live 2 KB apart**. The map draws the scatter.

### 2.2 The bit-braid

```
   CRTC address (MA, RA, k)                RAM byte address (16 bits)
   ────────────────────────                ──────────────────────────
   MA13 MA12 ─────────────────────────────► A15 A14      (which 16K "third")
   RA2  RA1  RA0 ─────────────────────────► A13 A12 A11  (scanline within row)
   MA9..MA0  (10 bits) ────────────────────► A10..A1      (char column ×2)
   k (0/1) ───────────────────────────────► A0           (even/odd byte)

   MA:  [13 12│11 10 9 8 7 6 5 4 3 2 1 0]
          │└�! discarded (11,10 fold via &0x3FF, but 11..10 ARE kept in low field)
          └───────────────► top of address
   note: (MA & 0x03FF) keeps MA bits 9:0; MA bits 11:10 are dropped from the
         low field and only MA 13:12 survive (as A15:14). This is the classic
         16K screen window: MA effectively addresses 0..0x3FF chars × 8 lines.
```

### 2.3 The scatter, drawn as a memory strip

Take one displayed character row (RA sweeps 0→7 at a fixed `ma_base`). On screen
those 8 scanlines stack vertically and touch. In RAM they explode:

```
   On-screen char cell (col c, char-row r):  8 stacked scanlines
   ┌────────────────┐  RA=0  ── RAM:  base + 0x0000
   │████████████████│  RA=1  ── RAM:  base + 0x0800   (+2048!)
   │██            ██│  RA=2  ── RAM:  base + 0x1000
   │██  a glyph   ██│  RA=3  ── RAM:  base + 0x1800
   │██            ██│  RA=4  ── RAM:  base + 0x2000
   │████████████████│  RA=5  ── RAM:  base + 0x2800
   │██            ██│  RA=6  ── RAM:  base + 0x3000
   └────────────────┘  RA=7  ── RAM:  base + 0x3800

   where base = ((MA&0x3000)<<2) | ((MA&0x3FF)<<1) | k
   Each +1 in RA jumps A13:11 → a 2 KB stride. The eight rows of ONE glyph
   are smeared across a 16 KB page.
```

And the *reverse* view — a linear RAM strip coloured by where each byte lands on
screen — shows the interleave the other way:

```
   RAM &C000 ────────────────────────────────────────────────► &FFFF
   │RA0 row0│RA0 row1│ … │RA0 row24│RA1 row0│RA1 row1│ … │RA7 row24│
   └────────┴────────┴───┴─────────┴────────┴─────────────────────┘
    ▲ contiguous run = one scanline of the WHOLE screen (all 25 text rows'
      RA=0 lines), then the next RA. Vertical screen neighbours are 2 KB apart.
```

### 2.4 Interactive cross-probe

Two-way linking makes it a tool, not a poster:

- **Screen → RAM:** pick a cell in the raster map (§1) → highlight its two bytes
  `vid_byte_addr(ma, ra, 0/1)` in the Memory hex window.
- **RAM → screen:** hover a byte in the hex window → the map inverts the shuffle
  and lights the (col, row, RA) cells it feeds. A single `POKE` lights up to 8
  pixel-columns; a 2 KB region lights one scanline of the entire display.

This is what a hex dump *cannot* show: the byte at `&C800` is not "near" `&C000`
on screen — it is the **second scanline of the top text row**. The map is the
decoder ring for that fact.

---

## 3. The memory-geography map — 64K as layered terrain

### 3.1 The base terrain: which page answers each band

The Z80 sees a flat 64K, but underneath, the 6128 PAL splits it into four 16K
slots, each pointing at a physical page selected by `ram_config` (the
`11 bbb ccc` latch in `MemRegs`). Draw the address space as four horizontal
bands and label each with its page under the current `ccc`:

```
   ram_config = 0xC0 → ccc=0 (all base)          ram_config = 0xC2 → ccc=2
   Z80 addr        physical page                 Z80 addr        physical page
   ┌──────────┐                                  ┌──────────┐
   │0000–3FFF │  page 0  (base)                  │0000–3FFF │  page 4  (exp)
   ├──────────┤                                  ├──────────┤
   │4000–7FFF │  page 1  (base)                  │4000–7FFF │  page 5  (exp)
   ├──────────┤                                  ├──────────┤
   │8000–BFFF │  page 2  (base)                  │8000–BFFF │  page 6  (exp)
   ├──────────┤                                  ├──────────┤
   │C000–FFFF │  page 3  (base)                  │C000–FFFF │  page 7  (exp)
   └──────────┘                                  └──────────┘  whole 64K = bank
```

The eight PAL maps are exactly the table in `memory-device.md §2b`; the map
renders whichever row `ccc` currently selects, and annotates the expansion bank
`bbb` (dk'tronics) / 6-bit Yarek `(ext<<3)|bbb` feeding pages 4–7. Change RAM
config live and the bands re-label — you *watch* CP/M+ config 3 alias base page 3
into slot 1, or config 2 swing the whole address space onto the expansion bank.

### 3.2 The overlay layers: transparency stacked on the terrain

The base pages are the ground floor. On top sit ROM and expansion overlays that
**punch through** for reads — the `/ROMDIS`·`/RAMDIS` mechanism from
`memory-device.md §4b`. Render them as translucent sheets, topmost = winner:

```
                 0000            4000            8000            C000       FFFF
   ┌─────────────┬───────────────┬───────────────┬───────────────┬──────────┐
 L4│ MF2 freeze RAM (ramdis) ── punches through anywhere it pages ──────────│ ← highest
   ├─────────────┼───────────────┼───────────────┼───────────────┼──────────┤
 L3│             │ ASIC reg page │ (model 3, when unlocked: locked=0)       │
   │             │  &4000-&7FFF  │  romdis+ramdis overlay + 1-tick latch    │
   ├─────────────┼───────────────┼───────────────┼───────────────┼──────────┤
 L2│ LOWER ROM   │               │               │ UPPER ROM (rom_select)   │ ← GA enables
   │ rom_cfg b2=0│               │               │ rom_cfg b3=0             │
   ├─────────────┼───────────────┼───────────────┼───────────────┼──────────┤
 L1│ page(ccc,0) │ page(ccc,1)   │ page(ccc,2)   │ page(ccc,3)              │ ← base terrain
   └─────────────┴───────────────┴───────────────┴───────────────┴──────────┘
   READ resolves top-down: first layer whose gate is asserted wins.
   WRITE ignores L2/L3 ROM sheets — always lands in L1 banked RAM
        (unless L4 ramdis vetoes it via the one-tick write latch).
```

Each layer's gate is a real bit from a peek surface:

| Layer | Region | Gate (from peek) | Notes |
|---|---|---|---|
| L1 base RAM | all 64K | `MemRegs.ram_config` (`ccc`,`bbb`), `ram_ext` | the terrain; the write target |
| L2 lower ROM | `0000–3FFF` | `rom_config` bit2 == 0 | firmware; disable reveals RAM beneath |
| L2 upper ROM | `C000–FFFF` | `rom_config` bit3 == 0 | `rom_select` (&DFxx) picks which of 256 |
| L3 ASIC page | `4000–7FFF` | `AsicRegs.plugged && !locked` | register page overlay |
| L4 MF2 / silicon disc | expansion-defined | committed `cpu.romdis`/`cpu.ramdis` | freeze RAM veto via 1-tick latch |

### 3.3 Reads vs writes split the geography in two

The single most clarifying thing the map shows: **read-terrain and write-terrain
are different maps.** A hex dump conflates them; the geography map draws both.

```
   Address  0xC000                     READ view          WRITE view
   ─────────────────────────────       ───────────        ───────────
   rom_config b3 = 0 (upper ROM on) →   UPPER ROM byte     banked page 3 RAM
                                        (BASIC/AMSDOS)     (write passes through)
```

Writes "pass *through* any ROM overlay to the RAM beneath" (memory-device.md §1).
So the same address can *show* firmware and *store* your data — the classic CPC
trick of running from ROM while keeping a RAM scratchpad underneath. Two side-by-side
64K strips (READ | WRITE), each coloured by winning layer, make it obvious where
they diverge.

### 3.4 The video-fetch caveat as a third strip

Video DMA is a third, distinct addressing regime worth its own strip: the GA's
fetch **always reads base-64K RAM**, never the ROM overlay and never the banked
expansion page (memory-device.md §1, §2b). So expansion RAM is *invisible to the
screen* by construction. Drawing the fetch strip next to the CPU-read strip shows
why: the beam and the CPU can look at the "same" address and get different bytes.

```
   0xC000        CPU READ            CPU WRITE          VIDEO FETCH
                 upper ROM           banked RAM pg3     base-64K RAM[0xC000]
                 (rom on)            (banking)          (never banked, never ROM)
```

---

## 4. Capture — how each map stays truthful

Every map is a **pure projection of a peek snapshot**; none reaches into private
Device state or invents a value. The peek surfaces already exist:

| Map | Primary peek | Fields consumed |
|---|---|---|
| Raster (§1) | `video_peek` → `VideoRegs` | `mode`, `frames`, `cur_row` (= `beam_row`) |
| Raster grid classification | `crtc_peek` → `CrtcRegs` | `hcc`, `ra`, `vcc`, `ma`, `hsync`, `vsync`, `dispen`, `scanline`, `reg[18]` (R1/R6/R9) |
| Raster Plus stack | `asic_peek` → `AsicRegs` + `asic_vid_*` | `split_line`, `split_addr`, sprite attrs, 12-bit palette |
| Address shuffle (§2) | pure fn `vid_byte_addr` + `CrtcRegs.ma/ra` | no state — it *is* the formula |
| Geography (§3) | `mem_peek` → `MemRegs` + `mem_peek_cpu` | `rom_config`, `ram_config`, `ram_ext`, `rom_select` |
| Geography overlays | committed `Bus` | `cpu.romdis`, `cpu.ramdis` (who's driving) |

### 4.1 Truthfulness bar

- **`beam_col`/`beam_row` are the renderer's own cursor**, not a reconstruction.
  The map draws the exact state that decides where the next pixel lands, so it
  cannot drift from what's painted. To expose them cleanly, widen `VideoRegs`
  with `int beam_col; int first_active_row;` alongside the existing `cur_row` —
  a peek-only additive change, no new bus line.
- **The shuffle map has no state to desync** — `vid_byte_addr` is the pure
  function the fetch path itself calls (`render_char` → `vid_render_line`), so
  the map's arrows are the real read addresses by construction.
- **The geography map reads the same latches the memory Device decodes on.**
  `mem_peek_cpu(addr)` "honours the active ROM overlays and RAM banking, like a
  logic analyzer replaying an mreq cycle" — so the READ strip is literally what
  the Z80 would sample. The WRITE strip uses the write rule (always banked RAM,
  ROM-transparent). The video-fetch strip reads base RAM directly, matching the
  fetch port.
- **Overlays are shown from the committed bus, not guessed.** `/ROMDIS` and
  `/RAMDIS` are live `Bus` lines; an expansion asserting them is *observed*
  driving, exactly as the memory Device observes them at commit time (the
  one-tick latch). No layer is drawn "on" unless its gate is truly asserted this
  cycle.

### 4.2 Cadence

The Devices already latch at the natural sync boundaries — the CRTC reads the
ASIC split once per frame (`crtc_attach_asic`), the video Device re-latches
sprites + palette once per scanline (`plus_refresh_line` on `hs_fall`). The maps
inherit that cadence for free: sample peeks once per frame for a static view,
or once per scanline to animate the split/sprite-snap. Pausing mid-frame freezes
a genuinely half-drawn framebuffer — the map narrates the frontier, it doesn't
fabricate the rest.

---

## 5. DevTools integration

Three new DevTools windows (peers of the existing 17), all peek-fed and
read-only, with cross-links wired through the existing `navigate_to(addr,
NavTarget)` address bus:

1. **Raster window** — the 48×N cell grid, beam crosshair, per-cell MA/RA/RAM
   tooltip, Plus layer-stack inspector, and a "draw beam on emulated screen"
   toggle. Click a cell → `navigate_to(vid_byte_addr(...), MEMORY)`; click its
   MA → `navigate_to(ma, GFX)`.
2. **Address-shuffle window** — the bit-braid (§2.2) plus the two strips
   (screen→RAM smear, RAM→screen interleave), bidirectionally linked to the
   Memory hex window's selection.
3. **Memory-geography window** — the four banded slots (§3.1) and the stacked
   transparency overlays (§3.2), with the READ | WRITE | VIDEO-FETCH triple strip
   (§3.3–3.4). Live-updates as `ram_config`/`rom_config`/`rom_select` change; a
   layer lights only while its gate bit is set.

Because the maps are projections, they cost only a peek per frame — cheap enough
to leave open. Per the render-loop convention, cache the grid classification and
the layer decode behind a dirty flag keyed on `frames` (raster) and on
`rom_config|ram_config|rom_select|romdis|ramdis` (geography); recompute only when
those change.

---

## 6. What this reveals that a hex dump cannot

- **WHERE, not just what.** `MA=0x3051` is a number; "column 17, row 42, about to
  read RAM `0xD8A2`" is a *place*. The raster map turns the CRTC counters back
  into the geometry they were always encoding.
- **The physical layout the numbers hide.** `vid_byte_addr` is a one-line braid;
  its consequence — one glyph smeared across 16 KB, vertical neighbours 2 KB
  apart — is invisible in hex and obvious as a strip.
- **The beam as a cursor of causality.** Pause and the frontier between
  drawn and undrawn pixels *is* the program counter of the video system. You can
  point at the exact RAM byte becoming light right now, and watch a raster split
  or a mode change happen as the beam crosses the line you armed it at.
- **Read ≠ write ≠ fetch.** The same 16-bit address resolves to three different
  bytes depending on who's asking (CPU read through ROM, CPU write to banked RAM,
  GA fetch from base RAM). The geography map draws all three terrains side by
  side; a single hex column can only ever show one.
- **Overlays as depth, not flags.** `romdis`/`ramdis`/ASIC-page/MF2 stop being
  scattered bits and become a stack of translucent sheets where you can see, at a
  glance, which chip wins each band — and why your write went somewhere your read
  can't see.
