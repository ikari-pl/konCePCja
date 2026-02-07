# konCePCja IPC Protocol Reference

TCP text protocol on **localhost:6543**. One command per line, newline-terminated.
Responses start with `OK` or `ERR <code> <reason>`.

Connect with `nc`: `echo "ping" | nc -w 1 localhost 6543`

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

## Disassembly

| Command | Response |
|---------|----------|
| `disasm <addr> <count>` | `OK\n<line1>\n<line2>\n...` — disassemble N instructions from addr |

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
