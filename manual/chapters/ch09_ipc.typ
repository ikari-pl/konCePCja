#import "../template.typ": *
#set-chapter("IPC Debugging Interface")

= IPC Debugging Interface

#intro[
  konCePCja exposes a TCP control interface that lets external scripts and tools
  drive and inspect the emulated machine in depth: pause and step the processor,
  read and write memory and every hardware register, set conditional breakpoints
  and watchpoints, search memory, inject input, and hash state for regression
  testing. This chapter is the working reference.
]

#figure(image("../images/topology.svg", width: 92%), caption: [konCePCja exposes three TCP services; this chapter covers the IPC interface on port 6543])

== Connecting

#idx("IPC")The server listens on #port[localhost:6543]. The protocol is plain
text: one command per line, newline-terminated. Every response begins with
#cmd[OK] (followed by any result) or #cmd[ERR] #arg[code] #arg[reason]. Any tool
that speaks TCP works:

```bash
echo "ping" | nc -w 1 localhost 6543      # → OK pong
nc localhost 6543                         # interactive
```

Numbers may be given in decimal, hexadecimal (`0x`, `#`, or `&` prefix), or
binary (`0b` or `%` prefix) throughout.

== Lifecycle and loading

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Command*], [*Description*],
  [`ping`], [Replies `OK pong`],
  [`version`], [Version string],
  [`help`], [List all commands],
  [`pause` / `run`], [Pause or resume emulation],
  [`reset`], [Hard reset the CPC],
  [`quit [code]`], [Exit the emulator (default code 0)],
  [`load <path>`], [Load by extension: `.dsk` (drive A), `.sna`, `.cpr`, or `.bin` (loaded at #port[0x6000])],
)

== Registers

#idx("registers, IPC")Read or write the Z80 register file, and read the custom
chips' live state:

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Command*], [*Description*],
  [`regs`], [Dump all Z80 registers (`A`, `F`, … `PC`, plus `IM` and `HALT`)],
  [`reg get <name>`], [One register --- `A`…`L`, `AF`…`SP`, the shadow set `AF'`…`HL'`, `I`, `R`, `IM`, `IFF1`, `IFF2`],
  [`reg set <name> <value>`], [Write a register],
  [`regs crtc`], [CRTC registers `R0`–`R17` and internal counters],
  [`regs ga`], [Gate Array state --- mode, pen, all 17 inks, ROM/RAM config],
  [`regs psg`], [AY-3-8912 registers `R0`–`R15`],
)

== Memory

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Command*], [*Description*],
  [`mem read <addr> <len> [ascii]`], [Read memory through Z80 banking],
  [`mem write <addr> <hex>`], [Write a hex byte string],
  [`mem fill <addr> <len> <hex>`], [Fill with a repeating pattern],
  [`mem compare <a1> <a2> <len>`], [Compare two regions; lists up to 64 differences],
)

#idx("memory banking")By default reads see the standard Z80 view, with ROM
overlaid at #port[0x0000]. Two options change that: `--view=write` exposes the
underlying RAM beneath the ROM, and `--bank=N` reads a raw physical 16 KB bank,
ignoring the current mapping --- useful for inspecting paged-out memory.

== Breakpoints

#idx("breakpoints, IPC")Execution breakpoints can be plain, conditional, or
counted:

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Command*], [*Description*],
  [`bp add <addr> [if <expr>] [pass <N>]`], [Add a breakpoint, optionally conditional and/or skipping its first #emph[N]−1 hits],
  [`bp del <addr>` / `bp clear`], [Remove one or all],
  [`bp list`], [List breakpoints with their conditions and hit counts],
)

```bash
# Break at the interrupt handler only when A exceeds 0x10
echo "bp add 0x0038 if A > #10" | nc -w 1 localhost 6543

# Break at 0x4000 on its 100th execution
echo "bp add 0x4000 pass 100" | nc -w 1 localhost 6543
```

== Watchpoints

#idx("watchpoints")Watchpoints break on memory #emph[access] rather than
execution:

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Command*], [*Description*],
  [`wp add <addr> [len] [r|w|rw] [if <expr>]`], [Watch a range for read, write, or both (default: 1 byte, `rw`)],
  [`wp del <index>` / `wp clear`], [Remove one or all],
  [`wp list`], [List watchpoints],
)

A watchpoint condition can use the special variables `address`, `value`, and
`previous` to inspect the access that triggered it:

```bash
# Break only when a value above 0x80 is written to screen memory
echo "wp add 0xC000 256 w if value > #80" | nc -w 1 localhost 6543
```

== I/O breakpoints

#idx("I/O breakpoints")Break on Z80 `IN`/`OUT` to a port. Because the CPC
decodes I/O addresses incompletely, ports are matched through a mask; the `X`
shorthand wildcards a nibble:

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Shorthand*], [*Matches*],
  [`BCXX`], [Gate Array register select (#port[0xBC00], mask #port[0xFF00])],
  [`FBXX`], [FDC data register],
  [`F5XX`], [Printer port],
)

```bash
echo "iobp add BCXX out" | nc -w 1 localhost 6543   # any Gate Array OUT
```

== Expression syntax

#idx("expression syntax")Conditional breakpoints, watchpoints, and I/O
breakpoints share a WinAPE-inspired expression language. All arithmetic is 32-bit
signed; comparisons return −1 for true and 0 for false.

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Category*], [*Tokens*],
  [Operators (low→high)], [`or`, `xor`, `and`, comparisons (`< <= = >= > <>`), `+ -`, `* / mod`, unary `not`],
  [Numbers], [decimal `42`, hex `#FF`/`&FF`/`0xFF`, binary `%10110`/`0b10110`],
  [Registers], [`A`…`L`, `AF`…`SP`, `IXh`/`IXl`, shadows `AF'`…`HL'`],
  [Context], [`address`, `value`, `previous`, `mode`],
  [Functions], [`peek(addr)`, `byte/hibyte/word/hiword(v)`, `ay(reg)`, `crtc(reg)`, `timer_start/stop(id)`],
)

```
A > #10 and B <> 0
peek(HL) = #C9
PC >= #8000 and PC < #C000
```

== Stepping and waiting

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Command*], [*Description*],
  [`step [N]`], [Single-step N instructions],
  [`step over [N]`], [Step, skipping over `CALL`/`RST`],
  [`step out`], [Run until the current routine returns],
  [`step to <addr>`], [Run until PC reaches an address],
  [`step frame [N]`], [Advance N complete video frames],
  [`wait pc <addr> [ms]`], [Resume, block until PC reaches addr, then pause],
  [`wait mem <addr> <val> [mask] [ms]`], [Block until memory matches],
  [`wait bp [ms]`], [Block until any breakpoint fires],
  [`wait vbl <count> [ms]`], [Wait N vertical blanks (~20 ms each)],
)

The `wait` commands resume emulation, block until their condition or a timeout
(default 5000 ms, then `ERR 408 timeout`), and pause again --- the building block
for deterministic test scripts.

== Inspecting code and memory

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Command*], [*Description*],
  [`disasm <addr> <count> [--symbols]`], [Disassemble N instructions],
  [`disasm follow <addr>`], [Recursive disassembly following jumps and calls],
  [`disasm refs <addr>`], [Find all instructions targeting an address],
  [`stack [depth]`], [Walk the call stack from `SP`, marking likely return addresses `[call]`],
  [`mem find hex <start> <end> <pat>`], [Search bytes (`??` = wildcard)],
  [`mem find text <start> <end> <str>`], [Search ASCII text],
  [`mem find asm <start> <end> <pat>`], [Search instructions (`*` = operand wildcard)],
  [`sym load/add/del/list/lookup`], [Manage and query the symbol table],
)

== Injecting input

#idx("input injection")Keyboard, joystick, and mouse input are all driveable
(keys take a friendly name or a single character):

```
input keydown SHIFT      # press and hold
input key RETURN         # tap (press, hold, release)
input type "mode 1"      # type literal text
input joy 0 F1           # joystick 0, fire 1
input mouse move 10 -4   # relative mouse motion (needs a mouse device)
```

#note[
  `input type` emits only mapped characters --- it does #emph[not] interpret the
  WinAPE `~KEY~` syntax. For special keys, holds, or multi-line entry use the
  `autotype` command, which understands tokens such as `~RETURN~`.
]

== Hashes for regression testing

#idx("hashes")For continuous integration, CRC32 hashes turn machine state into a
single comparable value:

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Command*], [*Description*],
  [`hash vram`], [CRC32 of the visible screen],
  [`hash mem <addr> <len>`], [CRC32 of a memory range],
  [`hash regs`], [CRC32 of the packed register state],
)

== A worked example

Type and run a one-line BASIC program, wait for it to settle, then assert on the
result by hashing the screen:

```bash
echo 'reset'                              | nc -w 1 localhost 6543
echo 'wait vbl 50'                        | nc -w 2 localhost 6543
echo 'input type "10 PRINT CHR\$(42)"'    | nc -w 5 localhost 6543
echo 'input key RETURN'                   | nc -w 2 localhost 6543
echo 'autotype "run~RETURN~"'             | nc -w 5 localhost 6543
echo 'step frame 20'                      | nc -w 5 localhost 6543
echo 'hash vram'                          | nc -w 1 localhost 6543
```

Run the emulator headless (no window) for CI with the #cmd[--headless] flag; the
protocol is identical in headless and windowed modes. A Python test harness built
on these commands lives in the project's `test/integrated` directory.
