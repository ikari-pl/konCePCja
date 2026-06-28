#import "../template.typ": *
#set-chapter("Emulated Hardware")

= Emulated Hardware

#intro[
  konCePCja emulates the complete Amstrad CPC: the Z80 processor and every custom
  chip around it. You do not need any of this to play games, but the developer
  tools (Chapter 7) expose all of it, and understanding the machine helps when
  you write or debug CPC code. This chapter covers both the system as a whole ---
  its memory, screen modes, and I/O --- and the individual chips.
]

== The processor

#idx("Z80")At the centre is a Zilog Z80A running at 4 MHz. konCePCja implements
the full documented and undocumented instruction set, including the shadow
register bank and the interrupt modes. The developer tools can breakpoint, step,
and disassemble it (Chapter 7).

== The memory map

#idx("memory map")The Z80 sees a 64 KB address space, divided into four 16 KB
banks. Two of those banks can show ROM instead of RAM:

#figure(image("../images/memory-map.svg", width: 78%), caption: [The 64 KB address space: four 16 KB banks, with the Lower and Upper ROMs overlaying RAM])

#idx("ROM paging")ROM is an #emph[overlay]: when a ROM is paged in, reads from
its range return ROM, but writes still go to the RAM underneath. This is how the
firmware lives at #port[\&0000] and #port[\&C000] while the program's variables
occupy the same addresses in RAM.

#idx("RAM banking")The 6128's extra 64 KB (and the up-to-4 MB Yarek-style
expansion) is reached by #emph[bank switching]: a control register pages 16 KB
blocks of the extra RAM into the #port[\&4000]--#port[\&7FFF] window (and others),
so the 8-bit Z80 can use far more than 64 KB.

== Screen modes

#idx("screen mode")The CPC has three screen modes, each trading colour for
resolution. All three use 80 bytes per scan line:

#table(
  columns: (auto, auto, auto, 1fr), stroke: 0.4pt + rule-grey, inset: 5pt,
  [*Mode*], [*Resolution*], [*Colours*], [*Typical use*],
  [0], [160 × 200], [16 inks], [Games --- chunky, colourful pixels],
  [1], [320 × 200], [4 inks], [The default --- text and general use],
  [2], [640 × 200], [2 inks], [High-resolution text and word processing],
)

#figure(image("../images/screen-modes.svg", width: 92%), caption: [The three modes trade colour for resolution; pixels widen from mode 2 to mode 0])

Switch modes from BASIC with #cmd[MODE 0], #cmd[MODE 1], or #cmd[MODE 2]. A mode
can even be changed partway down the screen for split-screen effects.

== Colour and the palette

#idx("palette")The CPC can show 27 distinct hardware colours. Each mode draws
from a palette of #emph[inks] --- 16 in mode 0, 4 in mode 1, 2 in mode 2 --- and
any ink can be assigned any of the 27 colours, plus a separate border colour.
Changing an ink updates every pixel drawn in that ink at once, which is the basis
of many colour-cycling effects. The Plus machines widen this to 4096 colours.

#figure(image("../images/palette.svg", width: 95%), caption: [The 27 hardware colours, with their firmware ink numbers])

== I/O ports

#idx("I/O ports")The custom chips are reached through Z80 `OUT` and `IN`
instructions. Because the CPC decodes I/O addresses only partially, each device
responds to a range of ports identified by the upper address bits:

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 5pt,
  [*Port range*], [*Device*],
  [#port[\&7Fxx]], [Gate Array (mode, palette, ROM paging) and RAM banking],
  [#port[\&BCxx]--#port[\&BFxx]], [CRTC register select, write, and read],
  [#port[\&DFxx]], [Upper-ROM bank select],
  [#port[\&EFxx]], [Printer port],
  [#port[\&F4xx]--#port[\&F7xx]], [PPI 8255 (keyboard, PSG control, tape)],
  [#port[\&FB7E]--#port[\&FB7F]], [Floppy disc controller],
)

Note that the Gate Array is write-only --- software cannot read back the current
mode or palette and must remember what it set. The developer tools, however, show
the live state directly (Chapter 7).

== Interrupts and timing

#idx("interrupt")The display refreshes at 50 Hz. The Gate Array raises a Z80
interrupt every 52 CRTC scan lines --- six times per frame, every ~3 milliseconds
--- which the firmware uses for the keyboard scan, sound, and timing. Because the
interrupt is tied to the raster position, demo code can synchronise palette and
mode changes to exact points on the screen.

== The custom chips

#idx("Gate Array")*Gate Array.* Amstrad's custom video and memory controller ---
it holds the palette, selects the screen mode, pages the ROMs, and generates the
raster interrupt described above.

#idx("CRTC")*CRTC.* The Cathode Ray Tube Controller generates video timing. The
CPC shipped with several variants, and konCePCja emulates all four types --- the
HD6845S (0), UM6845R (1), MC6845 (2), and the AMS40489 (3) in the Plus machines
--- each with its own register-level quirks, so timing-sensitive software runs
correctly when the matching type is selected.

#idx("PPI")*PPI 8255.* The general-purpose I/O chip: it reads the keyboard matrix,
controls the PSG, and drives the cassette motor.

#idx("PSG")*PSG AY-3-8912.* Three channels of square-wave tone plus a noise
generator and a hardware envelope generator. Its state is visible in the audio
developer tools and can be captured to a YM file (Chapter 12).

#idx("FDC")*FDC uPD765A.* The floppy disc controller, reading and writing both
standard and extended `.dsk` images, including copy-protected formats.

#idx("ASIC")*ASIC.* On the 6128+ (#cfg-key[system.model] `= 3`) a single ASIC
replaces the Gate Array and CRTC and adds hardware sprites, the 4096-colour
palette, programmable raster interrupts, and DMA sound channels, all inspectable
through the ASIC developer-tools window.
