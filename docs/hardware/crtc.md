# CPC CRTC (6845 CRT Controller)

Hardware reference for the Amstrad CPC's video timing controller. This describes
the real hardware, not the emulator's implementation.

## Overview

The CRTC generates video timing signals and memory addresses. It does NOT
generate pixels, colours, or any visual content — that's the Gate Array's job.
The CRTC is purely a timing and addressing engine.

```
                  ┌───────────────────────────────┐
  Z80 CPU ────────┤           CRTC 6845           │
  &BCxx select ──→│                               │
  &BDxx write ───→│  18 programmable registers     │
  &BExx status ←──│  (R0-R17)                     │
  &BFxx read ←────│                               │
                  │  Internal counters:            │
                  │  ┌─ HCC (Horizontal Char) ───┐│───→ MA0-MA13 (memory address)
                  │  │  counts 0 to R0           ││        → Gate Array
                  │  └───────────────────────────┘│
                  │  ┌─ VCC (Vertical Char) ─────┐│───→ RA0-RA4 (row address)
                  │  │  counts 0 to R4           ││        → Gate Array
                  │  └───────────────────────────┘│
                  │  ┌─ VLC (Vertical Line) ─────┐│───→ HSYNC  → Gate Array
                  │  │  counts 0 to R9           ││───→ VSYNC  → Gate Array
                  │  └───────────────────────────┘│───→ DISPEN → Gate Array
                  │                               │───→ CURSOR → (not connected)
                  └───────────────────────────────┘
```

The CRTC was designed for text-mode CRT terminals (like the VT100). Amstrad
repurposed it as a flexible timing generator. The "character" concept in the
registers maps to groups of pixels — there is no character ROM or text mode
in the CPC's video output path.

## What the CRTC controls vs. what it doesn't

### Controls (timing and addressing)
- **Where** in RAM the display starts (R12/R13 — screen base address)
- **How many** character columns and rows are visible (R1, R6)
- **How many** scan lines per character row (R9)
- **Where** sync pulses fall (R2, R7 — positions the picture on the monitor)
- **How wide** sync pulses are (R3 — affects interrupt timing via Gate Array)
- **Fine vertical adjustment** (R5 — adds extra scan lines for non-integer frame totals)

### Does NOT control
- Screen mode (Mode 0/1/2) — Gate Array register
- Palette / ink colours — Gate Array registers
- Pixel encoding / colour depth — Gate Array hardware
- Border colour — Gate Array register
- Interrupt rate — Gate Array counts HSYNCs from the CRTC, fires every 52

The CRTC has no concept of pixels, colours, or screen modes. It outputs
addresses and sync signals. The Gate Array interprets everything else.

## I/O ports

The CRTC is selected when bit 6 of the upper address byte is LOW (active-low
chip select). This gives a 4-port decode from the upper byte bits 1-0:

| Port pattern | Function | Address range | Direction |
|-------------|----------|---------------|-----------|
| `&BCxx` | Register select | &BC00-&BCFF | Write only |
| `&BDxx` | Register write | &BD00-&BDFF | Write only |
| `&BExx` | Status register | &BE00-&BEFF | Read (type-dependent) |
| `&BFxx` | Register read | &BF00-&BFFF | Read (type-dependent) |

The low byte of the port address is ignored — the CRTC sees only the upper
byte. So `OUT &BC00,n` and `OUT &BCFF,n` are identical.

### Selecting a register

```
OUT &BCxx, register_number    ; select register (0-17)
OUT &BDxx, value              ; write value to selected register
```

Only registers 0-15 are writable. Registers 16-17 are read-only (light pen).

## Register reference

### R0 — Horizontal Total (write-only)

**Default**: 63 (&3F)

Total number of character periods per scan line, minus 1. The horizontal
character counter (HCC) counts from 0 to R0 then resets.

One character period = 1 µs = 4 T-states at 4 MHz. So R0=63 gives 64 µs
per scan line = 15625 lines/second — the standard PAL horizontal frequency.

**Demo trick**: Changing R0 mid-frame changes line length. Combined with
R12/R13 changes, this creates "rupture" — multiple independent screen zones
with different memory start addresses.

```
Standard:   R0 = 63    → 64 characters = 64 µs per line
Shrunk:     R0 = 50    → 51 characters = 51 µs per line (faster than monitor)
Stretched:  R0 = 79    → 80 characters = 80 µs per line (slower than monitor)
```

### R1 — Horizontal Displayed (write-only)

**Default**: 40

Number of character columns displayed per line. The CRTC asserts DISPEN
(display enable) for the first R1 character periods, then de-asserts it.
While DISPEN is LOW, the Gate Array outputs border colour.

```
Standard:   R1 = 40    → 40 characters = 80 bytes = 320/640 pixels
Overscan:   R1 = 48    → 48 characters = 96 bytes (fills entire line)
Narrow:     R1 = 32    → 32 characters with wide borders
```

Each "character" is 2 bytes of video RAM. In Mode 1 (4 pixels/byte), R1=40
gives 40 × 8 = 320 pixels. In Mode 0, 40 × 4 = 160 pixels (but wider).

### R2 — Horizontal Sync Position (write-only)

**Default**: 46 (&2E)

Character position at which HSYNC begins. Moving R2 shifts the picture
left or right on the monitor:

```
R2 = 42    → picture shifts right (HSYNC earlier, picture starts later)
R2 = 50    → picture shifts left (HSYNC later, picture starts earlier)
```

The Gate Array uses HSYNC to generate interrupts. Every 52 HSYNCs, an
interrupt fires. Changing R2 position can affect interrupt timing subtly.

### R3 — Sync Widths (write-only)

**Default**: &8E (HSYNC=14, VSYNC=8)

Two fields packed into one byte:

| Bits | Field | Meaning |
|------|-------|---------|
| 3-0 | HSYNC width | Number of character periods (0-15) |
| 7-4 | VSYNC width | Number of scan lines (0-15, 0 = 16) |

**HSYNC width** affects interrupt timing. The Gate Array counts HSYNC edges.
On Types 0/3, HSYNC width 0 means NO HSYNC — no interrupts fire.
On Types 2/3, HSYNC width 0 means 16 characters wide.

**VSYNC width** is programmable on Types 0/3. On Types 1/2, VSYNC is always
16 lines regardless of R3 bits 7-4.

**Demo trick**: Programmers change HSYNC width to shift the interrupt counter,
creating custom interrupt timing for raster effects.

### R4 — Vertical Total (write-only)

**Default**: 38

Total number of character rows per frame, minus 1. Combined with R9 (scan
lines per row) and R5 (fine adjust), this determines the frame rate:

```
Frame lines = (R4 + 1) × (R9 + 1) + R5
Standard:   (38 + 1) × (8) + 0 = 312 lines = 50 Hz PAL
```

Only bits 6-0 are used (max 127).

### R5 — Vertical Total Adjust (write-only)

**Default**: 0

Extra scan lines added after the last character row to fine-tune the frame
total. Bits 4-0 (max 31).

```
Example: Need 312.5 lines for perfect 50 Hz PAL interlace:
  (38 + 1) × 8 = 312 lines
  R5 = 0 → 312.0 lines (close enough for non-interlaced)
  R5 = 6 → 318 lines (slower frame rate, picture rolls down)
```

### R6 — Vertical Displayed (write-only)

**Default**: 25

Number of character rows displayed. DISPEN goes LOW after R6 rows, showing
border colour for the remaining rows until VSYNC.

```
Standard:   R6 = 25    → 25 rows × 8 lines = 200 visible scan lines
Overscan:   R6 = 34    → 34 rows × 8 lines = 272 visible scan lines
Short:      R6 = 20    → 20 rows = 160 lines with large top/bottom borders
```

**Demo trick**: Full overscan uses R6=34 with R1=48 to fill the entire
visible screen area. This removes all borders but requires careful memory
layout since the screen wraps within the 16K page.

### R7 — Vertical Sync Position (write-only)

**Default**: 30

Character row at which VSYNC begins. Shifts the picture up or down on the
monitor:

```
R7 = 28    → picture shifts down
R7 = 33    → picture shifts up
```

The Gate Array resets its HSYNC counter to 2 when it detects VSYNC — this
synchronises interrupts to the frame. The 2-HSYNC delay means the first
interrupt fires 2 HSYNCs after VSYNC start.

### R8 — Interlace and Skew (write-only)

**Default**: 0

| Bits | Field | Types | Meaning |
|------|-------|-------|---------|
| 1-0 | Interlace mode | All | See below |
| 5-4 | Display skew | 0, 3 | DISPEN delay in characters |

#### Interlace modes

| Value | Mode | Effect |
|-------|------|--------|
| 0 | Non-interlaced | Normal CPC display — same content every field |
| 1 | Interlace sync | Fields alternate (odd/even), but display address doesn't change. Produces visible flicker — used by some demos for pseudo-hi-res effects |
| 2 | Reserved | Undefined behaviour (varies by CRTC type) |
| 3 | Interlace sync + video | True interlace: odd fields show even scan lines, even fields show odd. Effectively doubles vertical resolution. Requires careful R9 setup |

The CPC firmware always sets R8=0 (non-interlaced). The Gate Array wasn't
designed for interlace — visual artifacts appear on most monitors. But some
demos use mode 1 to create flickering between two images at 25 Hz each,
giving the illusion of more colours or higher resolution.

In interlace sync+video mode (3), the CRTC adds an extra half-line to odd
fields, and the row address (RA) advances differently on odd vs even fields.
This is the mode used by CRT terminals for 80×50 text displays.

#### Display skew (Types 0 and 3 only)

| Skew value | Effect |
|-----------|--------|
| 0 | No delay (default) |
| 1 | Display data delayed by 1 character |
| 2 | Display data delayed by 2 characters |
| 3 | Display permanently disabled |

Types 1 and 2 ignore the skew bits entirely.

**Demo trick**: Skew=3 disables all pixel output while keeping HSYNC/VSYNC
active. Combined with mid-frame register changes, this can blank specific
screen zones.

### R9 — Maximum Raster Address (write-only)

**Default**: 7

Number of scan lines per character row, minus 1. The raster line counter
(VLC) counts from 0 to R9 then resets and the character row counter (VCC)
increments.

```
Standard:   R9 = 7     → 8 scan lines per row (200 lines / 25 rows)
Demo:       R9 = 0     → 1 scan line per row (smooth vertical scrolling)
Stretched:  R9 = 15    → 16 scan lines per row (tall characters, 12.5 rows)
```

**Demo trick — smooth vertical scrolling**: Set R9=0 so each "character row"
is just 1 scan line. Now R4 controls individual scan lines and R12/R13
adjustments scroll the screen by single pixels instead of 8-pixel jumps.

**Demo trick — rupture**: Change R9 mid-frame at the exact HSYNC to create
different-height zones. One zone might have R9=7 (normal), another R9=3
(half height) for a status bar.

### R10 — Cursor Start Raster (write-only)

| Bits | Field | Meaning |
|------|-------|---------|
| 4-0 | Start line | First scan line of cursor within character cell |
| 6-5 | Blink mode | 00=steady, 01=invisible, 10=blink 1/16, 11=blink 1/32 |

Blink rates are in frame periods: 1/16 = blink every 16 frames (≈3 Hz at
50 fps), 1/32 = blink every 32 frames (≈1.5 Hz).

**On the CPC**: The CRTC computes cursor visibility internally and asserts
its CURSOR output pin when the address counter matches R14/R15 and the
raster counter is between R10 and R11. However, **the CPC does not connect
the CURSOR pin to the video output path**. The Gate Array has no input for
a cursor signal. So these registers have zero visual effect on a real CPC.

The CPC firmware draws its own software cursor by XORing pixels at the
cursor position during the VBL interrupt handler. This is entirely
independent of the CRTC cursor hardware.

If you wrote a value to R10/R11/R14/R15, the CRTC would internally track
and blink a cursor at the specified address — but nobody would see it.
The CURSOR pin would toggle on an oscilloscope, but no pixel would change
on screen.

### R11 — Cursor End Raster (write-only)

Bits 4-0: Last scan line of cursor. The cursor is displayed from R10
start line to R11 end line within each character cell.

Same caveat as R10: not connected to video output on the CPC.

### R12 — Display Start Address (High) (write/read on types 0, 3)

Bits 5-0: Upper 6 bits of the 14-bit display start address.

Combined with R13 to form the starting memory address for each frame:

```
Display start = (R12 << 8) | R13

Standard:    R12 = &30, R13 = &00  →  MA = &3000
             Gate Array maps: &C000 (page 3, offset 0)
```

On the CPC, bits 13-12 of the start address select the 16K video page:

| MA13:MA12 | Video page | RAM address range |
|-----------|-----------|-------------------|
| 00 | &0000 | &0000-&3FFF |
| 01 | &4000 | &4000-&7FFF |
| 10 | &8000 | &8000-&BFFF |
| 11 | &C000 | &C000-&FFFF (default) |

**Hardware scrolling**: Change R12/R13 each frame to smoothly scroll the
entire screen without moving any data in RAM. The CPC firmware uses this
for screen-up/screen-down operations.

**Double buffering**: Alternate R12/R13 between two video pages each frame.
Write to the hidden page while displaying the other → flicker-free animation.

**Type 1 (UM6845R) quirk**: When VCC=0 (first character row), R12/R13 are
re-read at the start of each scan line. Other types only latch the start
address once per frame.

### R13 — Display Start Address (Low) (write/read on types 0, 3)

Bits 7-0: Lower 8 bits of the 14-bit display start address.

### R14 — Cursor Address (High) (read/write)

Bits 5-0: Upper 6 bits of cursor position.

Readable on all CRTC types. The CPC firmware doesn't use this — it tracks
cursor position in RAM variables instead.

### R15 — Cursor Address (Low) (read/write)

Bits 7-0: Lower 8 bits of cursor position.

### R16 — Light Pen Address (High) (read-only)

Bits 5-0: Upper 6 bits of address latched when the light pen input triggers.

The CPC routes the light pen input to the CRTC. The Amstrad Magnum Phaser
light gun uses this. When the photodetector sees the electron beam, the
CRTC latches the current memory address into R16/R17.

### R17 — Light Pen Address (Low) (read-only)

Bits 7-0: Lower 8 bits of the light pen address.

## CRTC to RAM address mapping

The CRTC outputs 14 address bits (MA0-MA13) and 5 row address bits
(RA0-RA4). The Gate Array maps these to a 16-bit RAM address:

```
CRTC outputs:
  MA13 MA12 MA11 MA10 MA9 MA8 MA7 MA6 MA5 MA4 MA3 MA2 MA1 MA0
  RA4  RA3  RA2  RA1  RA0

Gate Array constructs RAM address:
  Bit 15 = MA13  ─┐  Page select (00=&0000, 01=&4000,
  Bit 14 = MA12  ─┘               10=&8000, 11=&C000)
  Bit 13 = RA2   ─┐
  Bit 12 = RA1    │  Scan line within character row
  Bit 11 = RA0   ─┘  (0-7 for standard 8-line rows)
  Bit 10 = MA9   ─┐
  Bit  9 = MA8    │
  Bit  8 = MA7    │
  Bit  7 = MA6    │  Character column address
  Bit  6 = MA5    │  (offset within row)
  Bit  5 = MA4    │
  Bit  4 = MA3    │
  Bit  3 = MA2    │
  Bit  2 = MA1    │
  Bit  1 = MA0   ─┘
  Bit  0 = 0/1      Even/odd byte (2 bytes per character)
```

This mapping produces the CPC's distinctive "2K stride" memory layout:

```
For default R12=&30, R13=&00, R9=7:

  Scan line 0 of row 0:  &C000-&C04F   (80 bytes, 40 characters × 2)
  Scan line 1 of row 0:  &C800-&C84F   (+&0800 = 2048 bytes apart)
  Scan line 2 of row 0:  &D000-&D04F
  Scan line 3 of row 0:  &D800-&D84F
  Scan line 4 of row 0:  &E000-&E04F
  Scan line 5 of row 0:  &E800-&E84F
  Scan line 6 of row 0:  &F000-&F04F
  Scan line 7 of row 0:  &F800-&F84F
  Scan line 0 of row 1:  &C050-&C09F   (next row starts after row 0 in same block)
  ...
```

This non-linear layout is confusing for beginners but is a direct
consequence of RA (scan line) occupying the middle bits of the address.
It means that vertically adjacent pixels are 2048 bytes apart, not 80.

### Address formula

```
RAM address = (page × &4000) + (RA × &0800) + (column × 2)

Where:
  page   = MA13:MA12 (0-3)
  RA     = raster line within row (0 to R9)
  column = MA0-MA9 (character position, 0 to R1-1)
```

## The four CRTC types

The CPC shipped with different CRTC chips depending on model and production
batch. They are register-compatible but differ in edge cases that demos and
copy protection exploit.

| Type | Chip | Manufacturer | Found in |
|------|------|-------------|----------|
| 0 | HD6845S/UM6845 | Hitachi | CPC 464, CPC 664 |
| 1 | UM6845R | UMC | CPC 6128 |
| 2 | MC6845 | Motorola | CPC 464 (some batches) |
| 3 | AMS40489 | Amstrad | 6128+ / Plus Range (integrated in ASIC) |

### Differences summary

| Feature | Type 0 | Type 1 | Type 2 | Type 3 |
|---------|--------|--------|--------|--------|
| R12/R13 readable | Yes | No (returns 0) | No (returns 0) | Yes |
| R14/R15 readable | Yes | Yes | Yes | Yes |
| Status register (&BExx) | No | Yes (bit 5 = VBLANK) | No | Reads as register |
| VSYNC width in R3 | Programmable | Fixed at 16 | Fixed at 16 | Programmable |
| HSYNC width 0 | No HSYNC | No HSYNC | 16 chars wide | 16 chars wide |
| Display skew (R8 bits 5-4) | Supported | Ignored | Ignored | Supported |
| R8 interlace bits | Full byte | Bits 1-0 only | Bits 1-0 only | Full byte |
| R12/R13 latching | Once per frame | Re-read when VCC=0 | Once per frame | Once per frame |
| Unselected register read | Returns 0 | Returns 0 (R31=&FF) | Returns 0 | Returns 0 |

### Type 0 (HD6845S) — the "reference" CRTC

Most flexible and predictable. VSYNC width is programmable via R3 bits 7-4.
Display skew works. R12/R13 are readable (games use this to detect CRTC type).

HSYNC width = 0 produces no HSYNC at all, which means no interrupts fire.
This can be used intentionally to suppress interrupts during critical code.

### Type 1 (UM6845R) — the "quirky" one

Has a status register at &BExx: bit 5 indicates vertical blanking (VCC ≥ R6).
This is the only CRTC type with a readable status register.

R12/R13 are write-only (reading returns 0). **Important for CRTC detection**:
if you write &FF to R12 and read it back as 0, you know it's Type 1.

When VCC=0 (the first character row of the frame), R12/R13 are re-latched
at the start of each scan line. On other types, the start address is latched
once per frame. This affects some rupture techniques.

### Type 2 (MC6845) — the "strict" one

Original Motorola design. No status register. No display skew. VSYNC fixed
at 16 lines. HSYNC width 0 means 16 (not zero).

R12/R13 are write-only (returns 0 on read).

### Type 3 (AMS40489) — the "ASIC" CRTC

Integrated into the Plus Range ASIC chip. Behaves like Type 0 for most
purposes but with some Plus-specific enhancements:

- Programmable raster interrupt (ASIC register, not CRTC)
- Hardware sprites overlaid on CRTC output
- Soft scroll register (shifts display by 0-15 pixels horizontally)
- DMA sound channels synchronized to scan lines

&BExx reads the selected register (same as &BFxx), unlike Types 0 and 2.

## Frame timing

### Standard PAL frame (default register values)

```
R0=63  R1=40  R2=46  R3=&8E  R4=38  R5=0  R6=25  R7=30  R9=7

Horizontal (per scan line):
  ├── 40 chars displayed ──├── 6 chars right border ──├── 14 chars HSYNC ──├── 4 chars left border ──┤
  │       R1=40            │   R2-R1=6                │    R3&0F=14       │  R0-R2-hsw+1=4         │
  └────────────────────────┴──────────────────────────┴───────────────────┴────────────────────────┘
                                                  Total: 64 characters = 64 µs

Vertical (per frame):
  ├── 25 rows displayed ──├── 5 rows bottom border ──├── 8 rows VSYNC ──├── 1 row top border ──┤
  │       R6=25           │   R7-R6=5                │   R3>>4=8        │  R4-R7-vsw+1        │
  └───────────────────────┴──────────────────────────┴──────────────────┴──────────────────────┘
                               Total: 39 rows × 8 lines = 312 scan lines
                               312 × 64 µs = 19968 µs ≈ 50.08 Hz

Visible area:
  25 rows × 8 lines = 200 scan lines
  40 chars × 2 bytes = 80 bytes per line
  200 × 80 = 16000 bytes of video data per frame
```

### Interrupt timing

The Gate Array generates interrupts based on HSYNC counting:
1. Count HSYNCs from the CRTC
2. When VSYNC starts, reset counter to 2 (not 0 — 2 HSYNC delay)
3. Every 52 HSYNCs, fire a maskable interrupt (IM 1 → RST &38)
4. If the Z80 acknowledges within 1 µs, clear pending; otherwise hold

This gives 6 interrupts per frame (312 lines / 52 = 6). The first fires
at line 2 (after VSYNC start), then at 54, 106, 158, 210, 262.

Games use `DI`/`EI` plus `HALT` to synchronize with specific scan lines
for raster effects (changing palette or mode mid-frame).

## Demo techniques

### Hardware scrolling (R12/R13)

The simplest CRTC trick. Change R12/R13 to move the display start address:

```z80
; Scroll screen down by one character row (8 scan lines)
ld bc,&BC0C          ; select R12
out (c),c
ld bc,&BD00+new_high ; write new R12 value
out (c),c
ld bc,&BC0D          ; select R13
out (c),c
ld bc,&BD00+new_low  ; write new R13 value
out (c),c
```

This shifts the entire display instantly with zero CPU time for memory
copies. The address wraps within the 16K page, so scrolling past &3FFF
wraps to &0000 (or &C000 wraps to &FFFF → &C000).

### Rupture (split screen)

Change R12/R13 and/or R9 mid-frame to create zones with different memory
sources or row heights. Requires precise timing — the write must happen
during HSYNC of the transition line.

```z80
; Wait for specific scan line using interrupt + counter
halt                  ; wait for interrupt (known scan line)
; ... count exact lines with NOPs or HALT chain ...
; At the boundary, change start address:
ld bc,&BC0C : out (c),c
ld bc,&BD30 : out (c),c  ; R12 = &30 → page &C000
ld bc,&BC0D : out (c),c
ld bc,&BD80 : out (c),c  ; R13 = &80 → offset &80 within page
```

### Overscan (borderless display)

Set R1=48 (or more) and R6=34 (or more) to extend the display area into
the border region:

```z80
; Full overscan setup
ld bc,&BC01 : out (c),c : ld bc,&BD30 : out (c),c  ; R1=48
ld bc,&BC06 : out (c),c : ld bc,&BD22 : out (c),c  ; R6=34
ld bc,&BC02 : out (c),c : ld bc,&BD32 : out (c),c  ; R2=50 (recentre)
ld bc,&BC07 : out (c),c : ld bc,&BD23 : out (c),c  ; R7=35 (recentre)
```

Screen memory wraps within the 16K page. With 96 bytes/line × 272 lines =
26112 bytes, the layout overlaps itself — careful memory management needed.

### Smooth vertical pixel scrolling

Set R9=0 so each "character row" is a single scan line. Now changing R12/R13
scrolls by one pixel instead of 8:

```z80
; Setup: 1 scan line per row
ld bc,&BC09 : out (c),c
ld bc,&BD00 : out (c),c  ; R9=0

; Vertical total must increase to compensate:
; Normal: (R4+1) × (R9+1) = 39 × 8 = 312
; New:    (R4+1) × 1 = R4+1 → need R4=311... but max is 127.
; Solution: use R5 (adjust) or accept non-standard frame rate.
```

In practice, demos combine R9=0 for a scrolling zone with R9=7 for a static
zone, using rupture to separate them.

### HSYNC width manipulation

Changing R3's lower nibble adjusts HSYNC width, which affects when the Gate
Array's interrupt counter increments. This allows shifting the interrupt
timing by fractions of a scan line — critical for raster effects that need
sub-line precision.

```z80
; Shift interrupt timing by reducing HSYNC to 4 characters
ld bc,&BC03 : out (c),c
ld bc,&BD84 : out (c),c  ; R3 = &84 (VSYNC=8, HSYNC=4)
```

## CRTC type detection

Software detects the CRTC type by probing register readback:

```z80
; Method: Write to R12, read back, check value
ld bc,&BC0C       ; select R12
out (c),c
ld bc,&BDFF       ; write &FF to R12
out (c),c
ld b,&BF          ; read back from &BFxx
in a,(c)
; Type 0/3: A = &3F (masked to 6 bits, readable)
; Type 1:   A = &00 (R12 not readable)
; Type 2:   A = &00 (R12 not readable)
; Now check status register to distinguish Type 1 from Type 2:
ld b,&BE          ; read &BExx
in a,(c)
; Type 1:   A = &00 or &20 (status register exists)
; Type 2:   A = &FF (no status register)
```

## Further reading

- **Motorola MC6845 datasheet** — Original CRTC specification
- **Hitachi HD6845S datasheet** — Type 0 reference
- **CRTC differences by Longshot (CPC-Scene)** — Definitive per-type comparison
- **Kevin Thacker's Arnoldemu docs** — CRTC internals for emulator developers
- **WinAPE CRTC documentation** — Type-specific behaviour tables
- **CPC Wiki CRTC page** — Community-maintained register reference
- **Grimware CRTC documentation** — Detailed analysis including undocumented behaviour
