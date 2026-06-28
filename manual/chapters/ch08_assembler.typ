#import "../template.typ": *
#set-chapter("Z80 Assembler")

= Z80 Assembler

#intro[
  konCePCja includes an integrated Z80 assembler, so you can write and assemble
  machine code directly into the emulated machine's memory without leaving the
  emulator --- useful for patching, experimenting, and learning.
]

== Using the assembler

#idx("assembler")Open the assembler window from the developer-tools toolbar
(#fkey[F12]). Type or paste Z80 source into the editor, assemble it, and the
resulting bytes are written straight into CPC memory at the addresses you
specify. Errors are reported with their line numbers; the symbol table from a
successful assembly is available for inspection.

== Assembly syntax

The assembler accepts standard Z80 mnemonics with labels, #cmd[ORG] and data
directives, and an expression evaluator. In expressions, #cmd[\&] introduces a
hexadecimal number and #cmd[%] a binary one:

```
        org &4000
start:  ld   hl, &C000      ; screen memory
        ld   bc, &4000
loop:   ld   (hl), %10101010
        inc  hl
        dec  bc
        ld   a, b
        or   c
        jr   nz, loop
        ret
```

== Driving the assembler over IPC

#idx("asm IPC")The assembler is also scriptable through the IPC interface
(Chapter 9). The #ipc-cmd[asm] command group lets a script set the source text,
assemble it, and read back errors or the symbol table:

```
asm text <source>     # set the source buffer
asm assemble          # assemble into memory
asm errors            # read assembly errors
asm symbols           # read the symbol table
```
