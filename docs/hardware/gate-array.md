# CPC Gate Array

Hardware reference for the Amstrad CPC's custom Gate Array chip. This describes
the real hardware, not the emulator's implementation.

## Overview

The Gate Array is Amstrad's custom chip — the heart of the CPC's architecture.
It sits between the CRTC, the CPU, and the RAM/ROM, performing several
critical functions that would require dozens of discrete logic chips otherwise:

1. **Pixel decoding** — Converts RAM bytes into coloured pixels
2. **Palette management** — Maps ink numbers to hardware colours
3. **Screen mode selection** — Controls resolution/colour trade-off
4. **ROM/RAM banking** — Pages ROMs in and out of the address space
5. **Interrupt generation** — Produces regular interrupts from HSYNC counting
6. **Clock generation** — Derives the 4 MHz CPU clock and 1 MHz CRTC clock

```
                    ┌──────────────────────────────────────┐
  Z80 CPU ──────────┤           Gate Array                 │
  (4 MHz clock) ←───┤                                      │
                    │   ┌─────────────┐  ┌──────────────┐  │
  CRTC ────────────→│   │ Pixel       │  │ Interrupt    │  │
   MA0-13   ───────→│   │ Decoder     │──│ Generator    │  │
   RA0-2    ───────→│   │             │  │ (52 HSYNCs)  │  │
   HSYNC    ───────→│   │ M0: 2 px/b  │  └──────────────┘  │
   VSYNC    ───────→│   │ M1: 4 px/b  │                    │──→ RGB video out
   DISPEN   ───────→│   │ M2: 8 px/b  │  ┌──────────────┐  │     (active/border/
                    │   └─────────────┘  │ Palette      │  │      black during sync)
  RAM data bus ────→│                    │ 16 inks +    │  │
  (8 bits)          │   ┌─────────────┐  │ 1 border     │  │
                    │   │ Memory      │  │ = 32 colours │  │
  ROM low   ←───────│   │ Manager     │  └──────────────┘  │
  ROM high  ←───────│   │ (ROM/RAM    │                    │
                    │   │  banking)   │                    │
                    │   └─────────────┘                    │
                    └──────────────────────────────────────┘
```

### Chip variants

| Machine | Chip | Package | Notes |
|---------|------|---------|-------|
| CPC 464 | 40007 | 40-pin DIP | Original Gate Array |
| CPC 664 | 40008 | 40-pin DIP | Minor fixes |
| CPC 6128 | 40010 | 40-pin DIP | Most common |
| 6128 Plus | 40489 (ASIC) | 100-pin QFP | Gate Array + CRTC + DMA + sprites integrated |

All behave identically from the software perspective (except the Plus ASIC,
which adds new features on top).

## What the Gate Array controls vs. what it doesn't

### Controls
- **Screen mode** (Mode 0/1/2) — resolution and colour depth
- **Palette** — maps 16 ink numbers + border to 32 hardware colours
- **ROM paging** — enables/disables lower ROM, upper ROM, expansion ROMs
- **Interrupt generation** — regular 300 Hz interrupts (every 52 HSYNCs)
- **Pixel serialisation** — reads RAM bytes, outputs coloured pixels
- **Border colour** — displayed outside the active area
- **Clock generation** — 16 MHz master → 4 MHz CPU, 1 MHz CRTC

### Does NOT control
- **Where** the display starts in RAM → CRTC registers R12/R13
- **How many** characters are visible → CRTC registers R1, R6
- **Where** sync pulses fall → CRTC registers R2, R7
- **Scan lines per row** → CRTC register R9
- **Frame timing** → CRTC registers R0, R4, R5
- **Sound** → PSG AY-3-8912 (entirely separate)
- **Keyboard** → PPI 8255

## I/O Port

The Gate Array is selected when **bit 7 of the upper address byte is 0 and
bit 6 is 1**: `(port.hi & 0xC0) == 0x40`. The standard port is `&7Fxx`.

The data byte's **bits 7-6** select one of four command types:

| Bits 7-6 | Command | Function |
|----------|---------|----------|
| `00` | Select pen | Choose which ink to modify next |
| `01` | Set colour | Assign a hardware colour to the selected pen |
| `10` | Set mode / ROM config | Screen mode + ROM paging + interrupt control |
| `11` | (unused by GA) | See RAM banking below |

**Important:** The Gate Array is **write-only**. There is no way to read back
the current mode, palette, or ROM configuration. Software must track state
itself (or rely on the firmware's copy in RAM).

### Memory configuration port

RAM banking uses a **different** chip select: bit 7 of the upper address byte
must be **0** (`!(port.hi & 0x80)`) AND the data byte must have bits 7-6 = `11`.
The standard port is `&7Fxx` — which overlaps the Gate Array port. The hardware
disambiguates by data bits: `00`, `01`, `10` go to the Gate Array; `11` goes to
the PAL (memory expansion controller).

## Command 0: Select Pen (bits 7-6 = 00)

```
  Bit:   7  6  5  4  3  2  1  0
         0  0  B  x  P3 P2 P1 P0

  B = 0: Select ink pen P3-P0 (0-15)
  B = 1: Select border (pen 16)
```

After selecting a pen, the next "Set colour" command will change that pen's
colour. The pen number is latched — it persists until a new pen is selected.

### Example: Select border
```z80
ld   bc, &7F10      ; pen = 16 (border): bit 4 set
out  (c), c         ; Gate Array command
```

### Example: Select ink 3
```z80
ld   bc, &7F03      ; pen = 3
out  (c), c
```

## Command 1: Set Colour (bits 7-6 = 01)

```
  Bit:   7  6  5  4  3  2  1  0
         0  1  x  C4 C3 C2 C1 C0

  C4-C0 = hardware colour number (0-31)
```

Sets the currently selected pen (from command 0) to hardware colour C4-C0.

### Example: Set ink 1 to bright yellow (hardware colour 25)
```z80
ld   bc, &7F01      ; select pen 1
out  (c), c
ld   bc, &7F40+25   ; set colour: 0x40 | 25 = 0x59
out  (c), c
```

### The 32 hardware colours

The CPC has 27 unique colours (3 levels × 3 channels = 27), but the hardware
colour table has 32 entries. Five pairs produce identical colours:

| # | Colour | R | G | B | Note |
|---|--------|---|---|---|------|
| 0 | White | 0.5 | 0.5 | 0.5 | = #1 |
| 1 | White | 0.5 | 0.5 | 0.5 | = #0 |
| 2 | Sea Green | 0.0 | 1.0 | 0.5 | |
| 3 | Pastel Yellow | 1.0 | 1.0 | 0.5 | |
| 4 | Blue | 0.0 | 0.0 | 0.5 | = #16 |
| 5 | Purple | 1.0 | 0.0 | 0.5 | = #8 |
| 6 | Cyan | 0.0 | 0.5 | 0.5 | |
| 7 | Pink | 1.0 | 0.5 | 0.5 | |
| 8 | Purple | 1.0 | 0.0 | 0.5 | = #5 |
| 9 | Pastel Yellow | 1.0 | 1.0 | 0.5 | = #3 |
| 10 | Bright Yellow | 1.0 | 1.0 | 0.0 | |
| 11 | Bright White | 1.0 | 1.0 | 1.0 | |
| 12 | Bright Red | 1.0 | 0.0 | 0.0 | |
| 13 | Bright Magenta | 1.0 | 0.0 | 1.0 | |
| 14 | Orange | 1.0 | 0.5 | 0.0 | |
| 15 | Pastel Magenta | 1.0 | 0.5 | 1.0 | |
| 16 | Blue | 0.0 | 0.0 | 0.5 | = #4 |
| 17 | Sea Green | 0.0 | 1.0 | 0.5 | = #2 |
| 18 | Bright Green | 0.0 | 1.0 | 0.0 | |
| 19 | Bright Cyan | 0.0 | 1.0 | 1.0 | |
| 20 | Black | 0.0 | 0.0 | 0.0 | |
| 21 | Bright Blue | 0.0 | 0.0 | 1.0 | |
| 22 | Green | 0.0 | 0.5 | 0.0 | |
| 23 | Sky Blue | 0.0 | 0.5 | 1.0 | |
| 24 | Magenta | 0.5 | 0.0 | 0.5 | |
| 25 | Pastel Green | 0.5 | 1.0 | 0.5 | |
| 26 | Lime | 0.5 | 1.0 | 0.0 | |
| 27 | Pastel Cyan | 0.5 | 1.0 | 1.0 | |
| 28 | Red | 0.5 | 0.0 | 0.0 | |
| 29 | Mauve | 0.5 | 0.0 | 1.0 | |
| 30 | Yellow | 0.5 | 0.5 | 0.0 | |
| 31 | Pastel Blue | 0.5 | 0.5 | 1.0 | |

Each RGB component has 3 possible levels: 0.0, 0.5, and 1.0. These correspond
to three voltage levels on the analogue RGB output: 0V, ~2.5V, and ~5V. The
monitor (GT64/GT65/CTM644) interprets these as off, half, and full brightness.

The duplicate entries (0=1, 3=9, 4=16, 5=8, 2=17) exist because the hardware
colour number's bit layout doesn't map efficiently to 3-level RGB. The 5-bit
encoding (32 values) overshoots the 27 actual distinct colours.

### Mode 2 anti-aliasing

When inks 0 or 1 are changed, the Gate Array computes an averaged colour from
inks 0 and 1. This is stored as palette entry 33 and used in Mode 2 rendering
to anti-alias adjacent pixels of different colours, reducing the "shimmer"
effect of 1-pixel-wide detail at 640×200.

## Command 2: Set Mode / ROM Config (bits 7-6 = 10)

```
  Bit:   7  6  5  4  3  2  1  0
         1  0  x  I  UE LE M1 M0

  M1-M0 = Screen mode (0, 1, 2, 3)
  LE    = Lower ROM disable (1 = disable, 0 = enable)
  UE    = Upper ROM disable (1 = disable, 0 = enable)
  I     = Interrupt control (1 = clear pending interrupt + reset counter)
```

This command does four things at once:

1. **Sets screen mode** — but the actual mode change is deferred until the
   next HSYNC (this prevents visual glitches from mid-scanline mode changes;
   demos exploit this with "rupture" techniques to have different modes on
   different lines)

2. **Controls ROM paging** — see Memory Map section below

3. **Clears pending interrupts and resets the scanline counter** — when bit 4
   is set. This is how software synchronises with the display:
   ```z80
   ; Wait for VSYNC then reset interrupt counter
   halt                ; wait for interrupt
   di                  ; prevent further interrupts
   ld   bc, &7F10+2    ; bit 4 = reset counter, mode 2
   out  (c), c
   ei                  ; re-enable interrupts
   ```

### The three screen modes

| Mode | Resolution | Colours | Pixels/byte | Pixel width | Use |
|------|-----------|---------|-------------|-------------|-----|
| 0 | 160×200 | 16 | 2 | 4 CPC pixels | Games, graphics |
| 1 | 320×200 | 4 | 4 | 2 CPC pixels | General purpose, text |
| 2 | 640×200 | 2 | 8 | 1 CPC pixel | Text, word processing |
| 3 | 160×200 | 4 | 2 | 4 CPC pixels | Undocumented, same as Mode 0 layout but only 4 colours |

**Mode 3** is undocumented and was never used by Amstrad. It uses the same 2
pixels per byte layout as Mode 0, but only uses inks 0-3 (like Mode 1's colour
range). Some demos use it for specific effects.

## Pixel Decoding

This is where the Gate Array earns its keep. Every microsecond (1 MHz CRTC
clock), the CRTC advances its character counter, and the Gate Array fetches
two bytes from RAM (one per 0.5 µs at 2 MHz video fetch rate). Each byte is
decoded according to the current screen mode.

### The CPC's scrambled pixel bit layout

Unlike most computers of the era, the CPC does **not** store pixels in
left-to-right bit order. The bits within each byte are interleaved across
pixels in a pattern that minimises the Gate Array's logic:

#### Mode 2: 8 pixels per byte (2 colours)

Each bit is one pixel. Bit 7 = leftmost, bit 0 = rightmost:

```
  Bit:   7  6  5  4  3  2  1  0
  Pixel: 0  1  2  3  4  5  6  7
  Value: ink number (0 or 1)
```

This is the only straightforward mode — each bit directly selects ink 0 or 1.

#### Mode 1: 4 pixels per byte (4 colours)

Each pixel is 2 bits, but the bits are scattered:

```
  Bit:       7   6   5   4   3   2   1   0
  Pixel:     0   1   2   3   0   1   2   3
  Bit role: hi  hi  hi  hi  lo  lo  lo  lo

  Pixel 0 = bit7 << 1 | bit3    → ink 0-3
  Pixel 1 = bit6 << 1 | bit2    → ink 0-3
  Pixel 2 = bit5 << 1 | bit1    → ink 0-3
  Pixel 3 = bit4 << 1 | bit0    → ink 0-3
```

#### Mode 0: 2 pixels per byte (16 colours)

Each pixel is 4 bits, maximally scrambled:

```
  Bit:       7   6   5   4   3   2   1   0
  Pixel:     0   1   0   1   0   1   0   1
  Bit role: b3  b3  b1  b1  b2  b2  b0  b0

  Pixel 0 = bit7 << 3 | bit5 << 2 | bit3 << 1 | bit1    → ink 0-15
  Pixel 1 = bit6 << 3 | bit4 << 2 | bit2 << 1 | bit0    → ink 0-15
```

### Why the scrambled layout?

The Gate Array decodes pixels with a shift register. By interleaving the bit
positions, the same hardware shift-and-select logic works for all three modes
— the only difference is how many shift cycles it performs per pixel. This
saved transistors in the custom chip design, at the cost of making pixel
manipulation in software much more complex (requiring lookup tables like
`M0Map`, `M1Map`, and `M2Map` in the emulator).

### Address generation: from CRTC to RAM

The Gate Array doesn't just receive addresses from the CRTC — it **transforms**
them. The CRTC outputs MA0-MA13 (14-bit memory address) and RA0-RA2 (3-bit
row address). The Gate Array combines them into a 16-bit RAM address:

```
  CRTC output:    MA13 MA12  [MA11..MA0]        RA2  RA1  RA0

  RAM address:    MA13 MA12  RA2  RA1  RA0  MA9  MA8  MA7  MA6  MA5  MA4  MA3  MA2  MA1  MA0  0
                  ────────── ──────────────  ───────────────────────────────────────────────── ─
                  Bit 15-14  Bit 13-11       Bit 10-1                                          Bit 0

  MA11,MA10 are dropped (not connected to RAM address bus)
```

This is the **same address mapping** described in the CRTC doc, but it's the
Gate Array that implements it, not the CRTC. Key consequences:

- **MA13:12** → bits 15:14 → selects one of 4 screen pages (at &0000, &4000,
  &8000, &C000)
- **RA2:0** → bits 13:11 → creates the CPC's characteristic **2KB stride** —
  consecutive screen rows are 2048 bytes apart, not adjacent
- **MA9:0** → bits 10:1 → byte address within a 2KB band
- **Bit 0** → always 0 in the character address (the Gate Array fetches two
  bytes per character slot, toggling bit 0 internally)

The emulator implements this in `MAXlate[]`:
```c
MAXlate[l] = (j & 0x7FE) | ((j & 0x6000) << 1);
// j = l << 1 (actual byte address)
// Keeps bits 10-1 in place, shifts bits 14-13 up by 1 to make room for RA insertion
```

## ROM/RAM Memory Map

The CPC's 64KB address space is divided into four 16KB banks:

```
  &0000-&3FFF   Bank 0: RAM (or Lower ROM when paged in)
  &4000-&7FFF   Bank 1: RAM (always)
  &8000-&BFFF   Bank 2: RAM (always)
  &C000-&FFFF   Bank 3: RAM (or Upper ROM when paged in)
```

### ROM paging (Command 2, bits 2-3)

The Gate Array controls ROM visibility:

- **Bit 2 = 0** → Lower ROM enabled at &0000-&3FFF (contains BASIC for 464/664/6128,
  or cartridge page for Plus)
- **Bit 3 = 0** → Upper ROM enabled at &C000-&FFFF (contains AMSDOS firmware, or
  selected expansion ROM)
- ROM reads overlay RAM — writes always go to RAM underneath

When the Z80 reads from &0000-&3FFF with lower ROM enabled, it reads ROM.
When it writes to the same address, the write goes to RAM. This is possible
because the Gate Array controls separate read/write bus drivers.

### RAM banking (Command 3 — not Gate Array, but PAL)

The 128KB models (6128, 6128+, and expansions) add extra RAM selected by the
data byte sent with bits 7-6 = `11`:

```
  Bit:   7  6  5  4  3  2  1  0
         1  1  x  B2 B1 B0 C2 C1 C0

  B2-B0 = Bank number (which 64KB block of expansion RAM)
  C2-C0 = Configuration (which 16KB page maps where)
```

The 8 configurations:

| C2-C0 | Bank 0 | Bank 1 | Bank 2 | Bank 3 | Notes |
|-------|--------|--------|--------|--------|-------|
| 0 | RAM 0 | RAM 1 | RAM 2 | RAM 3 | Default (base 64KB) |
| 1 | RAM 0 | RAM 1 | RAM 2 | ext 3 | Expansion in bank 3 |
| 2 | ext 4 | ext 5 | ext 6 | ext 7 | Full 64KB expansion block |
| 3 | RAM 0 | ext 3 | RAM 2 | ext 7 | Mixed |
| 4 | RAM 0 | ext 4 | RAM 2 | RAM 3 | Expansion page in bank 1 |
| 5 | RAM 0 | ext 5 | RAM 2 | RAM 3 | Expansion page in bank 1 |
| 6 | RAM 0 | ext 6 | RAM 2 | RAM 3 | Expansion page in bank 1 |
| 7 | RAM 0 | ext 7 | RAM 2 | RAM 3 | Expansion page in bank 1 |

Configurations 4-7 are the most useful: they page a single 16KB block of
expansion RAM into bank 1 (&4000-&7FFF), leaving the rest of the address
space unchanged. This is how AMSDOS accesses the extra 64KB on a 6128.

### Yarek 4MB expansion

The standard RAM banking protocol uses 3 bits for bank select (B2-B0), giving
access to 8 × 64KB = 512KB. The Yarek 4MB expansion extends this by using
**inverted bits from the port address** as additional bank bits:

```
  Port address bits 5-3 (inverted) = high 3 bits of bank number
  Data bits 5-3                    = low 3 bits of bank number
  Combined: 6-bit bank number (0-63) × 64KB = up to 4MB
```

The standard port `&7Fxx` has address bits 5-3 = `111`, which inverts to `000`
— making it backward compatible with unmodified software.

## Interrupt Generation

The Gate Array generates the CPC's primary interrupt source using a simple
counter driven by the CRTC's HSYNC signal.

### The 52-HSYNC counter

```
  HSYNC falling edge
       │
       ▼
  ┌──────────┐     counter == 52?     ┌──────────────┐
  │ sl_count  │─────── yes ──────────→│ Fire INT      │
  │   += 1    │                       │ sl_count = 0  │
  └──────────┘                       └──────────────┘
       │
       │  VSYNC arrived?
       ▼  yes
  ┌──────────────────────────────────┐
  │ hs_count = 2                      │  (2-HSYNC delay)
  │ After 2 more HSYNCs:              │
  │   if sl_count >= 32: fire INT     │
  │   sl_count = 0                    │
  └──────────────────────────────────┘
```

**Normal operation:**
- Every HSYNC (at the end of the horizontal sync pulse width), `sl_count` increments
- When `sl_count` reaches 52, an interrupt is queued and the counter resets
- With a standard frame of 312 scan lines: 312/52 = 6 interrupts per frame = 300 Hz

**VSYNC synchronisation:**
- When VSYNC begins, a 2-HSYNC delay (`hs_count = 2`) is started
- After 2 HSYNCs: if `sl_count >= 32`, an interrupt fires immediately
- Either way, `sl_count` resets to 0
- This synchronises the interrupt pattern to the frame start

**Manual reset (Command 2, bit 4):**
- Writing bit 4 of command 2 clears any pending interrupt AND resets `sl_count`
  to 0
- This is how demo code achieves precise raster timing:
  ```z80
  halt              ; wait for interrupt (aligned to 52-HSYNC boundary)
  di                ; disable further interrupts
  ld bc, &7F8C      ; command 2: mode 0, disable ROMs, reset counter
  out (c), c        ; sl_count = 0, int_pending = 0
  ; Now we know exactly where we are in the frame
  ```

### Interrupt frequency

| Frame type | Scan lines | Interrupts/frame | INT frequency |
|-----------|-----------|------------------|---------------|
| Standard PAL (312 lines) | 312 | 6 | 300 Hz |
| Overscan (variable) | varies | varies | depends on R4/R9 |
| Interlace (625 lines) | 625 | ~12 | ~600 Hz |

The 6 interrupts per frame occur at scan lines 0 (VSYNC-synchronised), 52,
104, 156, 208, and 260 — though the exact positions shift slightly depending
on VSYNC timing.

## Screen Mode Details

### Mode 0: 160×200, 16 colours

The richest colour mode. Each byte encodes 2 pixels, each selecting from 16
inks. Games use this mode almost universally.

- 16 inks available (from 27 unique hardware colours)
- 2 pixels per byte → 80 bytes per scan line (standard 40-column mode)
- Pixels are wide (4 CPC pixels each) — chunky, game-friendly
- BASIC: `MODE 0` / Firmware: `SCR SET MODE 0` / Gate Array: `OUT &7F00, &8C`

### Mode 1: 320×200, 4 colours

The default mode. Balances resolution and colour for general use, text, and
most applications.

- 4 inks available
- 4 pixels per byte → 80 bytes per scan line
- Pixels are medium width (2 CPC pixels each)
- Default palette: blue (0), yellow (1), cyan (2), red (3)
- BASIC: `MODE 1` / Firmware: `SCR SET MODE 1` / Gate Array: `OUT &7F00, &8D`

### Mode 2: 640×200, 2 colours

High-resolution mode for text and word processing. Only 2 colours, but at
the highest resolution the CPC can produce.

- 2 inks available (typically ink 0 = background, ink 1 = foreground)
- 8 pixels per byte → 80 bytes per scan line
- Pixels are narrow (1 CPC pixel each)
- The Gate Array generates an "anti-aliased" palette entry (average of ink 0
  and ink 1) to reduce shimmer on 1-pixel detail
- BASIC: `MODE 2` / Firmware: `SCR SET MODE 2` / Gate Array: `OUT &7F00, &8E`

### Why always 80 bytes per line?

All three modes produce 80 bytes per scan line (in a standard 40-column
CRTC setup). This is because the CRTC counts characters, not pixels — it
outputs the same number of memory addresses regardless of mode. The Gate
Array simply decodes each byte into fewer (wider) or more (narrower) pixels
depending on the mode. The total bandwidth is always the same.

## Mode Switching and Raster Effects

Mode changes are latched: writing a new mode via command 2 doesn't take
effect immediately. The Gate Array applies the new mode at the **next HSYNC**.
This means:

1. **Mid-frame mode changes** are safe — the mode switches cleanly at a
   scanline boundary
2. **Mid-line mode changes** require careful timing — the Gate Array won't
   change until the line ends

Demos exploit this to have different modes on different scanlines:

```z80
; Split screen: Mode 1 on top, Mode 0 on bottom
  halt                        ; wait for interrupt (top of frame area)
  ld bc, &7F00+&80+1          ; command 2: mode 1
  out (c), c
  ; ... wait for the right scan line ...
  ld b, &7F
  ld c, &80+0                 ; command 2: mode 0
  out (c), c                  ; switches at next HSYNC
```

## The Gate Array and the Character ROM Question

The CPC's Gate Array fundamentally prevents the CRTC from being used in its
intended role as a text-mode controller. In a CRT terminal:

```
  RAM (character codes) → Character ROM (code + RA → dot pattern) → video
```

On the CPC:

```
  RAM (raw pixel bytes) → Gate Array (byte → coloured pixels via mode decode) → video
```

The Gate Array intercepts the CRTC's RA (raster address) output and uses it
for **RAM address generation** (bits 13:11), not for character ROM row
selection. The data bus from RAM feeds directly into the pixel decoder. There
is no insertion point for a character ROM without significant hardware
modification.

This is a deliberate design choice: Amstrad wanted a bitmap display that could
handle graphics, not just text. The firmware implements "text mode" entirely in
software by drawing bitmapped characters into the pixel buffer through the
TXT_OUTPUT routine.

## Plus Range: ASIC Enhancements

The 6128+ and CPC+ machines replace the discrete Gate Array + CRTC with a
single ASIC chip (40489) that includes all Gate Array functions plus:

- **Hardware sprites** — 16 sprites, 16×16 pixels, 16 colours each
- **Programmable raster interrupts** — fire at any scan line, not just every 52
- **DMA sound** — 3 independent DMA channels for sample playback
- **Extended palette** — 4096 colours (12-bit RGB: 4 bits per channel)
- **Soft scroll** — pixel-level smooth scrolling via dedicated registers
- **Analogue joystick** — dedicated ADC support

The ASIC is accessed through a "register page" at &4000-&7FFF, enabled by
an unlock sequence written to the CRTC register select port. The unlock
sequence is a specific 17-byte pattern that must be sent in exact order.

When the register page is active, writes to &4000-&7FFF address ASIC
registers instead of RAM:

| Range | Function |
|-------|----------|
| &4000-&43FF | Sprite pixel data |
| &6400-&6421 | Sprite attributes (x, y, magnification) |
| &6800-&6803 | Programmable raster interrupt |
| &6804-&680F | Soft scroll, split screen |
| &6C00-&6C0F | DMA channel control |
| &6D00-&6D01 | DMA interrupt control |

## Demo Techniques Using the Gate Array

### Palette flashing (colour cycling)

Change ink colours rapidly between frames for animation effects without
moving any data in RAM:

```z80
; Cycle ink 1 through red → yellow → green
frame1:
  ld bc, &7F01        ; select pen 1
  out (c), c
  ld bc, &7F40+12     ; bright red
  out (c), c
  halt                ; wait one frame
  ld bc, &7F01
  out (c), c
  ld bc, &7F40+10     ; bright yellow
  out (c), c
  halt
  ld bc, &7F01
  out (c), c
  ld bc, &7F40+18     ; bright green
  out (c), c
  halt
  jr frame1
```

### Raster bars (per-scanline colour changes)

Change the border or ink colour at precise points during the frame:

```z80
; Rainbow border effect
  di
  ld bc, &7F10        ; select border pen
  out (c), c
  ld bc, &7F8C        ; reset interrupt counter + mode 0
  out (c), c
  ei
  halt                ; wait for top of frame

  ld b, 200           ; 200 visible lines
.loop:
  ; Each visible line: change border colour
  push bc
  ld bc, &7F40+X      ; set border to colour X (varies per line)
  out (c), c
  ; ... wait ~64µs for one scan line ...
  pop bc
  djnz .loop
```

### Split-mode screens

Different screen modes on different parts of the screen, as shown in the
Mode Switching section above. Combined with interrupt-driven timing to switch
at the correct scan line.

### Overscan with ROM disable

For full-screen overscan (filling the border area with graphics), disable
both ROMs and set the CRTC to display the full frame:

```z80
  ld bc, &7F8C        ; mode 0, both ROMs disabled (bits 2,3 = 1)
  out (c), c
```

With ROMs disabled, the entire 64KB address space is RAM — giving more room
for screen data that wraps around the address space.

## Emulator Implementation Notes

In konCePCja, the Gate Array state is held in the global `t_GateArray` struct:

```c
typedef struct {
   unsigned int hs_count;          // VSYNC delay counter
   unsigned char ROM_config;       // bits 2-3: ROM enables, bits 0-1: mode
   unsigned char lower_ROM_bank;   // which 16KB bank lower ROM occupies (Plus only)
   bool registerPageOn;            // ASIC register page mapped at &4000
   unsigned char RAM_bank;         // current expansion RAM bank
   unsigned char RAM_config;       // raw value written to RAM config port
   unsigned char RAM_ext;          // Yarek 4MB: extra bank bits from port address
   unsigned char upper_ROM;        // selected upper ROM number
   unsigned int requested_scr_mode; // mode requested (deferred until HSYNC)
   unsigned int scr_mode;          // current active screen mode
   unsigned char pen;              // currently selected pen (0-16)
   unsigned char ink_values[17];   // hardware colour for each pen (0-15 + border)
   unsigned int palette[34];       // pre-computed pixel colours for rendering
   unsigned char sl_count;         // scanline counter for interrupt generation
   unsigned char int_delay;        // frames to delay interrupt (unused in current code)
} t_GateArray;
```

Pixel decoding uses pre-computed lookup tables (`M0Map`, `M1Map`, `M2Map`,
`M3Map`) that convert a byte into a 4-dword block of palette indices — one
index per pixel position, ready for the renderer to look up in
`GateArray.palette[]`.

## Further Reading

- [Gate Array on CPCWiki](https://www.cpcwiki.eu/index.php/Gate_Array) — comprehensive hardware documentation
- [CPC I/O Port Summary](https://www.cpcwiki.eu/index.php/I/O_Port_Summary) — all I/O port decodings
- [RAM Banking](http://cpctech.cpc-live.com/docs/rampage.html) — detailed RAM config documentation
- [CRTC reference](crtc.md) — companion document for the CRTC timing controller
- [Floppy disc system](floppy-disc-system.md) — FDC and disc format documentation
