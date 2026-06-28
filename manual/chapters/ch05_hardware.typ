#import "../template.typ": *
#set-chapter("Emulated Hardware")

= Emulated Hardware

#intro[
  konCePCja emulates the complete Amstrad CPC hardware: the Z80 processor and
  every custom chip around it. You do not need to understand any of this to use
  the emulator, but the developer tools (Chapter 7) expose all of it, so this
  chapter names the parts and what each one does.
]

== Z80A CPU

#idx("Z80")The CPC is built around a Zilog Z80A running at 4 MHz. konCePCja
implements the full documented and undocumented instruction set. The developer
tools can set execution breakpoints and memory watchpoints on it, single-step,
and disassemble; see Chapter 7.

== Gate Array

#idx("Gate Array")The Gate Array is Amstrad's custom video and memory controller.
It holds the colour palette, selects the screen mode (0, 1, or 2), banks the
upper and lower ROMs in and out of the address space, and generates the
50 Hz raster interrupt that drives the machine.

== CRTC

#idx("CRTC")The Cathode Ray Tube Controller generates the video timing. The CPC
was shipped with several CRTC variants over its life, and konCePCja emulates all
four types --- the HD6845S (type 0), UM6845R (type 1), MC6845 (type 2), and the
AMS40489 (type 3) found in the Plus machines --- each with its own
register-level behaviour. Software that depends on a specific CRTC quirk runs
correctly when the matching type is selected.

== PPI 8255

#idx("PPI")The Programmable Peripheral Interface is the machine's general-purpose
I/O chip. It reads the keyboard matrix, controls the PSG sound chip, and drives
the cassette motor relay.

== PSG AY-3-8912

#idx("PSG")The Programmable Sound Generator provides three channels of square-wave
tone plus a noise generator and a hardware envelope generator. Its register state
is visible in the audio developer tools and can be captured to a YM chiptune file
(Chapter 12).

== FDC uPD765A

#idx("FDC")The NEC uPD765A Floppy Disc Controller drives the 3-inch disc system.
konCePCja reads and writes both standard and extended `.dsk` disc images,
including copy-protected formats.

== ASIC (CPC 6128+ only)

#idx("ASIC")On the Plus machines (#cfg-key[system.model] `= 3`) the discrete Gate
Array and CRTC are replaced by a single ASIC that adds hardware sprites, an
enhanced 4096-colour palette, programmable raster interrupts, and DMA-driven
sound channels. These features are inspected through the ASIC developer-tools
window.
