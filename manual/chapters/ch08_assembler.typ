#import "../template.typ": *
#set-chapter("Z80 Assembler")

= Z80 Assembler

#intro[
  konCePCja includes an integrated Z80 assembler, so you can write machine code
  and assemble it straight into the emulated machine's memory without leaving the
  emulator --- ideal for patching a game, trying an idea, or learning Z80 by
  watching code run.
]

== Using the assembler

#idx("assembler")Open the assembler window from the developer-tools toolbar
(#fkey[F12]). Type or paste Z80 source into the editor and assemble it; the
resulting bytes are written directly into CPC memory at the addresses your source
specifies. Assembly happens in two passes --- the first builds the symbol table
and measures each instruction, the second emits the bytes --- so labels can be
referenced before they are defined. Errors are reported with line numbers, and
the symbol table from a successful assembly is available for inspection and for
the disassembler.

#figure(image("../images/devtools-assembler.png", width: 80%), caption: [The assembler window with a short example assembled to #port[\&4000]: source on the left with syntax highlighting, and the toolbar to assemble, check, format, load, and save])

== Syntax

The assembler accepts standard Z80 mnemonics, labels, directives, and an
expression evaluator.

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 5pt,
  [*Element*], [*Form*],
  [Label], [`name:` at the start of a line; referenced by name],
  [Origin], [`org <addr>` sets the assembly address],
  [Define bytes], [`db` / `defb` --- one or more byte values or a string],
  [Define words], [`dw` / `defw` --- 16-bit values],
  [Define space], [`ds` / `defs <n>` --- reserve n bytes],
  [Constant], [`name equ <value>`],
  [Comment], [`;` to end of line],
)

#idx("number prefixes")In expressions, #cmd[\&] (or #cmd[\#]) introduces a
hexadecimal number and #cmd[%] a binary one; the usual arithmetic and bitwise
operators are available.

```
        org   &4000
screen  equ   &C000

start:  ld    hl, screen      ; top-left of the screen
        ld    bc, &4000       ; 16K to fill
loop:   ld    (hl), %10101010 ; a striped byte
        inc   hl
        dec   bc
        ld    a, b
        or    c
        jr    nz, loop
        ret
```

#note[
  The first release of the assembler covers the full instruction set, labels,
  the directives above, and expressions. Macros, conditional assembly, repeat
  blocks, and file includes are not yet supported.
]

== Driving the assembler over IPC

#idx("asm IPC")The assembler is also scriptable through the IPC interface
(Chapter 9), so a build script can assemble code into the machine and read back
the results:

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 5pt,
  [*Command*], [*Description*],
  [`asm text <source>`], [Set the source buffer],
  [`asm load <file>`], [Load source from a file],
  [`asm assemble`], [Assemble into memory],
  [`asm errors`], [Read back any assembly errors],
  [`asm symbols`], [Read the resulting symbol table],
  [`asm source`], [Read the current source buffer],
)

Because assembled symbols feed straight into the disassembler and the IPC
#ipc-cmd[sym] commands, you can assemble a routine, breakpoint one of its labels
by name, and step through it --- a complete edit-assemble-debug loop inside the
emulator.
