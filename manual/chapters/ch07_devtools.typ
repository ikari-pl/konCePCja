#import "../template.typ": *
#set-chapter("Developer Tools")

= Developer Tools

#intro[
  konCePCja includes a full suite of developer tools --- a debugger, memory
  inspector, disassembler, and more --- modelled on modern emulator debug
  environments. Press #fkey[F12] to open them. This chapter tours the windows
  you will use most.
]

== Overview

#idx("DevTools")The developer tools open as a set of dockable windows over the
CPC display. Core windows open automatically the first time you press #fkey[F12];
the rest are reached from the DevTools toolbar.

== Z80 registers

#idx("registers window")The registers window shows the live Z80 register file ---
#reg[AF], #reg[BC], #reg[DE], #reg[HL], the index registers, #reg[PC], and
#reg[SP] --- together with the individual flag bits. Values update as the machine
runs and can be edited while paused.

#figure(image("../images/devtools-registers.png", width: 70%), caption: [The Z80 registers window])

== Disassembly

#idx("disassembly")The disassembly window decodes memory into Z80 mnemonics
around the program counter, follows jumps and calls, and shows symbol labels when
a symbol file is loaded. Breakpoints can be toggled directly in the listing.

== Memory hex viewer

#idx("memory viewer")A hex/ASCII viewer over the full 64 KB address space, with
search and direct editing (poke).

== Stack

The stack window shows the bytes around #reg[SP], so you can watch calls and
returns as they happen.

== Breakpoints and watchpoints

#idx("breakpoints")Set execution breakpoints (optionally conditional, with pass
counts), memory watchpoints, and I/O breakpoints on port reads and writes. When
one fires, emulation pauses and the registers and disassembly update to the
breakpoint location.

== Symbols

Load a symbol file (with #cmd[--sym_file]) to show labels in the disassembler and
to refer to addresses by name.

== Video and audio state

#idx("video state")Dedicated windows expose the live state of the video hardware
(Gate Array palette, CRTC registers, screen mode) and the audio hardware (the
three PSG channels, the envelope generator, and any active DAC peripherals).

== Other windows

The toolbar also opens windows for the data-area map, disassembly export, the
ASIC (on Plus machines), the disc tools, the silicon-disc expansion, and session
recording (Chapter 12). The integrated Z80 assembler has its own chapter
(Chapter 8).

== Graphics finder

#idx("graphics finder")The graphics finder decodes raw memory as CPC pixels, so
you can locate sprites, fonts, and screen data by eye. Point it at an address,
set the screen mode (0, 1, or 2) and the width in bytes, and it renders that
region as a zoomable image. It can also encode edits back into memory, making it
a simple in-memory pixel editor.

== Memory search

#idx("memory search")The memory search tool scans the full address space for a
pattern. You can search for #emph[hex] byte sequences, #emph[text] strings, or
#emph[assembly] (matching decoded instructions), with wildcards for unknown
bytes. Each hit is reported with its address and surrounding context --- the
disassembled instruction for an assembly search, or a hex dump otherwise.

== Pokes

#idx("pokes")The pokes system applies game cheats from `.pok` files. A poke file
lists named cheats (for example "infinite lives"), each a set of memory writes.
Apply a cheat to patch the running game and un-apply it to restore the original
bytes. Where a cheat needs a value you choose --- a number of lives, say ---
konCePCja prompts you for it.

== A debugging session

To see how the windows work together, here is a typical investigation --- finding
where a program writes to the screen.

First, pause the machine (the menu, or #ipc-cmd[pause] over IPC) and open the
disassembly and memory windows. Suppose you want to catch every write to screen
memory, which lives at #port[\&C000]. Add a #emph[watchpoint] on that range, set
to trigger on writes:

```
wp add 0xC000 0x4000 w
```

Resume. The moment the program writes to the screen, emulation pauses, the
registers window shows the exact #reg[PC] of the writing instruction, and the
disassembly window centres on it. The stack window shows who called the routine.
From there you can single-step (#fkey[F12] tools, or #ipc-cmd[step]) to watch the
write happen byte by byte, edit a register or memory value to test a hypothesis,
and set a permanent breakpoint to return to this spot later.

The same loop --- pause, set a breakpoint or watchpoint, run, inspect, step ---
is the heart of debugging on the CPC. Conditional breakpoints (Chapter 9) narrow
it further: break only when a register holds a particular value, or only on the
hundredth time a routine runs. Everything available in these windows is also
available over the IPC interface, so an investigation you do by hand can be turned
into a repeatable script.
