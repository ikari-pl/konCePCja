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

| Machine | Chip | Package | Process | Notes |
|---------|------|---------|---------|-------|
| CPC 464 | 40007 | 40-pin DIP | 5µm NMOS | Original Gate Array, ~7000 gates |
| CPC 664 | 40008 | 40-pin DIP | 5µm NMOS | Bug fix revision |
| CPC 6128 | 40010 | 40-pin DIP | 3µm CMOS | Most common, lower power |
| 6128 Plus | 40489 (ASIC) | 100-pin QFP | CMOS | GA + CRTC + DMA + sprites, single chip |

The 40007/40008/40010 are functionally identical from software's perspective.
The difference is manufacturing process: the NMOS 40007/40008 run hotter and
consume more power (~1.5W), while the CMOS 40010 runs cooler (~0.3W). The
CMOS version is why the 6128 needs smaller heatsinks than the 464.

All three discrete Gate Arrays have the same 40-pin DIP pinout:

```
                    ┌──────┐
            MREQ ──┤1   40├── Vcc (+5V)
             M1  ──┤2   39├── CLK (16 MHz master clock in)
         IORQ  ──┤3   38├── READY (active-low, active during video fetch)
           RD  ──┤4   37├── CCLK (1 MHz character clock → CRTC)
         A15  ──┤5   36├── PHI (4 MHz CPU clock out)
         A14  ──┤6   35├── RAS (DRAM row address strobe)
         D7   ──┤7   34├── CAS (DRAM column address strobe)
         D6   ──┤8   33├── WE (write enable)
         D5   ──┤9   32├── 244E (ROM high enable)
         D4   ──┤10  31├── 244E (ROM low enable)
         D3   ──┤11  30├── SYNC (active during H/V sync → blanks video)
         D2   ──┤12  29├── INT (active-low interrupt → Z80 INT pin)
         D1   ──┤13  28├── BLUE (analogue output, accent)
         D0   ──┤14  27├── BLUE (analogue output, base)
       HSYNC  ──┤15  26├── RED (analogue output, accent)
       VSYNC  ──┤16  25├── RED (analogue output, base)
      DISPEN  ──┤17  24├── GREEN (analogue output, accent)
    RESET  ──┤18  23├── GREEN (analogue output, base)
       CASAD  ──┤19  22├── CAS-ADDRESS (active during column address phase)
         Vss  ──┤20  21├── READY (active-high complement)
                    └──────┘
```

Notable pins:
- **CLK (pin 39):** The 16 MHz master clock input (from a crystal oscillator
  on the PCB). The Gate Array divides this by 4 to produce the Z80's 4 MHz
  clock (PHI, pin 36) and by 16 to produce the CRTC's 1 MHz character clock
  (CCLK, pin 37).
- **READY (pin 38):** Directly controls the Z80's READY/WAIT line. The Gate
  Array asserts this during video fetch cycles, halting the CPU to prevent bus
  contention. This is how the CPC handles the shared bus between CPU and video
  without dedicated video RAM.
- **RGB outputs (pins 23-28):** Six analogue outputs, two per colour channel
  (base + accent). Each channel has 3 voltage levels: base only (level 1),
  accent only (level 2), both (level 3). This gives 3 levels per channel ×
  3 channels = 27 colours.
- **INT (pin 29):** Active-low interrupt output connected directly to the
  Z80's INT pin. The Gate Array generates this from its 52-HSYNC counter.

### Clock generation

The Gate Array is the CPC's master timekeeper. A 16 MHz crystal oscillator
feeds pin 39, and the Gate Array derives all system clocks:

```
  16 MHz (crystal)
    │
    ├── ÷4 ──→ 4 MHz   → Z80 CPU clock (PHI)
    │            │
    │            └──→ also used internally for pixel output timing
    │                  (8 pixels per µs in Mode 2 = 8 MHz pixel rate,
    │                   doubled from 4 MHz by serialising 2 pixels
    │                   from each fetched byte)
    │
    ├── ÷16 ─→ 1 MHz   → CRTC character clock (CCLK)
    │                      One character = 2 bytes = 16 pixels (Mode 0)
    │                                             = 8 pixels (Mode 1)
    │                                             = 16 pixels (Mode 2)
    │
    └── ÷2 ──→ 8 MHz   → DRAM access timing (RAS/CAS generation)
                           Two memory accesses per character clock:
                           one for the Gate Array (video fetch),
                           one for the Z80 (CPU access)
```

The interleaved bus access is the key to the CPC's shared-memory architecture.
Every 1 µs (one character clock period), the Gate Array performs two DRAM
accesses: one for itself (to fetch video data) and one on behalf of the CPU.
This is invisible to the Z80 — it simply runs at 4 MHz, unaware that its bus
accesses are being interleaved with video fetches.

**However:** When the Z80 tries to access memory during the Gate Array's fetch
phase, the READY line is deasserted, inserting a wait state. This is why the
effective speed of memory-accessing Z80 instructions on the CPC is slightly
lower than on a "pure" 4 MHz Z80 — the CPU is occasionally stalled by a
fraction of a clock cycle. For instructions that don't access memory (register
operations), there is no penalty.

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

This is a deliberate cost-saving measure: a read-back path would require
additional tri-state buffers and register multiplexing logic, adding
transistors to the custom chip. Since the firmware can keep a software shadow
copy in RAM (at addresses &B7C4-&B7CB), Amstrad considered read-back
unnecessary.

**Consequence for demos:** Demo code that takes over from the firmware and
reprograms the palette must maintain its own shadow state. There is no way to
"read the current ink 3 colour" from hardware — you either remember what you
wrote, or you don't know.

**The firmware's shadow copies:**
| Address | Contents |
|---------|----------|
| &B7C4 | Current pen number |
| &B7C5-&B7D4 | Ink values for pens 0-15 |
| &B7D5 | Border ink value |
| &B7D6 | Screen mode |
| &B7D7 | ROM configuration |

### I/O port decoding details

The CPC uses partial address decoding — not all 16 address bits are checked.
The Gate Array responds to **any** port where bits 15-14 of the upper address
byte are `01`. This means many port addresses alias to the same register:

```
  &40xx, &41xx, &42xx, ..., &7Exx, &7Fxx — ALL select the Gate Array

  The canonical port is &7Fxx because OUT (C),r sends B as the upper byte,
  and loading BC = &7Fxx gives B=&7F (bit 7=0, bit 6=1).
```

**Why &7Fxx specifically?** The `OUT (C),r` instruction puts B on the upper
address bus and C on the lower. By loading BC with `&7Fnn`, we get B=&7F
(which selects the Gate Array via bits 7-6=01) and C=nn (the command byte).
Since `OUT (C),C` is a common idiom (one instruction to send both the port
and data), the entire operation fits in just `LD BC,&7Fnn` + `OUT (C),C`.

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
to three voltage levels on the analogue RGB output.

### How the Gate Array generates analogue RGB

Each colour channel (R, G, B) has **two output pins** — a "base" and an
"accent" line. Each pin drives a resistor to the monitor input:

```
                                        ┌── 1KΩ ── base pin  (0 or 5V)
  Monitor RGB input ←── summing node ──┤
                                        └── 2KΩ ── accent pin (0 or 5V)
```

With two binary signals (on/off) per channel, each combined through a
resistor divider, you get three distinct voltage levels:

| Base | Accent | Output voltage | Colour level |
|------|--------|---------------|--------------|
| 0V | 0V | 0V | Off (0.0) |
| 0V | 5V | ~1.7V | Half (0.5) |
| 5V | 0V | ~3.3V | Full... wait, this doesn't happen |
| 5V | 5V | ~5V | Full (1.0) |

Actually the mapping isn't 1:1 with "base on, accent off" — the Gate Array's
internal logic ensures only three of the four combinations are used per
channel. The 5-bit hardware colour number encodes the state of all 6 output
pins (2 per channel × 3 channels), but not all 64 possible combinations
are valid. The Gate Array's internal decoder maps the 5-bit number to the
correct pin pattern.

The duplicate entries (0=1, 3=9, 4=16, 5=8, 2=17) arise because multiple
5-bit numbers can map to the same set of pin states. With 32 possible
numbers but only 27 valid colour combinations (3 levels × 3 channels), 5
numbers must be duplicates.

### Colour number to RGB mapping

The relationship between the 5-bit hardware colour number and the actual
RGB levels is **not** a simple binary encoding. It's determined by the Gate
Array's internal decoder logic. The firmware provides a lookup table at
&BD32 that maps hardware colour numbers to a human-readable order, but
there's no algebraic formula — it's an arbitrary mapping chosen during
chip design.

**Practical consequence:** To set a specific colour, you need the lookup
table. You can't compute "bright green" from first principles — you just
have to know it's hardware colour 18.

### The three monitor types

| Monitor | Model | Input | Colour rendering |
|---------|-------|-------|-----------------|
| CTM 640 | Colour | Analogue RGB | Full 27 colours |
| GT 64 | Green | Analogue RGB → green phosphor | 27 shades of green |
| GT 65 | Green | Same as GT 64 | Same, later revision |
| MP-1/MP-2 | Modulator | Composite PAL | ~16 usable colours (PAL artifacts) |

The green monitors are **not** monochrome in the traditional sense — they
receive the full RGB signal but only the green phosphor is present. Red
and blue contribute nothing visible. This means colours with the same
green component are indistinguishable (e.g., blue and black both appear
as "dark"). The emulator offers a "green screen" mode that simulates this
by converting all colours to green luminance values.

### Mode 2 anti-aliasing

Mode 2 has a unique problem: at 640 pixels across a PAL display, each pixel
is only ~1 µs / 8 = 125 ns wide. On a CRT monitor, the electron beam's spot
size is wider than a single pixel, which means adjacent ink-0 and ink-1 pixels
physically overlap. The result is visible "shimmer" or "fuzz" where colours
bleed into each other — particularly distracting for text, which is Mode 2's
primary use case.

The Gate Array mitigates this with a hardware **anti-aliasing colour**. When
either ink 0 or ink 1 is changed, the Gate Array automatically computes the
average of both colours:

```
  anti-alias.R = (ink0.R + ink1.R) / 2
  anti-alias.G = (ink0.G + ink1.G) / 2
  anti-alias.B = (ink0.B + ink1.B) / 2
```

This averaged colour is stored as palette entry 33 (an internal entry not
directly accessible via pen selection). When rendering Mode 2, the Gate Array
uses this entry at pixel transitions — where a 0-pixel meets a 1-pixel — to
smooth the boundary.

**Example:** With ink 0 = black (0,0,0) and ink 1 = bright white (1,1,1), the
anti-alias colour is (0.5, 0.5, 0.5) — a mid-grey that softens the harsh
black/white edges of text characters.

**Practical effect:** This is why Mode 2 text looks noticeably smoother on a
real CPC than you'd expect from a 2-colour 640×200 display. The anti-aliasing
is a subtle but effective touch — especially considering it's implemented in
pure hardware with zero CPU cost.

**Quirk:** The anti-alias colour only updates when pen 0 or pen 1 is written
via command 1. If you set ink 0, then ink 1, the anti-alias reflects both.
But if you only change ink 2 (which Mode 2 doesn't use), the anti-alias
colour stays unchanged. The firmware always sets inks 0 and 1 when entering
Mode 2, so this is only noticeable in demo code that manipulates the palette
directly.

In the emulator, this is implemented at `kon_cpc_ja.cpp:636`:
```c
if (GateArray.pen < 2) {
   byte r = (colours[GateArray.ink_values[0]].r + colours[GateArray.ink_values[1]].r) >> 1;
   byte g = (colours[GateArray.ink_values[0]].g + colours[GateArray.ink_values[1]].g) >> 1;
   byte b = (colours[GateArray.ink_values[0]].b + colours[GateArray.ink_values[1]].b) >> 1;
   GateArray.palette[33] = MapRGBSurface(back_surface, r, g, b);
}
```

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

### Worked example: decoding byte &A7 in each mode

Let's trace a concrete byte through all three decoders. The byte `&A7` =
`10100111` in binary:

```
  Bit:    7   6   5   4   3   2   1   0
  Value:  1   0   1   0   0   1   1   1
```

**Mode 2** (8 pixels, 1 bit each):
```
  Pixel:  0   1   2   3   4   5   6   7
  Ink:    1   0   1   0   0   1   1   1
  Result: ■ · ■ · · ■ ■ ■   (■ = ink 1, · = ink 0)
```

**Mode 1** (4 pixels, 2 bits each):
```
  Pixel 0: bit7 << 1 | bit3 = 1 << 1 | 0 = 2    → ink 2
  Pixel 1: bit6 << 1 | bit2 = 0 << 1 | 1 = 1    → ink 1
  Pixel 2: bit5 << 1 | bit1 = 1 << 1 | 1 = 3    → ink 3
  Pixel 3: bit4 << 1 | bit0 = 0 << 1 | 1 = 1    → ink 1
  Result: 2 1 3 1
```

**Mode 0** (2 pixels, 4 bits each):
```
  Pixel 0: bit7<<3 | bit5<<2 | bit3<<1 | bit1 = 1<<3 | 1<<2 | 0<<1 | 1 = 13  → ink 13
  Pixel 1: bit6<<3 | bit4<<2 | bit2<<1 | bit0 = 0<<3 | 0<<2 | 1<<1 | 1 = 3   → ink 3
  Result: 13 3
```

This shows why Mode 0 pixel manipulation in software is painful: to set pixel
0 to ink 5 (`0101` in binary), you need to write bits 7=0, 5=1, 3=0, 1=1 —
scattered across the byte. Most games use lookup tables for this.

### Why the scrambled layout?

The Gate Array decodes pixels with an 8-bit shift register. The byte from RAM
is loaded into the register, then shifted one bit at a time. The key insight
is that **the same shift register logic handles all three modes**:

```
  Mode 2: Shift out 1 bit → 1-bit ink number → look up palette → output pixel
           Repeat 8 times per byte

  Mode 1: Shift out 1 bit → this is the HIGH bit of a 2-bit ink number
           Shift out 3 more bits → the bit 4 positions later is the LOW bit
           (But actually: accumulate positions 7,3 or 6,2 or 5,1 or 4,0)

  Mode 0: Same shift register, but gather 4 bits from positions 7,5,3,1 (pixel 0)
           or 6,4,2,0 (pixel 1)
```

The scrambled bit layout means the Gate Array can use a single shift register
with simple combinatorial logic to extract pixel values for any mode. In
Mode 2, each shift produces one complete pixel. In Mode 1, every other shift
contributes to the same pixel. In Mode 0, every fourth position contributes
to the same pixel. The physical wiring stays the same — only the selection
logic changes.

This approach saved perhaps 100-200 transistors compared to separate decode
logic per mode. At the 5µm NMOS process of the 40007, that's a meaningful
area saving on a chip with ~7000 total gates.

**The cost for programmers:** Every CPC game and demo that manipulates screen
pixels needs lookup tables or complex bit-twiddling. Common patterns:

```z80
; Mode 0: Set left pixel of byte to ink N
; Requires a 16-entry table: ink_to_left_pixel[16]
; ink_to_left_pixel[5] = %00001010 (bits 7=0, 5=1, 3=0, 1=0 for ink 5...
;                         actually ink 5 = 0101, so bit7=0,bit5=1,bit3=0,bit1=1)
ld a, (hl)              ; get current byte
and %01010101           ; mask out left pixel (keep right pixel bits)
or (ix+INK_TABLE)       ; OR in the pre-computed left pixel pattern
ld (hl), a              ; write back
```

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

The CPC 464 has exactly 64KB of RAM and no way to extend it through the Gate
Array. The 6128 introduced a **separate PAL chip** (a Programmable Array Logic
device) that handles memory expansion banking. This is NOT part of the Gate
Array itself, but shares the same I/O port range, which causes frequent
confusion.

The PAL responds to data bytes with bits 7-6 = `11`, sent to any port where
bit 7 of the upper address byte is 0:

```
  Bit:   7  6  5  4  3  2  1  0
         1  1  x  B2 B1 B0 C2 C1 C0

  B2-B0 = Bank number (which 64KB block of expansion RAM)
  C2-C0 = Configuration (which 16KB page maps where)
```

#### The 8 configurations

| C2-C0 | &0000-&3FFF | &4000-&7FFF | &8000-&BFFF | &C000-&FFFF | Notes |
|-------|-------------|-------------|-------------|-------------|-------|
| 0 | RAM 0 | RAM 1 | RAM 2 | RAM 3 | Default (base 64KB) |
| 1 | RAM 0 | RAM 1 | RAM 2 | ext 3 | Expansion page replaces bank 3 |
| 2 | ext 4 | ext 5 | ext 6 | ext 7 | Full 64KB expansion block |
| 3 | RAM 0 | ext 3 | RAM 2 | ext 7 | Two expansion pages |
| 4 | RAM 0 | ext 4 | RAM 2 | RAM 3 | Expansion page in bank 1 |
| 5 | RAM 0 | ext 5 | RAM 2 | RAM 3 | Expansion page in bank 1 |
| 6 | RAM 0 | ext 6 | RAM 2 | RAM 3 | Expansion page in bank 1 |
| 7 | RAM 0 | ext 7 | RAM 2 | RAM 3 | Expansion page in bank 1 |

**Naming convention:** "RAM 0-3" are the four 16KB pages of the base 64KB.
"ext 3-7" are 16KB pages from the expansion bank (selected by B2-B0). A
single expansion bank contains pages ext 4, ext 5, ext 6, ext 7 (64KB), plus
ext 3 which is always page 3 of that bank.

#### Why configs 4-7 are the most important

Configurations 4-7 all do the same thing: map a single 16KB page from the
expansion bank into **bank 1** (&4000-&7FFF), leaving everything else
unchanged. This is the pattern AMSDOS, CP/M, and most applications use:

```
  Standard memory map:                 After OUT &7FC4:
  ┌──────────────────┐  &FFFF         ┌──────────────────┐  &FFFF
  │ RAM 3 / Upper ROM│                │ RAM 3 / Upper ROM│
  ├──────────────────┤  &C000         ├──────────────────┤  &C000
  │ RAM 2 (screen)   │                │ RAM 2 (screen)   │
  ├──────────────────┤  &8000         ├──────────────────┤  &8000
  │ RAM 1            │  ◄── normal    │ ext 4            │  ◄── expansion page
  ├──────────────────┤  &4000         ├──────────────────┤  &4000
  │ RAM 0 / Lower ROM│                │ RAM 0 / Lower ROM│
  └──────────────────┘  &0000         └──────────────────┘  &0000
```

This design means:
- **Bank 0** (&0000-&3FFF) is always base RAM — the system stack, firmware
  workspace, and interrupt vectors are never disturbed
- **Bank 2** (&8000-&BFFF) is always base RAM — the default screen memory
  location stays stable
- **Bank 3** (&C000-&FFFF) stays as base RAM — BASIC programs and firmware
  variables are preserved
- Only **bank 1** (&4000-&7FFF) swaps — the "data window" for copying between
  base and expansion memory

**Typical use pattern (copy 16KB from expansion to base):**
```z80
  ld bc, &7FC4          ; config 4: map ext page 4 into bank 1
  out (c), c
  ld hl, &4000          ; source: expansion page (now visible at &4000)
  ld de, &8000          ; destination: somewhere in base RAM
  ld bc, &4000          ; 16KB
  ldir                  ; copy
  ld bc, &7FC0          ; config 0: restore normal mapping
  out (c), c
```

#### Config 2: the "full swap"

Configuration 2 maps the entire 64KB expansion bank into all four slots.
This is rarely used because it hides the base RAM entirely — including the
stack, interrupt vectors, and firmware workspace. The Z80 must be carefully
set up first (stack in ROM space, interrupts disabled) or the system will
crash immediately. CP/M's bank-switched mode uses this for running
transient programs in a full 64KB TPA (Transient Program Area).

#### Config 1 and 3: the "screen swaps"

Configs 1 and 3 map expansion pages into bank 3 (&C000-&FFFF). This is
useful for double-buffered screen displays when the screen is relocated to
&C000 via CRTC R12/R13:

```z80
  ; Set up screen at &C000
  ld bc, &BC0C           ; CRTC register 12
  out (c), c
  ld bc, &BD30           ; value &30 → screen at &C000
  out (c), c

  ; Now swap between base and expansion screens
  ld bc, &7FC0           ; config 0: screen shows base RAM page 3
  out (c), c             ; ... draw to expansion page while this is displayed ...
  ld bc, &7FC1           ; config 1: screen shows expansion page 3
  out (c), c             ; ... draw to base RAM page while this is displayed ...
```

#### The display reads *whatever is mapped*

A critical subtlety: the Gate Array reads screen data from whatever RAM is
currently paged in. It doesn't have a separate "video memory" path. So if you
page expansion RAM into the region the CRTC is scanning, the display
immediately shows the expansion RAM's contents. This is both powerful (instant
screen swapping for double-buffering) and dangerous (paging in uninitialised
memory creates garbage on screen).

### Yarek 4MB expansion

The standard RAM banking protocol uses 3 bits for bank select (B2-B0), giving
access to 8 × 64KB = 512KB. The Yarek 4MB expansion extends this by using
**inverted bits from the port address** as additional bank bits:

```
  Standard port:  &7Fxx  = 0111 1111 xxxx xxxx
                            ─┬──
                  bits 5-3:  111  → inverted = 000  (bank high bits = 0)

  Extended port:  &7Dxx  = 0111 1101 xxxx xxxx
                            ─┬──
                  bits 5-3:  110  → inverted = 001  (bank high bits = 1)
```

The 6-bit bank number is assembled as:

```
  Bank = (inverted_port_bits_5_3 << 3) | data_bits_5_3

  Port &7Fxx, data bits 5-3 = 010: bank = 000'010 = 2
  Port &7Dxx, data bits 5-3 = 010: bank = 001'010 = 10
  Port &61xx, data bits 5-3 = 111: bank = 111'111 = 63
```

This gives 64 banks × 64KB = **4096 KB = 4MB** total addressable RAM.

The backward compatibility trick: the standard port `&7Fxx` has address bits
5-3 = `111`, which inverts to `000`. So `bank = 000'xxx` — the same as the
original 3-bit protocol. Unmodified software using `&7Fxx` sees banks 0-7
exactly as before.

**Example: Accessing bank 42, config 5:**
```z80
  ; Bank 42 = 101'010 in binary
  ; High 3 bits = 101, inverted = 010 → port bits 5-3 = 010 → port &7F10 area
  ; Low 3 bits = 010 → data bits 5-3 = 010
  ; Config 5 → bits 2-0 = 101
  ; Data byte = 11'010'101 = &D5
  ld bc, &6BD5          ; port &6Bxx (bits 5-3 = 010+base), data = &D5
  out (c), c
```

The emulator handles this at `kon_cpc_ja.cpp:707`:
```c
GateArray.RAM_ext = (~port.b.h >> 3) & 7;  // extract inverted bits 5-3
```

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

### DISPEN and the border

The CRTC outputs a signal called **DISPEN** (display enable) that tells the
Gate Array whether the beam is in the active display area or the border.
The Gate Array uses this to decide what to output:

```
  DISPEN = 1 (active area):
    Gate Array fetches bytes from RAM and decodes pixels.
    Output = ink colours from the decoded pixel data.

  DISPEN = 0 (border area):
    Gate Array ignores RAM data.
    Output = border colour (pen 16's ink value).

  During HSYNC or VSYNC:
    Output = black (all RGB outputs at 0V).
    This creates the physical blanking needed by the CRT.
```

The border is **not** stored in RAM. It's generated purely by the Gate Array
hardware — an infinite plane of the border colour, covering everything
outside the CRTC's active display rectangle. This is why the border fills
the entire screen instantly when you change its colour, and why it costs zero
RAM.

**DISPEN timing is controlled by the CRTC**, not the Gate Array. The CRTC
asserts DISPEN when the horizontal character counter is less than R1 (display
width) AND the vertical character counter is less than R6 (display height).
When the CRTC is manipulated for overscan, DISPEN stays high for longer,
reducing the border area. When the CRTC is manipulated to show less (small
display), the border area expands.

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

### How CRT terminals use the CRTC

The 6845 CRTC was designed by Hitachi for **text-mode CRT terminals** like
the DEC VT100, the IBM PC's MDA/CGA adapter, and the BBC Micro's teletext
mode. In these systems, the CRTC sits at the centre of a character-generation
pipeline:

```
  ┌─────────┐     MA0-MA13     ┌──────────┐   char code    ┌──────────────┐
  │  CRTC   │────────────────→│ Video RAM │──────────────→│ Character    │
  │ 6845    │                  │ (stores   │               │ Generator   │
  │         │     RA0-RA3      │  ASCII    │   RA0-RA3     │ ROM         │
  │         │─────────┬───────│  codes)   │──────────────→│ (8×8 or     │
  └─────────┘         │        └──────────┘               │  8×16 font) │
                      │                                    └──────┬───────┘
                      │                                           │ 8-bit dot pattern
                      │                                           ▼
                      │                                    ┌──────────────┐
                      │                                    │ Shift        │
                      │                                    │ Register     │───→ Video out
                      │                                    └──────────────┘
                      │
                      └─→ RA selects which row of the character glyph
                           e.g., RA=0 → top row, RA=7 → bottom row
```

The flow:
1. CRTC generates MA (character position in video RAM)
2. Video RAM returns the **character code** (e.g., ASCII 65 = 'A')
3. The character code + RA (raster address, which line of the glyph) address
   into a character generator ROM
4. The ROM outputs 8 bits — the dot pattern for that row of that character
5. A shift register serialises the 8 bits into the video signal

In this architecture:
- **RAM stores codes, not pixels** — a full 80×25 screen is only 2000 bytes
- **RA selects the glyph row** — the CRTC scans each character row R9+1
  times, incrementing RA each time, to build up the full character height
- **The character ROM holds the font** — typically 256 characters × 8 rows =
  2KB ROM

### How the CPC breaks this pipeline

The CPC's Gate Array sits where the character ROM would be, but it does
something completely different:

```
  ┌─────────┐     MA0-MA13     ┌──────────────────────────────────────┐
  │  CRTC   │────────────────→│             Gate Array                │
  │ 6845    │                  │                                      │
  │         │     RA0-RA2      │  ┌─────────────────────────────────┐  │
  │         │────────────────→│  │ Address translator:              │  │
  └─────────┘                  │  │   RAM addr = MA13:12 | RA2:0 |  │  │
                               │  │             MA9:0 | 0          │  │
                               │  └────────────────┬────────────────┘  │
                               │                   │ 16-bit address    │
                               │                   ▼                   │
                               │  ┌──────────┐                        │
                               │  │   RAM     │── 8-bit data bus ──→  │
                               │  │ (stores   │                    ┌──┤
                               │  │  PIXELS,  │                    │P │
                               │  │  not      │                    │i │──→ RGB
                               │  │  codes)   │                    │x │    video
                               │  └──────────┘                    │e │    out
                               │                                   │l │
                               │                                   │  │
                               │                                   └──┤
                               └──────────────────────────────────────┘
```

The critical differences:

1. **RA is consumed by address generation, not character ROM.** The Gate Array
   uses RA2:0 as address bits 13:11, creating the 2KB stride. The RA bits
   don't go anywhere near a character lookup — they're just part of the RAM
   address. This is the fundamental architectural barrier.

2. **RAM data goes directly to pixel decoding.** The byte read from RAM is
   immediately split into pixels by the mode-dependent decoder (2, 4, or 8
   pixels per byte). There's no stage where the byte could be interpreted as
   a character code.

3. **There is no character ROM socket.** The PCB has no provision for a font
   ROM. Unlike the BBC Micro (which has a character ROM socket alongside its
   SAA5050 teletext chip) or the IBM PC MDA (which has a character generator
   ROM), the CPC's video path has no such component.

### Could you add a character ROM anyway?

In theory, you could build an expansion board that:

1. **Intercepts the RAM data bus** before it reaches the Gate Array
2. **Uses the RAM data as a character code** to address into a ROM
3. **Uses RA from the CRTC** as the glyph row select
4. **Feeds the ROM's dot-pattern output** back to the Gate Array's data input

But this faces several practical problems:

- **Timing:** The Gate Array reads RAM at 2 MHz (one byte per 500 ns). An
  interposed ROM lookup adds propagation delay. Unless the ROM is very fast
  (< 50 ns access time), it would violate the timing requirements.

- **RA wiring:** The CRTC's RA outputs go directly to the Gate Array. They're
  not readily available on the expansion bus. You'd need to tap them from the
  CRTC chip's pins directly — a delicate solder job.

- **Mode incompatibility:** The Gate Array's pixel decoder would still apply.
  A dot pattern of `0b10110100` from the character ROM would be decoded as
  Mode 0 (2 pixels), Mode 1 (4 pixels), or Mode 2 (8 pixels), not as 8
  monochrome dots. Only Mode 2 would produce the correct result (each bit =
  one pixel), and even then, the bit scrambling means the dots wouldn't
  appear in the expected order.

- **No attribute bytes:** CRT terminals typically use a second byte per
  character position for attributes (colour, bold, blink, underline). The
  CPC's address generation has no provision for interleaved attribute bytes.

The bottom line: it's not just difficult, it's architecturally antagonistic.
The entire video path is designed around raw pixel bytes, not character codes.

### How the CPC actually does "text"

The firmware implements text mode entirely in software:

1. **TXT_OUTPUT (&BB5A):** The firmware entry point for printing a character.
   Register A contains the character code.

2. **Character ROM in firmware:** The CPC's lower ROM (BASIC ROM) contains a
   built-in font — 256 characters × 8 bytes each = 2KB of bitmapped glyphs.
   This is ordinary ROM data, not a hardware character generator.

3. **Software rendering:** TXT_OUTPUT reads the 8-byte glyph from ROM,
   converts each byte to the correct pixel format for the current mode, and
   writes the result to screen RAM:
   - Mode 2: direct copy (1 bit = 1 pixel, matching the glyph format)
   - Mode 1: each bit expanded to 2 pixel bits (doubling width)
   - Mode 0: each bit expanded to 4 pixel bits (quadrupling width)

4. **Cursor, scrolling, windows:** All handled in software by the firmware's
   text VDU driver.

The cost: printing a character takes ~100-200 µs of CPU time (vs. zero CPU
time with a hardware character ROM). This is why CPC text output is
noticeably slower than on machines with hardware text modes (the IBM PC's
MDA could fill a screen of text with zero CPU intervention beyond writing
character codes to video RAM). But the benefit is absolute flexibility —
every pixel is individually addressable, enabling the CPC's rich graphics
capabilities.

### Historical context

Amstrad's choice was deliberate and commercially astute. In 1984, competing
8-bit micros (ZX Spectrum, Commodore 64, BBC Micro) all used some form of
bitmap display, and users expected graphical capabilities. A text-only
display like the IBM PC's MDA would have been commercially unacceptable in
the home computer market. The Gate Array's bitmap approach let Amstrad
deliver a machine that could run graphical games *and* business software
(in Mode 2) on the same hardware — just with different software.

## Plus Range: ASIC Enhancements

The 6128+ and CPC+ machines (1990) replace the discrete Gate Array + CRTC
with a single ASIC chip (40489). This ASIC integrates **all** functions of
the original Gate Array, the CRTC, and adds significant new capabilities.
From the old CPC's perspective, the ASIC is backward-compatible — all
existing software runs unmodified.

### The unlock sequence

The ASIC's extended features are hidden behind a **lock mechanism**. On power-up,
the ASIC behaves exactly like an old Gate Array + CRTC. To access Plus
features, software must send a specific 17-byte unlock sequence to the CRTC
register select port (&BCxx):

```z80
; ASIC unlock sequence (17 bytes)
asic_unlock:
  ld bc, &BC00            ; CRTC register select port
  ld hl, unlock_data
  ld d, 17
.loop:
  ld a, (hl)
  out (c), a              ; send each byte to &BCxx
  inc hl
  dec d
  jr nz, .loop
  ret

unlock_data:
  db &FF, &00, &FF, &77, &B3, &51, &A8, &D4
  db &62, &39, &9C, &46, &2B, &15, &8A, &CD
  db &EE
```

The sequence is a pseudo-random pattern that the ASIC checks byte-by-byte.
If any byte is wrong, the state machine resets. Sending &FF (or any value
with bit 7 set) at any point resets the sequence, which is why the sequence
starts with &FF — it acts as a "clear" to ensure a clean start.

**Why the lock?** Amstrad wanted backward compatibility. If the Plus features
were always accessible, old software might accidentally write to ASIC
registers (since &4000-&7FFF is just RAM on old CPCs). The lock ensures only
software specifically designed for the Plus can access the new hardware.

### Register page

Once unlocked, the ASIC's register page can be mapped into the Z80 address
space at &4000-&7FFF via a special RMR2 (ROM Mapping Register 2) command:

```z80
  ld bc, &7FA0+8          ; command 2 with bit 5 set (RMR2): map register page
  out (c), c              ; &4000-&7FFF now addresses ASIC registers
```

Reads and writes to &4000-&7FFF now access ASIC registers instead of RAM.
The full register map:

| Range | Size | Function |
|-------|------|----------|
| &4000-&43FF | 1024 bytes | Sprite pixel data (16 sprites × 16×16 pixels × 4 bits) |
| &4400-&4FFF | — | (unused, reads as &00) |
| &6400-&6421 | 34 bytes | Sprite attributes (X, Y, magnification per sprite) |
| &6422-&67FF | — | (unused) |
| &6800-&6801 | 2 bytes | Programmable raster interrupt (PRI) scan line |
| &6802-&6803 | 2 bytes | Split screen scan line + address |
| &6804 | 1 byte | Soft scroll control (X/Y pixel offset) |
| &6805-&680F | — | (reserved) |
| &6C00-&6C0F | 16 bytes | DMA channel 0/1/2 address and prescaler |
| &6C10-&6C1F | 16 bytes | DMA sound control |

### Hardware sprites

16 independent sprites, each 16×16 pixels with 16 colours from a dedicated
sprite palette. Sprites are rendered in hardware during the display scan —
no CPU time required.

Each sprite has:
```
  Pixel data:   16×16 pixels, 4 bits each = 128 bytes
                Stored at &4000 + (sprite_number × 256)
                Pixel value 0 = transparent

  Attributes (at &6400 + sprite_number × 8):
    X position:   16-bit signed (-256 to +767)
    Y position:   16-bit signed (-256 to +767)
    Magnification: 2 bits (1×, 2×, 4×, 8× — same for both axes)
```

Sprites are drawn in priority order (sprite 0 = highest, sprite 15 = lowest).
Transparent pixels (value 0) show the background underneath. Sprite-to-sprite
collision detection is NOT provided in hardware — the CPU must check positions
manually.

**Sprite palette:** Sprites use a separate 16-colour palette defined in the
ASIC registers, independent of the main screen palette. Each entry is 12-bit
RGB (4 bits per channel = 4096 possible colours), a massive upgrade from the
main screen's 27 colours.

### Extended palette

The Plus ASIC extends the colour depth from the Gate Array's 27 colours (3
levels × 3 channels, 1.5 bits per channel) to **4096 colours** (4 bits per
channel × 3 channels = 12-bit RGB):

```
  Old Gate Array:  3 levels/channel → 27 colours  (analogue output)
  Plus ASIC:       16 levels/channel → 4096 colours (12-bit digital → DAC)
```

The extended palette registers map each ink to a 12-bit value:
```
  Ink N palette register (16-bit, at &6400 + N × 2):
    Bit 11-8: Green (0-15)
    Bit 7-4:  Red (0-15)
    Bit 3-0:  Blue (0-15)
```

This means Plus software can use smooth gradients, realistic shading, and
richer colour in all modes — still limited to 16/4/2 simultaneous inks by
the mode, but chosen from 4096 instead of 27.

### Programmable Raster Interrupt (PRI)

The old Gate Array fires interrupts on a rigid 52-HSYNC grid. The Plus ASIC
adds a **programmable raster interrupt** that fires at any specific scan line:

```
  PRI register (&6800-&6801): 16-bit scan line number

  When the CRTC's vertical counter reaches this value, an interrupt fires.
  This is independent of (and in addition to) the regular 52-HSYNC interrupts.
```

This eliminates the need for busy-wait loops to time raster effects. A demo
that needs a palette change at line 150 simply programs PRI = 150, and the
interrupt handler executes the palette change with hardware precision.

### DMA sound

Three independent DMA channels that play samples from RAM with no CPU
intervention. Each channel has:

- A start address in RAM (16-bit)
- A prescaler (playback speed control)
- A loop flag
- Volume control (independent per channel)

The DMA engine fetches sample bytes from RAM and feeds them to the PSG's
DAC channels, bypassing the PSG's tone/noise generators. This enables
digitised sound playback, PCM music, and speech synthesis at quality far
beyond the old PSG's square waves.

**DMA + sprites + extended palette** together made the Plus range a
significant upgrade for games — hardware-accelerated sprite handling, richer
colours, and digitised sound, all within the CPC's existing architecture.

## Demo Techniques Using the Gate Array

The Gate Array is at the centre of almost every CPC demo effect. Unlike the
CRTC (which handles timing tricks), the Gate Array controls what you
actually *see* — colours, modes, and memory mapping. Here are the major
categories of Gate Array tricks, from simple to advanced.

### Palette animation (colour cycling)

The simplest and most common trick. Because palette changes are instant and
free (no memory moves), you can animate colours without touching screen RAM.
This costs only 4 OUTs per colour change (select pen + set colour).

**Use cases:**
- Waterfall/lava animations in games (redraw nothing, just cycle inks)
- Flashing text or sprites (swap between visible and background colour)
- Fade-in/fade-out by stepping through colour levels
- Loading screens with animated gradients

```z80
; Cycle ink 1 through red → yellow → green (3-frame loop)
frame1:
  ld bc, &7F01        ; select pen 1
  out (c), c
  ld bc, &7F40+12     ; bright red (hw colour 12)
  out (c), c
  halt                ; wait one frame (20ms at 50Hz)
  ld bc, &7F01
  out (c), c
  ld bc, &7F40+10     ; bright yellow (hw colour 10)
  out (c), c
  halt
  ld bc, &7F01
  out (c), c
  ld bc, &7F40+18     ; bright green (hw colour 18)
  out (c), c
  halt
  jr frame1
```

**Fade to black** — step all inks through decreasing brightness levels:
```z80
; Fade ink 0 from bright white to black in 3 steps
; bright white (11) → white (0) → black (20)
  ld bc, &7F00          ; pen 0
  out (c), c
  ld bc, &7F40+11       ; bright white
  out (c), c
  ; ... wait some frames ...
  ld bc, &7F00
  out (c), c
  ld bc, &7F40+0        ; half white
  out (c), c
  ; ... wait some frames ...
  ld bc, &7F00
  out (c), c
  ld bc, &7F40+20       ; black
  out (c), c
```

The CPC's 3-level RGB means fading has only 3 steps per channel. Games
typically cross-fade by cycling through intermediate colours:
bright → half → off. For smoother fading, demos rapidly alternate between
two adjacent levels (temporal dithering at 50 Hz).

### Raster bars (per-scanline colour changes)

Change ink or border colours at precise points *during* the frame, creating
horizontal colour bands that are impossible in the static palette.

**The key insight:** An OUT to the Gate Array takes effect immediately — the
very next pixel output by the Gate Array uses the new colour. If you time the
OUT to happen during the horizontal blanking period (between scan lines), you
get a clean colour change. If you time it mid-line, you get a vertical
colour split within a single scanline.

**Timing requirement:** Each scan line is 64 µs = 256 T-states at 4 MHz.
The visible portion is ~40 µs (160 T-states). The HSYNC + borders take the
remaining ~24 µs. To change colours between lines, the code must execute its
OUTs within the blanking period.

```z80
; Rainbow border: each line gets a different border colour
; This creates a continuous gradient down the screen
  di
  ld bc, &7F10          ; select border pen (once, persists)
  out (c), c
  ld bc, &7F8C          ; reset counter + mode 0
  out (c), c
  ei
  halt                  ; sync to frame start

  ld hl, colour_table   ; table of 200 hardware colour values
  ld d, 200             ; 200 visible lines
.line_loop:
  ld a, (hl)            ; get colour for this line
  or &40                ; set colour command (bits 7-6 = 01)
  ld b, &7F             ; Gate Array port
  out (c), a            ; change border colour — takes effect NOW
  inc hl

  ; Waste exactly enough T-states to fill one 64µs scan line
  ; 64µs = 256 T-states. The OUT+INC+DEC+JP above use ~30.
  ; Need ~226 T-states of delay:
  ld b, 28              ; DJNZ loop: 28 iterations × 8 T-states = 224
.delay:
  djnz .delay           ; busy-wait

  dec d
  jp nz, .line_loop
```

**Border rasters** are simpler than ink rasters because they don't affect
screen content. Ink rasters change the colours of the actual pixels, which
means any screen data rendered in that ink instantly changes colour across
the affected lines — producing the classic "plasma" or "copper bar" effects.

### Split-mode screens

Different screen modes on different parts of the display. The Gate Array
defers mode changes to the next HSYNC, so writing a mode change at any point
during a scan line takes effect cleanly at the line boundary.

**Common patterns:**
- Mode 1 text panel on top, Mode 0 graphics below (strategy games)
- Mode 2 high-res status bar, Mode 0 game area (adventure games)
- Different modes on alternating groups of lines (artistic effects)

```z80
; Split screen: Mode 2 status bar (8 lines), Mode 0 game area (192 lines)
; Assumes interrupt-driven timing is set up
  halt                        ; sync to top of frame
  di
  ld bc, &7F8E                ; command 2: mode 2, ROMs off, counter reset
  out (c), c
  ei

  ; Wait for 8 character rows (8 × 8 = 64 scan lines)
  ; Each interrupt is 52 lines, so wait ~1 interrupt + fine-tune
  halt                        ; first interrupt (52 lines into frame)
  ; We need 64 - 52 = 12 more lines = 12 × 64µs = 768µs = 3072 T-states
  ld b, 192                   ; 192 × 16 T-states = 3072
.wait_split:
  djnz .wait_split

  ld bc, &7F8C                ; command 2: mode 0, ROMs off
  out (c), c                  ; mode 0 takes effect at next HSYNC
```

**Advanced: mid-line mode split.** The Gate Array nominally defers mode
changes to HSYNC, but the exact moment depends on the CRTC's internal
counter reaching the HSYNC width (R3). Some demos exploit CRTC timing
manipulation to force mode changes at sub-character boundaries, creating
vertical mode splits within a single line. This is extremely timing-sensitive
and CRTC-type dependent.

### Overscan with ROM disable

A standard CPC screen at &C000 uses 16KB (&C000-&FFFF). But with CRTC
manipulation (increasing R1 and R6), the display can cover the entire visible
area of the monitor — "overscan" mode. This requires more than 16KB of screen
RAM.

The problem: with ROMs paged in, parts of the address space contain ROM
instead of RAM, creating holes in the display. The solution is to disable
both ROMs:

```z80
  ld bc, &7F8C          ; mode 0, both ROMs disabled (bits 2+3 = 1)
  out (c), c            ; now &0000-&3FFF and &C000-&FFFF read as RAM
```

With ROMs disabled, the entire 64KB address space is contiguous RAM. The
CRTC wraps addresses through the Gate Array's address translation, so the
display seamlessly crosses the &FFFF → &0000 boundary, creating a single
continuous screen buffer.

**Full overscan layout** (the classic "32KB screen"):
```
  &0000 ┌────────────────────────┐
        │ Bottom portion of      │  (wraps from &FFFF+1)
        │ overscan screen        │
  &3FFF └────────────────────────┘
  &4000 ┌────────────────────────┐
        │ Free RAM               │  (not displayed in standard overscan)
  &7FFF └────────────────────────┘
  &8000 ┌────────────────────────┐
        │ Top portion of         │
        │ overscan screen        │
  &FFFF └────────────────────────┘
```

The screen start address (CRTC R12/R13) is set to the middle of RAM, and
the display wraps around. The exact layout depends on R12/R13 and the
display dimensions set in R1/R6.

**Consequence for firmware:** With ROMs disabled, all firmware routines are
inaccessible (they live in ROM). The program must handle everything itself —
interrupts, keyboard scanning, and any I/O. This is why overscan is mainly
a demo technique; games prefer the standard 16KB screen.

### Palette-based hidden screens

A clever trick that uses the palette to hide pre-drawn screens:

1. Draw screen A in inks 0-3 (Mode 1)
2. Draw screen B in inks 4-7, overlapping the same pixels
3. Set inks 0-3 to visible colours, inks 4-7 to the background colour
   → Screen A is visible
4. Swap: set inks 0-3 to background, inks 4-7 to visible colours
   → Screen B appears instantly, zero memory moves

This gives instant screen switching (4 palette OUTs vs copying 16KB of RAM),
at the cost of using half the available inks for each "layer".

### Interrupt-synchronised effects (raster timing)

The Gate Array's interrupt counter is the foundation of all frame-synchronised
effects. The standard approach:

1. **HALT** — wait for the next interrupt (fires every 52 scan lines)
2. **DI + reset counter** — disable further interrupts, reset sl_count to 0
3. **Count exact T-states** — each scan line is 256 T-states (64 µs × 4 MHz)
4. **Execute effects** — palette changes, mode switches, CRTC register writes
5. **EI + HALT** — wait for next interrupt, repeat

```z80
; Framework for raster-timed effects
main_loop:
  halt                    ; sync to frame (first interrupt after VSYNC)
  di
  ld bc, &7F8C            ; reset counter
  out (c), c

  ; We're now at a known position in the frame.
  ; Each HALT advances exactly 52 scan lines = 52 × 64µs.

  ; Wait for line 100: 100 lines × 256 T-states = 25600 T-states
  ; That's ~1 interrupt (52 lines) + 48 lines
  ei
  halt                    ; fires at line 52
  di

  ; Fine-wait 48 lines = 48 × 256 = 12288 T-states
  ld b, 48
.wait48:
  ; Each iteration: 256 T-states (one full scan line)
  push hl                 ; 11
  pop hl                  ; 10
  push hl                 ; 11
  pop hl                  ; 10
  ; ... pad to exactly 256 T-states with NOPs ...
  djnz .wait48            ; 13/8

  ; Now at line 100 — do our raster effect
  ld bc, &7F40+12         ; change border to red
  out (c), c

  ei
  jr main_loop
```

**The 52-line grid:** Because interrupts fire every 52 lines, most demo
effects are structured around this 52-line rhythm. Effects that need
finer positioning use busy-wait loops between interrupts. The VSYNC sync
ensures interrupt #0 is always at the same position relative to the
visible screen.

## Emulator Implementation Notes

### State representation

In konCePCja, the Gate Array state is held in the global `t_GateArray` struct
(defined in `koncepcja.h:358`):

```c
typedef struct {
   unsigned int hs_count;          // VSYNC delay counter (counts down from 2)
   unsigned char ROM_config;       // raw command 2 byte: bits 2-3 ROM, bits 0-1 mode
   unsigned char lower_ROM_bank;   // which 16KB slot lower ROM occupies (Plus: 0-3)
   bool registerPageOn;            // ASIC register page mapped at &4000 (Plus only)
   unsigned char RAM_bank;         // active expansion RAM bank number
   unsigned char RAM_config;       // raw byte written to RAM config port
   unsigned char RAM_ext;          // Yarek 4MB: high 3 bits of bank from port address
   unsigned char upper_ROM;        // selected upper/expansion ROM number (0-252)
   unsigned int requested_scr_mode; // mode latched by command 2 (applied at HSYNC)
   unsigned int scr_mode;          // active screen mode (updated by change_mode())
   unsigned char pen;              // currently selected pen (0-16, where 16 = border)
   unsigned char ink_values[17];   // hardware colour number for each pen
   unsigned int palette[34];       // pre-computed SDL pixel values for rendering
   unsigned char sl_count;         // scanline counter for interrupt generation (0-51)
   unsigned char int_delay;        // interrupt delay counter
} t_GateArray;
```

**Key subtlety:** `requested_scr_mode` vs `scr_mode` models the real hardware's
deferred mode change. When software writes command 2, only `requested_scr_mode`
updates. The `change_mode()` function (in `crtc.cpp:513`) copies it to
`scr_mode` — but only at the HSYNC boundary:

```c
inline void change_mode() {
   if (CRTC.flag_hadhsync) {
      CRTC.flag_hadhsync = 0;
      GateArray.scr_mode = GateArray.requested_scr_mode;
      ModeMap = ModeMaps[GateArray.scr_mode];  // switch decode table
   }
}
```

### Pixel decode lookup tables

The emulator avoids per-pixel bit manipulation by using pre-computed lookup
tables that convert an entire byte into palette indices in one operation.
There are 4 tables (one per mode), each with 512 entries:

```c
dword M0Map[0x200];   // Mode 0: 512 entries, each = 4 dwords (2 pixels × 2 bytes)
dword M1Map[0x200];   // Mode 1: 512 entries (4 pixels × 2 bytes)
dword M2Map[0x200];   // Mode 2: 512 entries (8 pixels × 2 bytes)
dword M3Map[0x200];   // Mode 3: same layout as M0 but 4-colour decode
```

**How it works:** The rendering inner loop fetches two bytes from video RAM
per character position. For each byte, it uses the byte value as an index
into the current `ModeMap`:

```c
// Simplified rendering (actual code is more complex with render buffers)
dword *map_entry = &ModeMap[byte_value * 2];
// map_entry[0] = first half of pixels from this byte
// map_entry[1] = second half of pixels from this byte
```

Each entry contains **palette indices** (not actual colours). The renderer
then looks up `GateArray.palette[index]` to get the final SDL pixel value.
This two-stage lookup (byte → palette index → pixel colour) means palette
changes take effect immediately without rebuilding the decode tables.

**Why 0x200 entries instead of 256?** Each table entry actually covers a pair
of adjacent pixels, and the indexing incorporates the byte value plus some
additional state. The half-pixel variants (`M0hMap`, `M1hMap`, etc.) are used
when `dwXScale == 1` (the display is at native CRTC resolution without
horizontal doubling).

### Memory banking implementation

The `ga_memory_manager()` function (in `kon_cpc_ja.cpp:366`) is the single
point where all memory mapping decisions are made. It maintains two arrays:

```c
byte *membank_read[4];   // what the Z80 reads from each 16KB slot
byte *membank_write[4];  // what the Z80 writes to each 16KB slot
```

This split read/write mapping is how ROM overlay works: `membank_read[0]`
can point to ROM while `membank_write[0]` points to RAM underneath. The Z80
memory access functions check the appropriate array:

```c
// Simplified:
byte z80_read_mem(word addr) {
   return membank_read[addr >> 14][addr & 0x3FFF];
}
void z80_write_mem(word addr, byte val) {
   membank_write[addr >> 14][addr & 0x3FFF] = val;
}
```

The function handles 8 memory configurations via a pre-computed table
(`membank_config[8][4]`), ROM paging (lower ROM, upper ROM, Multiface II
ROM, cartridge pages), the ASIC register page (Plus), and Silicon Disc
bank ownership.

### Interrupt generation implementation

The interrupt counter logic lives in `crtc.cpp:673` (inside `match_hsw()`):

```c
// At each HSYNC completion:
GateArray.sl_count++;
if (GateArray.sl_count == 52) {
   if (CRTC.interrupt_sl == 0) {
      z80.int_pending = 1;     // queue interrupt for Z80
   }
   GateArray.sl_count = 0;
}

// VSYNC synchronisation:
if (GateArray.hs_count) {       // counting down from VSYNC start?
   GateArray.hs_count--;
   if (!GateArray.hs_count) {   // reached zero?
      if (GateArray.sl_count >= 32 && CRTC.interrupt_sl == 0) {
         z80.int_pending = 1;   // fire early interrupt
      }
      GateArray.sl_count = 0;   // reset counter
   }
}
```

The `CRTC.interrupt_sl` field supports the Plus ASIC's programmable raster
interrupt — when non-zero, the regular 52-HSYNC interrupt is suppressed
in favour of the programmed scan line.

### I/O dispatch

The Gate Array command handler is in `z80_OUT_handler()` at
`kon_cpc_ja.cpp:611`. The four command types are dispatched by a
`switch (val >> 6)` on the data byte. The memory configuration handler
is separate (at line 697) because it has a different chip select condition.

The Plus-specific RMR2 register (for lower ROM bank selection and ASIC
register page) is handled within case 2 of the switch, guarded by
`if (!asic.locked && (val & 0x20))`. The ASIC unlock state machine
(`asic_poke_lock_sequence()`) is triggered by writes to the CRTC register
select port, not the Gate Array port.

## CPU/Video Bus Contention

The CPC uses a **shared memory bus** — both the Z80 CPU and the Gate Array's
video fetch circuit need to access the same DRAM chips. Unlike machines with
dedicated video RAM (like the Amiga with its separate chip RAM DMA), the CPC
solves this with time-division multiplexing controlled by the Gate Array.

### The interleaved access scheme

The Gate Array divides each 1 µs character clock period into two phases:

```
  1 µs (one character period = 16 master clock cycles at 16 MHz)
  ┌───────────────────────────────────────────────────────┐
  │       Phase 1 (500 ns)      │      Phase 2 (500 ns)    │
  │    Gate Array video fetch   │     Z80 CPU access        │
  │                             │                           │
  │  RAS → CAS → data latched   │  RAS → CAS → data to Z80  │
  └─────────────────────────────┴───────────────────────────┘
```

The Gate Array controls the DRAM's RAS (Row Address Strobe) and CAS (Column
Address Strobe) lines. During phase 1, it fetches a byte for video display.
During phase 2, it allows the Z80 to perform its memory access.

### Wait state insertion

The Z80 runs at 4 MHz, meaning each T-state is 250 ns. A typical memory read
takes 3 T-states (750 ns). If the Z80's memory access phase doesn't align
perfectly with the Gate Array's phase-2 window, the Gate Array deasserts the
READY line, inserting a **wait state** that stretches the Z80's T-state until
the next phase-2 slot is available.

In practice:
- **NOP** (4 T-states, no memory access beyond fetch): runs at full speed
- **LD A,(HL)** (7 T-states, 2 memory accesses): may have 1 wait state inserted
- **LDIR** (21/16 T-states, 5 memory accesses): multiple potential wait states

This is why cycle-exact timing on the CPC is tricky: the actual execution
time of an instruction depends on when it happens relative to the Gate Array's
access phase. Demo coders must account for this when building cycle-exact
raster effects.

### Comparison with other architectures

| Machine | Video memory scheme | CPU impact |
|---------|-------------------|-----------|
| ZX Spectrum | Shared bus, CPU halted during border display | CPU slowed during display area ("contended memory") |
| CPC | Shared bus, interleaved access | Occasional wait states, generally transparent |
| C64 | VIC-II steals bus for badlines | CPU halted for 40-43 cycles per badline |
| BBC Micro | Shared bus, 2 MHz CPU / 2 MHz video | Exact 50/50 split, no contention |
| Amiga | Separate chip RAM bus | No CPU/video contention (for chip RAM) |

The CPC's approach is among the most transparent — the CPU generally runs
unimpeded, with only occasional single-cycle stalls. This is part of why the
CPC achieves good real-world performance despite its 4 MHz Z80 being no
faster on paper than the Spectrum's 3.5 MHz Z80.

## Further Reading

- [Gate Array on CPCWiki](https://www.cpcwiki.eu/index.php/Gate_Array) — comprehensive hardware documentation
- [CPC I/O Port Summary](https://www.cpcwiki.eu/index.php/I/O_Port_Summary) — all I/O port decodings
- [RAM Banking](http://cpctech.cpc-live.com/docs/rampage.html) — detailed RAM config documentation
- [CRTC reference](crtc.md) — companion document for the CRTC timing controller
- [Floppy disc system](floppy-disc-system.md) — FDC and disc format documentation
