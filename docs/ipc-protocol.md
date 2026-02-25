# konCePCja IPC Protocol Reference

TCP text protocol on **localhost:6543**. One command per line, newline-terminated.
Responses start with `OK` or `ERR <code> <reason>`.

Connect with `nc`: `echo "ping" | nc -w 1 localhost 6543`

## Companion: Telnet Console (port 6544)

A separate persistent TCP connection on **port 6544** provides a text terminal
for the CPC — everything the CPC prints appears in the terminal, and everything
typed is injected as keyboard input. Unlike IPC (one-shot command/response),
the telnet console keeps the connection open.

```bash
nc localhost 6544       # connect — type to interact with the CPC
```

- Output is captured by hooking TXT_OUTPUT (&BB5A) in the Z80 execution loop
- Input is fed through AutoTypeQueue with ANSI escape → CPC key mapping
- Single client at a time; new connections replace the existing one
- Port probes forward up to +10 if 6544 is taken

See CLAUDE.md § Telnet Console for architecture details and key mappings.

## Lifecycle

| Command | Description |
|---------|-------------|
| `ping` | Returns `OK pong` |
| `version` | Returns version string |
| `help` | Lists all commands |
| `quit [code]` | Exit emulator with given code (default 0) |
| `pause` | Pause emulation |
| `run` | Resume emulation |
| `reset` | Hard reset the CPC |

## Loading

| Command | Description |
|---------|-------------|
| `load <path>` | Load file by extension: `.dsk` (drive A), `.sna` (snapshot), `.cpr` (cartridge), `.bin` (binary at 0x6000) |

## Registers

### Z80 registers

| Command | Response |
|---------|----------|
| `regs` | `OK A=xx F=xx B=xx C=xx D=xx E=xx H=xx L=xx IX=xxxx IY=xxxx SP=xxxx PC=xxxx IM=x HALT=x` |
| `reg get <name>` | `OK xxxx` — names: A B C D E H L F I R IM HALT IFF1 IFF2 AF BC DE HL IX IY SP PC AF' BC' DE' HL' |
| `reg set <name> <value>` | `OK` |

### Hardware registers

| Command | Response |
|---------|----------|
| `regs crtc` | `OK R0=xx..R17=xx VCC=xx VLC=xx HCC=xx HSC=xx VSC=xx VMA=xxxx R52=xx SL=xx` — CRTC 6845 registers and internal counters |
| `regs ga` | `OK MODE=x PEN=xx INK0=xx..INK16=xx ROM_CFG=xx RAM_CFG=xx SL=xx INT_DELAY=xx` — Gate Array state |
| `regs psg` | `OK R0=xx..R15=xx SELECT=xx CONTROL=xx` — AY-3-8912 sound chip registers |

## Memory

| Command | Response |
|---------|----------|
| `mem read <addr> <len> [--view=read\|write] [--bank=N] [ascii]` | `OK <hex> [\|ascii\|]` — reads through Z80 banking |
| `mem write <addr> <hex>` | `OK` — writes through Z80 banking |
| `mem fill <addr> <len> <hex-pattern>` | `OK` — fill memory with repeating hex pattern |
| `mem compare <addr1> <addr2> <len>` | `OK diffs=N [addr:src:dst ...]` — compare two regions, up to 64 diffs listed |

Addresses and values accept decimal, `0x` hex, or `0b` binary.

### Bank access (mem read)

- Default: reads through `membank_read[]` (standard Z80 view — ROM overlays visible at 0x0000)
- `--view=write`: reads through `membank_write[]` (underlying RAM at 0x0000-0x3FFF visible)
- `--bank=N`: reads raw from physical 16KB bank N (`pbRAM + N*16384`), ignoring current mapping

## Breakpoints

| Command | Description |
|---------|-------------|
| `bp add <addr> [if <expr>] [pass <N>]` | Add breakpoint, optionally conditional and/or with pass count |
| `bp del <addr>` | Remove breakpoint |
| `bp clear` | Remove all breakpoints |
| `bp list` | `OK count=N ADDR1 [if EXPR] [pass N hit M] ...` |

### Conditional breakpoints

A breakpoint can include an `if` clause with an expression (see [Expression Syntax](#expression-syntax) below). The breakpoint only fires when the expression evaluates to non-zero.

The optional `pass <N>` clause causes the breakpoint to skip the first N-1 hits — it fires on the Nth hit and every hit thereafter.

```bash
# Break at 0x0038 only when A > 0x10
echo "bp add 0x0038 if A > #10" | nc -w 1 localhost 6543

# Break at 0x4000 on the 100th hit
echo "bp add 0x4000 pass 100" | nc -w 1 localhost 6543

# Combined: conditional + pass count
echo "bp add 0x8000 if peek(#C000) = #FF pass 5" | nc -w 1 localhost 6543
```

## IO Breakpoints

Break on Z80 IN/OUT port access. Port matching uses a mask to support the CPC's incomplete address decoding.

| Command | Description |
|---------|-------------|
| `iobp add <port> [mask] [in\|out\|both] [if <expr>]` | Add IO breakpoint |
| `iobp del <index>` | Remove IO breakpoint by index |
| `iobp clear` | Remove all IO breakpoints |
| `iobp list` | `OK count=N PORT/MASK/DIR ...` |

### Port shorthand

Use `X` in a 4-character hex port spec to wildcard nibbles:

| Shorthand | Port | Mask | Matches |
|-----------|------|------|---------|
| `BCXX` | 0xBC00 | 0xFF00 | Gate Array select (any low byte) |
| `FBXX` | 0xFB00 | 0xFF00 | FDC data register |
| `F5XX` | 0xF500 | 0xFF00 | Printer port |
| `0xBC00 0xFF00` | 0xBC00 | 0xFF00 | Explicit port + mask form |

### Examples

```bash
# Break on any Gate Array OUT
echo "iobp add BCXX out" | nc -w 1 localhost 6543

# Break on CRTC register select IN, conditional
echo "iobp add BCXX in if A = 1" | nc -w 1 localhost 6543

# Explicit port and mask
echo "iobp add 0xF400 0xFF00 in" | nc -w 1 localhost 6543
```

## Stepping

| Command | Description |
|---------|-------------|
| `step [N]` | Single-step N instructions (default 1). Pauses first. |
| `step over [N]` | Execute N instructions, skipping over CALL/RST (sets ephemeral breakpoint at next PC). Timeout: 5s. |
| `step out` | Run until current function returns (uses return address tracking). Timeout: 5s. |
| `step to <addr>` | Run until PC reaches addr (ephemeral breakpoint). Timeout: 5s. |
| `step frame [N]` | Advance N complete frames (default 1), then pause. Blocks until done. |

## Waiting

All wait commands resume emulation, block until condition or timeout, then pause.

| Command | Description |
|---------|-------------|
| `wait pc <addr> [timeout_ms]` | Wait until PC reaches address |
| `wait mem <addr> <value> [mask=0xFF] [timeout_ms]` | Wait until memory matches |
| `wait bp [timeout_ms]` | Wait for any breakpoint hit. Returns `OK PC=xxxx WATCH=0\|1` |
| `wait vbl <count> [timeout_ms]` | Wait for N vertical blanks (~20ms each) |

Default timeout: 5000ms. Returns `ERR 408 timeout` on expiry.

## Hashes

CRC32 hashes for CI regression testing.

| Command | Response |
|---------|----------|
| `hash vram` | `OK crc32=DEADBEEF` — CRC32 of visible screen surface |
| `hash mem <addr> <len>` | `OK crc32=DEADBEEF` — CRC32 of memory range |
| `hash regs` | `OK crc32=DEADBEEF` — CRC32 of packed register state |

## Screenshots & Snapshots

| Command | Description |
|---------|-------------|
| `screenshot [path]` | Save current screen as PNG |
| `snapshot save <path>` | Save emulator state (.sna) |
| `snapshot load <path>` | Load emulator state (.sna) |

## Watchpoints

Memory access breakpoints. Trigger on read, write, or both within an address range.

| Command | Description |
|---------|-------------|
| `wp add <addr> [len] [r\|w\|rw] [if <expr>] [pass <N>]` | Add watchpoint. Default: length 1, type rw. |
| `wp del <index>` | Remove watchpoint by index |
| `wp clear` | Remove all watchpoints |
| `wp list` | `OK count=N idx:ADDR+LEN/TYPE[if ...][pass N] ...` |

When a watchpoint triggers, the breakpoint hit response includes: `OK PC=XXXX WATCH=1 WP_ADDR=XXXX WP_VAL=XX WP_OLD=XX`

Watchpoint conditions can use the special variables `address`, `value`, and `previous` to inspect the access that triggered.

```bash
# Break on any write to screen memory (first 256 bytes)
echo "wp add 0xC000 256 w" | nc -w 1 localhost 6543

# Break only when writing values > 0x80
echo "wp add 0x4000 1 w if value > #80" | nc -w 1 localhost 6543
```

## Symbols

Load, manage, and query the global symbol table. Symbols appear in disassembly output and stack traces.

| Command | Response |
|---------|----------|
| `sym load <file>` | `OK loaded=N` — merge symbols from .sym file |
| `sym add <addr> <name>` | `OK` |
| `sym del <name>` | `OK` |
| `sym list [filter]` | `OK count=N\n  ADDR NAME\n ...` — optional substring filter |
| `sym lookup <addr_or_name>` | `OK <name>` or `OK <addr>` — bidirectional lookup |

```bash
echo "sym add 0x0038 irq_handler" | nc -w 1 localhost 6543
echo "sym lookup irq_handler" | nc -w 1 localhost 6543   # → OK 0038
echo "sym lookup 0x0038" | nc -w 1 localhost 6543         # → OK irq_handler
```

## Memory Search

| Command | Description |
|---------|-------------|
| `mem find hex <start> <end> <hex-pattern>` | Search for hex bytes. `??` = wildcard byte. Max 32 results. |
| `mem find text <start> <end> <string>` | Search for ASCII text. Quotes optional. |
| `mem find asm <start> <end> <pattern>` | Search for Z80 instructions. `*` = operand wildcard. Case-insensitive. |

```bash
# Find CALL 0x0038 instructions (CD 38 00)
echo "mem find hex 0x0000 0xFFFF CD3800" | nc -w 1 localhost 6543

# Find "AMSTRAD" in memory
echo "mem find text 0x0000 0xFFFF AMSTRAD" | nc -w 1 localhost 6543

# Find all LD (*),HL instructions
echo "mem find asm 0x0000 0xFFFF ld (*),hl" | nc -w 1 localhost 6543
```

## Call Stack

| Command | Response |
|---------|----------|
| `stack [depth]` | `OK depth=N\n  SP+0: XXXX [call] label\n ...` |

Walk from SP upward, reading 16-bit words. Default depth: 16, max: 128.

Each entry is heuristically checked: if the instruction *before* the potential return address is a CALL or RST, the entry is marked `[call]`. Symbol names are included when the global symbol table has a match.

```bash
echo "stack 8" | nc -w 1 localhost 6543
```

## Disassembly

| Command | Response |
|---------|----------|
| `disasm <addr> <count> [--symbols]` | `OK\n<line1>\n<line2>\n...` — disassemble N instructions. With `--symbols`, show labels and replace hex targets with symbol names. |
| `disasm follow <addr>` | Recursive disassembly following jumps and calls from entry point. Includes symbol labels. |
| `disasm refs <addr>` | Cross-reference search: find all CALL/JP/JR instructions targeting addr. Max 100 results. |

## Input Replay

### Key names

Named keys: `ESC` `RETURN`/`ENTER` `SPACE` `TAB` `DEL` `COPY` `CONTROL`/`CTRL` `SHIFT`/`LSHIFT`/`RSHIFT` `UP` `DOWN` `LEFT` `RIGHT` `CLR` `F0`-`F9`

Single characters work directly: `A`-`Z`, `a`-`z`, `0`-`9`, punctuation.

### Commands

| Command | Description |
|---------|-------------|
| `input keydown <key>` | Press and hold key in matrix. Works even when paused. |
| `input keyup <key>` | Release key from matrix. |
| `input key <key>` | Tap: press, hold 2 frames, release. Blocks. |
| `input type "<text>"` | Type each character with 2-frame hold and 1-frame gap. Handles uppercase (auto-SHIFT). Blocks. |
| `input joy <n> <dir>` | Joystick N (0 or 1). Directions: `U`/`UP` `D`/`DOWN` `L`/`LEFT` `R`/`RIGHT` `F`/`F1`/`FIRE1` `F2`/`FIRE2`. Prefix `-` to release. `0` releases all. |

### Example: Type and run a BASIC program

```bash
echo "wait vbl 50"                          | nc -w 5 localhost 6543
echo 'input type "10 PRINT CHR$(42)"'       | nc -w 15 localhost 6543
echo "input key RETURN"                     | nc -w 2 localhost 6543
echo 'input type "RUN"'                     | nc -w 5 localhost 6543
echo "input key RETURN"                     | nc -w 2 localhost 6543
echo "step frame 20"                        | nc -w 5 localhost 6543
echo "screenshot /tmp/result.png"           | nc -w 1 localhost 6543
```

## Instruction Trace

Ring-buffer recording of Z80 instruction execution.

| Command | Description |
|---------|-------------|
| `trace on [buffer_size]` | Enable tracing. Default buffer: 65536 entries. |
| `trace off` | Disable and free buffer. |
| `trace status` | `OK active=0\|1 entries=N` |
| `trace dump <path>` | Write trace to text file. Format: `ADDR OPCODE A=xx F=xx BC=xxxx DE=xxxx HL=xxxx SP=xxxx` |
| `trace on_crash <path>` | Enable tracing and auto-dump on breakpoint hit. |

## Frame Dumps

| Command | Description |
|---------|-------------|
| `frames dump <pattern> <count> [delay_cs]` | Advance N frames, saving output. If pattern ends in `.gif`, produces an animated GIF. Otherwise, saves a PNG per frame. Max 10000 frames. |

**PNG mode** (default): Pattern uses printf `%d`/`%04d` for frame number, or `_NNNN.png` is appended. Returns `OK saved=N`.

**GIF mode**: When the path ends in `.gif`, all frames are encoded into a single optimized animated GIF with LZW compression and delta encoding. The optional `delay_cs` parameter sets inter-frame delay in centiseconds (default 2 = 50fps, matching CPC VBL rate). Returns `OK frames=N`.

### Examples

```bash
# PNG series
echo "frames dump /tmp/frame_%04d.png 50" | nc -w 30 localhost 6543

# Animated GIF at 50fps (default)
echo "frames dump /tmp/recording.gif 100" | nc -w 60 localhost 6543

# Animated GIF at 10fps for slow-motion review
echo "frames dump /tmp/slowmo.gif 50 10" | nc -w 30 localhost 6543
```

## Event System

Register IPC commands to execute automatically on triggers.

### Triggers

| Trigger | Syntax | Description |
|---------|--------|-------------|
| PC match | `pc=0xADDR` | Fires when Z80 PC reaches address |
| Memory write | `mem=0xADDR` or `mem=0xADDR:VAL` | Fires on write to address (optionally matching value) |
| VBL interval | `vbl=N` | Fires every N vertical blanks |

### Commands

| Command | Description |
|---------|-------------|
| `event on <trigger> <command>` | Add persistent event. Returns `OK id=N`. |
| `event once <trigger> <command>` | Add one-shot event (removed after first fire). |
| `event off <id>` | Remove event by ID. |
| `event list` | List all events with triggers and commands. |

### Examples

```bash
# Screenshot every 50 frames
echo "event on vbl=50 screenshot /tmp/periodic.png" | nc -w 1 localhost 6543

# One-shot: screenshot when PC reaches entry point
echo "event once pc=0x8000 screenshot /tmp/entry.png" | nc -w 1 localhost 6543

# Log memory writes to screen memory
echo "event on mem=0xC000 hash vram" | nc -w 1 localhost 6543
```

## Expression Syntax

Expressions are used in conditional breakpoints (`bp add ... if <expr>`) and IO breakpoints (`iobp add ... if <expr>`). The syntax is inspired by WinAPE.

### Operators (lowest to highest precedence)

| Operator | Description |
|----------|-------------|
| `or` | Bitwise OR |
| `xor` | Bitwise XOR |
| `and` | Bitwise AND |
| `<` `<=` `=` `>=` `>` `<>` | Comparison (returns -1 for true, 0 for false) |
| `+` `-` | Addition, subtraction |
| `*` `/` `mod` | Multiplication, division, modulo |
| `not` | Bitwise NOT (unary) |

All operations are 32-bit signed integers. Division by zero returns 0.

### Number literals

| Format | Example |
|--------|---------|
| Decimal | `42` |
| Hex (`#` or `&` or `0x`) | `#FF`, `&FF`, `0xFF` |
| Binary (`%` or `0b`) | `%10110`, `0b10110` |

### Variables

**Z80 registers:** `A` `B` `C` `D` `E` `H` `L` `F` `I` `R` `IM` `IFF1` `IFF2` `AF` `BC` `DE` `HL` `IX` `IY` `SP` `PC` `IXh` `IXl` `IYh` `IYl`

**Shadow registers:** `AF'` `BC'` `DE'` `HL'`

**Context variables:** `address` (breakpoint address), `value` (data value), `previous` (previous value for watchpoints), `mode` (access mode)

### Functions

| Function | Description |
|----------|-------------|
| `peek(addr)` | Read byte from memory at addr |
| `byte(val)` | Low byte (val AND 0xFF) |
| `hibyte(val)` | High byte ((val >> 8) AND 0xFF) |
| `word(val)` | Low word (val AND 0xFFFF) |
| `hiword(val)` | High word ((val >> 16) AND 0xFFFF) |
| `ay(reg)` | Read AY-3-8912 (PSG) register 0-15 |
| `crtc(reg)` | Read CRTC 6845 register 0-17 |
| `timer_start(id)` | Start debug timer, returns 0 |
| `timer_stop(id)` | Stop debug timer, returns elapsed microseconds |

### Examples

```
A > #10 and B <> 0
peek(HL) = #C9
PC >= #8000 and PC < #C000
byte(BC) = #7F and hibyte(BC) = #F4
ay(7) and %00111111
```

## Debug Timers

Measure elapsed T-states between code points. Timers are triggered via expression functions in conditional breakpoints.

| Command | Description |
|---------|-------------|
| `timer list` | `OK count=N id=1 count=5 last=123 min=100 max=200 avg=145 ...` |
| `timer clear` | `OK` — remove all timers |

Timer values are reported in microseconds (T-states / 4, since the CPC runs at 4 MHz).

### Example: Measure interrupt handler duration

```bash
# Start timer 1 when entering interrupt handler at 0x0038
echo "bp add 0x0038 if timer_start(1)" | nc -w 1 localhost 6543

# Stop timer 1 when hitting the RETI at 0x0052
echo "bp add 0x0052 if timer_stop(1)" | nc -w 1 localhost 6543

# Run for a while, then check timers
echo "step frame 500" | nc -w 30 localhost 6543
echo "timer list" | nc -w 1 localhost 6543
```

Note: `timer_start()` always returns 0 and `timer_stop()` returns the elapsed microseconds, so `if timer_start(1)` evaluates to false and the breakpoint doesn't actually pause — it just starts/stops the timer as a side effect. To also break, use `if timer_start(1) or 1`.

## Auto-Type

WinAPE-compatible auto-type with special key syntax.

| Command | Description |
|---------|-------------|
| `autotype <text>` | Queue text for typing. Supports `~KEY~` syntax for special keys (e.g. `~ENTER~`, `~SHIFT~`). |
| `autotype status` | `OK active: N actions remaining` or `OK idle` |
| `autotype clear` | Cancel pending auto-type queue. |

```bash
echo 'autotype 10 PRINT "HELLO"~ENTER~RUN~ENTER~' | nc -w 1 localhost 6543
```

## Disc Management

File-level and sector-level access to DSK disc images.

### Disc Commands

| Command | Description |
|---------|-------------|
| `disk formats` | `OK data vendor system ...` — list available format names |
| `disk format <A\|B> <format_name>` | Format drive with named format |
| `disk new <path> [format]` | Create a new blank DSK file (default format: `data`) |
| `disk ls <A\|B>` | List AMSDOS files on drive. Returns `name size [R/O] [SYS]` per line |
| `disk cat <A\|B> <filename>` | Read file contents as hex (strips AMSDOS header). Returns `OK size=N\nhex...` |
| `disk get <A\|B> <filename> <local_path>` | Extract file to local filesystem |
| `disk put <A\|B> <local_path> [cpc_name]` | Write local file to disc (auto-generates CPC name if omitted) |
| `disk rm <A\|B> <filename>` | Delete file from disc |
| `disk info <A\|B> <filename>` | `OK type=basic\|binary\|protected load=XXXX exec=XXXX size=N` — AMSDOS header info |

### Sector Commands

| Command | Description |
|---------|-------------|
| `disk sector read <drive> <track> <side> <sector_id>` | Read raw sector data as hex |
| `disk sector write <drive> <track> <side> <sector_id> <hex>` | Write raw hex data to sector |
| `disk sector info <drive> <track> <side>` | List sectors on track: `C=xx H=xx R=xx N=xx size=N` per sector |

```bash
# List files on drive A
echo "disk ls A" | nc -w 1 localhost 6543

# Extract a file
echo "disk get A GAME.BAS /tmp/game.bas" | nc -w 1 localhost 6543

# Read sector C1 on track 0 side 0
echo "disk sector read A 0 0 C1" | nc -w 1 localhost 6543
```

## Recording

Capture audio and video output.

### WAV Recording

| Command | Description |
|---------|-------------|
| `record wav start <path>` | Start recording audio to WAV file |
| `record wav stop` | Stop recording. Returns `OK <path> <bytes>` |
| `record wav status` | `OK recording <path> <bytes>` or `OK idle` |

### YM Recording

| Command | Description |
|---------|-------------|
| `record ym start <path>` | Start recording PSG registers to YM chiptune file |
| `record ym stop` | Stop recording. Returns `OK <path> <frames>` |
| `record ym status` | `OK recording <path> <frames>` or `OK idle` |

### AVI Recording

| Command | Description |
|---------|-------------|
| `record avi start <path> [quality]` | Start recording video+audio to AVI (default quality: 85) |
| `record avi stop` | Stop recording. Returns `OK <path> <frames>` |
| `record avi status` | `OK recording <path> frames=N bytes=N` or `OK idle` |

```bash
# Record 5 seconds of audio
echo "record wav start /tmp/audio.wav" | nc -w 1 localhost 6543
echo "step frame 250" | nc -w 10 localhost 6543
echo "record wav stop" | nc -w 1 localhost 6543
```

## Pokes

Game cheat system supporting .pok format files.

| Command | Description |
|---------|-------------|
| `poke load <path>` | Load pokes from .pok file. Returns `OK loaded N games` |
| `poke list` | List loaded games with their pokes and application status |
| `poke apply <game_idx> <poke_idx\|all>` | Apply one poke or all pokes for a game |
| `poke unapply <game_idx> <poke_idx>` | Restore original values for a poke |
| `poke write <hex_addr> <value>` | Direct single-byte poke (0-255) |

```bash
echo "poke load cheats.pok" | nc -w 1 localhost 6543
echo "poke list" | nc -w 1 localhost 6543
echo "poke apply 0 all" | nc -w 1 localhost 6543
```

## Configuration Profiles

Save and switch between named config presets.

| Command | Description |
|---------|-------------|
| `profile list` | List profiles. Active profile marked with `*`. |
| `profile current` | Show active profile name |
| `profile load <name>` | Switch to named profile |
| `profile save <name>` | Save current config as named profile |
| `profile delete <name>` | Remove a profile |

## Configuration

Read and write emulator settings.

| Command | Description |
|---------|-------------|
| `config get crtc_type` | `OK 0`-`3` — current CRTC type |
| `config get crtc_info` | `OK type=N chip=<name> manufacturer=<name>` |
| `config get ram_size` | `OK <kb>` — current RAM in KB |
| `config get silicon_disc` | `OK 0\|1` — Silicon Disc enabled |
| `config set crtc_type <0-3>` | Set CRTC type (0=HD6845S, 1=UM6845R, 2=MC6845, 3=ASIC) |
| `config set ram_size <kb>` | Set RAM size (reset required) |
| `config set silicon_disc <0\|1>` | Enable/disable Silicon Disc |

## Status

| Command | Description |
|---------|-------------|
| `status` | Overall emulator status summary |
| `status drives` | Detailed drive state (loaded image, tracks, format) |

## ASIC Registers (Plus Range)

| Command | Description |
|---------|-------------|
| `regs asic` | Dump ASIC state: sprites, DMA, palette, interrupts |
| `regs asic dma` | DMA channel details (3 channels) |
| `regs asic sprites` | All 16 hardware sprite positions and magnification |
| `regs asic interrupts` | Raster line, DMA IRQ flags, vector |
| `regs asic palette` | 32-colour ASIC palette in RGB |
| `asic sprite <n>` | Individual sprite info |
| `asic palette [index]` | Read palette entry or all entries |
| `asic dma <channel>` | Individual DMA channel state |

## Silicon Disc

256 KB battery-backed RAM disc in banks 4-7.

| Command | Description |
|---------|-------------|
| `sdisc status` | `OK enabled\|disabled allocated\|not-allocated size=256K banks=4-7` |
| `sdisc clear` | Zero all 256 KB of Silicon Disc memory |
| `sdisc save <path>` | Save Silicon Disc contents to file |
| `sdisc load <path>` | Load Silicon Disc contents from file (auto-enables if disabled) |

## ROM Management

Load and unload ROM images in 32 expansion ROM slots.

| Command | Description |
|---------|-------------|
| `rom list` | List all 32 slots with loaded ROM paths |
| `rom load <slot> <path>` | Load ROM file into slot (0-31). Validates ROM header. |
| `rom unload <slot>` | Unload ROM from slot (2-31; system ROMs 0-1 cannot be unloaded) |
| `rom info <slot>` | `OK slot=N loaded=true\|false size=16384 crc=XXXXXXXX path=...` |

```bash
echo "rom list" | nc -w 1 localhost 6543
echo "rom load 7 maxam.rom" | nc -w 1 localhost 6543
```

## Data Areas

Mark memory regions as data (not code) for correct disassembly output.

| Command | Description |
|---------|-------------|
| `data mark <start> <end> <bytes\|words\|text> [label]` | Mark address range as data |
| `data clear <addr\|all>` | Remove data area at addr, or clear all |
| `data list` | List all data areas: `XXXX XXXX type [label]` per line |

```bash
echo "data mark 0x8000 0x80FF bytes sprite_data" | nc -w 1 localhost 6543
echo "data mark 0x9000 0x903F text message_table" | nc -w 1 localhost 6543
echo "data list" | nc -w 1 localhost 6543
```

## Graphics Finder

Decode and manipulate CPC pixel data at arbitrary memory addresses.

| Command | Description |
|---------|-------------|
| `gfx view <addr> <w_bytes> <h> <mode> [path]` | Decode pixels. If path given, export as BMP. Returns dimensions. |
| `gfx decode <byte_hex> <mode>` | Decode a single byte to palette indices |
| `gfx paint <addr> <w> <h> <mode> <x> <y> <color>` | Set pixel colour at (x,y) in the decoded region |
| `gfx palette` | Show current 16-colour palette as RGBA hex |

Mode 0 = 2 pixels/byte (16 colours), Mode 1 = 4 pixels/byte (4 colours), Mode 2 = 8 pixels/byte (2 colours).

```bash
# View 16x16 sprite at 0xC000 in Mode 1, export to BMP
echo "gfx view 0xC000 4 16 1 /tmp/sprite.bmp" | nc -w 1 localhost 6543

# Show palette
echo "gfx palette" | nc -w 1 localhost 6543
```

## Disassembly Export

Export memory as Z80 assembly source.

| Command | Description |
|---------|-------------|
| `disasm export <start> <end> [path] [--symbols]` | Export range as `.asm` source. Uses data areas for correct formatting. With `--symbols`, includes symbol labels. If path given, writes to file; otherwise returns in response. |

```bash
echo "disasm export 0x4000 0x5FFF /tmp/game.asm --symbols" | nc -w 1 localhost 6543
```

## Session Recording

Record and replay full emulator sessions (input events + state snapshots).

| Command | Description |
|---------|-------------|
| `session record <path>` | Start recording. Saves a snapshot alongside the recording. |
| `session play <path>` | Start playback. Loads the embedded snapshot first. Returns `OK playing from <path> frames=N`. |
| `session stop` | Stop recording or playback |
| `session status` | `OK state=idle\|recording\|playing frames=N events=N path=...` |

```bash
# Record a session
echo "session record /tmp/demo" | nc -w 1 localhost 6543
# ... play for a while ...
echo "session stop" | nc -w 1 localhost 6543

# Play it back
echo "session play /tmp/demo" | nc -w 1 localhost 6543
```

## Enhanced Search

Full-memory search with glob-style wildcards. Searches the entire 64K address space.

| Command | Description |
|---------|-------------|
| `search hex <pattern>` | Search for hex bytes. `??` = wildcard. Max 256 results. |
| `search text <pattern>` | Search for ASCII text. Case-sensitive. |
| `search asm <pattern>` | Search for Z80 mnemonics with `?` (single char) and `*` (any sequence) glob wildcards. |

Note: `search` scans 0x0000-0xFFFF. For range-limited search, use `mem find`.

```bash
# Find all RET instructions
echo "search asm ret" | nc -w 1 localhost 6543

# Find all JP instructions with any operand
echo "search asm jp *" | nc -w 1 localhost 6543
```

## CLI Flags

| Flag | Description |
|------|-------------|
| `--headless` / `-H` | Run without window or audio. IPC and screenshots still work. |
| `--exit-after <spec>` / `-E <spec>` | Exit after duration: `100f` (frames), `5s` (seconds), `3000ms` (milliseconds). |
| `--exit-on-break` / `-B` | Exit with code 1 on breakpoint instead of pausing. |

## Error Codes

| Code | Meaning |
|------|---------|
| `400` | Bad arguments or syntax |
| `404` | Resource not found |
| `408` | Timeout |
| `415` | Unsupported file format |
| `500` | Internal error |
| `501` | Command not implemented |
| `503` | Service unavailable (e.g. no video surface) |

## Typical Automation Workflow

```bash
#!/bin/bash
# Boot CPC, load a DSK, run a program, verify output

./koncepcja --headless &
EMU_PID=$!
sleep 2

# Wait for BASIC prompt
echo "wait vbl 50" | nc -w 5 localhost 6543

# Load disk
echo "load /path/to/game.dsk" | nc -w 2 localhost 6543

# Type RUN"GAME and press RETURN
echo 'input type "RUN\"GAME"' | nc -w 10 localhost 6543
echo "input key RETURN" | nc -w 2 localhost 6543

# Wait for game to load
echo "step frame 500" | nc -w 30 localhost 6543

# Verify screen content
HASH=$(echo "hash vram" | nc -w 1 localhost 6543)
echo "Screen hash: $HASH"

# Take screenshot
echo "screenshot /tmp/game_loaded.png" | nc -w 1 localhost 6543

# Clean up
echo "quit 0" | nc -w 1 localhost 6543
```
