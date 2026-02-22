# konCePCja - Agent Instructions

Amstrad CPC emulator based on Caprice32, with Dear ImGui interface and IPC debugging support.

## Project Structure

```
src/
├── kon_cpc_ja.cpp     # Main emulator loop, keyboard handling
├── z80.cpp            # Z80 CPU emulation with breakpoint support
├── video.cpp          # Video rendering, scanlines
├── psg.cpp            # Sound chip (AY-3-8912) emulation
├── tape.cpp           # CDT/TZX tape playback engine
├── slotshandler.cpp   # File loading (DSK, CDT, SNA, CPR)
├── imgui_ui.cpp       # Dear ImGui interface (topbar, devtools, menus)
├── koncepcja_ipc_server.cpp  # TCP IPC server for external debugging
└── disk.cpp           # Floppy disk emulation
```

## Emulated Devices

### Core Hardware
- **Z80A CPU** — Full instruction set with breakpoint/watchpoint support
- **Gate Array** — Palette, screen mode, ROM banking, interrupt generation
- **CRTC** — Types 0-3 (HD6845S, UM6845R, MC6845, AMS40489)
- **PPI 8255** — Keyboard matrix, PSG control, tape motor
- **PSG AY-3-8912** — 3-channel sound + envelope generator
- **FDC uPD765A** — Floppy disk controller with standard/extended DSK support
- **ASIC (6128+)** — Plus-specific features: DMA, programmable raster, sprites

### Peripheral Expansions
- **Digiblaster** — Printer port 8-bit DAC. Uses `CPC.printer_port` value and a 256-level lookup table (`Level_PP[]`) mixed into the PSG audio output
- **AmDrum (Cheetah)** — 8-bit DAC on port `&FFxx` (the "uncontested" I/O space where all upper address bits are high). Same mixing pattern as Digiblaster via `Level_AmDrum[]`
- **Dobbertin SmartWatch** — Dallas DS1216 phantom RTC in upper ROM socket. Intercepts ROM reads using a 64-bit recognition pattern sent via address lines A0 (data) and A2 (mode). Returns BCD-encoded host system time via data bit D0
- **AMX Mouse** — Joystick port mouse on keyboard matrix row 9. Direction bits pulse LOW for one "mickey" per deselect/reselect cycle of row 9. Uses monostable reset tracking
- **Symbiface II** — Multi-function expansion board:
  - **IDE (ATA PIO)** at `&FD00-&FD3F` — Standard ATA register file backed by raw `.img` files. Supports READ/WRITE SECTORS and IDENTIFY DEVICE
  - **RTC (DS12887)** at `&FD00-&FD3F` — 14 time registers (BCD from host clock) + 50 bytes CMOS NVRAM
  - **PS/2 Mouse** at `&FBEE`/`&FBEF` — Kempston protocol with wrapping 8-bit X/Y counters
- **M4 Board** — Virtual filesystem expansion via command/response protocol. OUTs to `&FE00` accumulate command bytes, OUT to `&FC00` triggers execution. Response written to ROM overlay at `&E800`. Backs virtual SD card with a host directory (path traversal protected)
- **Drive/Tape Sounds** — Procedurally generated audio effects for FDC motor hum, head seek clicks, and tape loading hiss, mixed into the PSG audio output
- **Multiface II** — ROM-based debugging interface (original Caprice32 implementation)
- **Amstrad Magnum Phaser** — Light gun via CRTC register intercept

## Building

```bash
# macOS
make -j$(nproc) ARCH=macos

# Linux
make -j$(nproc)

# Debug build
make -j$(nproc) DEBUG=1
```

## Command Line Arguments

Run `./koncepcja --help` for the full list:

```
Usage: koncepcja [options] <slotfile(s)>

Options:
  -a/--autocmd=<command>   Execute BASIC command after CPC boots
  -c/--cfg_file=<file>     Use custom config file
  -h/--help                Show help
  -i/--inject=<file>       Inject binary into memory after boot
  -o/--offset=<address>    Injection address (default: 0x6000)
  -O/--override=<opt=val>  Override config option (repeatable)
  -s/--sym_file=<file>     Load symbols for disassembly
  -V/--version             Show version
  -v/--verbose             Verbose logging

Slot files: .dsk (disk), .cdt/.voc (tape), .cpr (cartridge), .sna (snapshot)
```

### Examples

```bash
# Load a disk image
./koncepcja game.dsk

# Load disk and auto-run
./koncepcja -a 'run"game' game.dsk

# Inject binary at custom address
./koncepcja -i loader.bin -o 0x4000

# Override CPC model (3 = 6128+)
./koncepcja -O system.model=3 game.dsk
```

## IPC Debugging Interface

The emulator runs a TCP server on **port 6543** (localhost only) for external debugging.

### Connecting

```bash
# Using netcat
nc localhost 6543

# Using socat (for line-buffered interaction)
socat - TCP:localhost:6543
```

### Getting Help

```bash
echo "help" | nc localhost 6543
# Response: OK commands: ping version help pause run reset load regs reg set/get mem bp(list/add/del/clear) step wait screenshot snapshot(save/load) disasm devtools
```

### IPC Command Reference

| Command | Description | Example |
|---------|-------------|---------|
| `ping` | Test connection | `ping` → `OK pong` |
| `version` | Get version | `version` → `OK kaprys-0.1` |
| `help` | List commands | `help` → `OK commands: ...` |
| `pause` | Pause emulation | `pause` → `OK` |
| `run` | Resume emulation | `run` → `OK` |
| `reset` | Reset CPC | `reset` → `OK` |
| `regs` | Dump all registers | `regs` → `OK A=00 F=00 ...` |
| `reg get <R>` | Get register | `reg get PC` → `OK 1234` |
| `reg set <R> <V>` | Set register | `reg set PC 0x4000` → `OK` |
| `mem read <addr> <len> [ascii]` | Read memory | `mem read 0x4000 16 ascii` |
| `mem write <addr> <hex>` | Write memory | `mem write 0x4000 C3004000` |
| `bp add <addr>` | Add breakpoint | `bp add 0x4000` → `OK` |
| `bp del <addr>` | Remove breakpoint | `bp del 0x4000` → `OK` |
| `bp list` | List breakpoints | `bp list` → `OK count=1 4000` |
| `bp clear` | Clear all breakpoints | `bp clear` → `OK` |
| `step [n]` | Step N instructions | `step 10` → `OK` |
| `wait pc <addr> [timeout]` | Wait for PC | `wait pc 0x4000 5000` |
| `wait mem <addr> <val> [mask] [timeout]` | Wait for memory value | `wait mem 0xBE80 0xFF` |
| `wait bp [timeout]` | Wait for breakpoint hit | `wait bp 10000` |
| `wait vbl <n> [timeout]` | Wait N vertical blanks | `wait vbl 50` |
| `disasm <addr> <count>` | Disassemble | `disasm 0x4000 10` |
| `screenshot [path]` | Take screenshot | `screenshot /tmp/shot.bmp` |
| `snapshot save <path>` | Save state | `snapshot save game.sna` |
| `snapshot load <path>` | Load state | `snapshot load game.sna` |
| `load <path>` | Load file (.dsk/.sna/.cpr/.bin) | `load game.dsk` |
| `devtools` | Open DevTools window | `devtools` → `OK` |

### Scripting Example

```bash
#!/bin/bash
# wait-and-screenshot.sh - Wait for game to load, take screenshot

echo "load game.dsk" | nc localhost 6543
sleep 1
echo "run" | nc localhost 6543
echo "wait pc 0xC000 30000" | nc localhost 6543  # Wait for game entry
echo "screenshot /tmp/game.bmp" | nc localhost 6543
```

## Debugging Tips

### macOS SIGBUS on Startup

If you get a crash dialog mentioning "reopening windows", clear macOS saved state:
```bash
rm -rf ~/Library/Saved\ Application\ State/*koncepcja*
```

### Running with lldb

```bash
# Basic debugging
lldb ./koncepcja
(lldb) run game.dsk

# With breakpoint on crash
lldb ./koncepcja -o "run game.dsk" -k "bt all" -k "quit"
```

### Checking Tape File Validity

Malformed CDT/TZX files can crash older versions. Test files are rejected gracefully:
```bash
./koncepcja malformed.cdt 2>&1 | grep ERROR
# ERROR   src/slotshandler.cpp:1219 - CDT block 0xff extends past end of file
```

### Logging

The emulator uses LOG_DEBUG/LOG_INFO/LOG_ERROR macros. Key files with logging:
- `slotshandler.cpp` - File loading errors
- `psg.cpp` - Audio timing info
- `keyboard.cpp` - Key mapping issues

### DevTools

Press **F12** or send `devtools` via IPC to open the developer tools:
- **Z80 tab**: Registers, flags, disassembly
- **Memory tab**: Hex viewer, search, poke
- **Breakpoints**: Add/remove execution breakpoints
- **Watch**: Memory watchpoints

### Function Keys

| Key | Action |
|-----|--------|
| F1 | Show menu (pause) |
| F2 | Toggle fullscreen |
| F3 | Screenshot |
| F4 | Tape play/pause |
| F5 | Reset CPC |
| F6 | Multiface II stop |
| F7 | Toggle joystick emulation |
| F8 | Toggle FPS display |
| F9 | Toggle speed limiter |
| F10 | Exit |
| F12 | Toggle DevTools |
| Shift+F1 | Virtual keyboard |
| Shift+F3 | Save snapshot |

## Configuration

Config file locations (in order of precedence):
1. `-c/--cfg_file=<path>` argument
2. `$CWD/koncepcja.cfg`
3. `$XDG_CONFIG_HOME/koncepcja.cfg` (or `~/.config/koncepcja.cfg`)
4. `~/.koncepcja.cfg`
5. `/etc/koncepcja.cfg`

### Key Config Options

```ini
[system]
model=2           # 0=464, 1=664, 2=6128, 3=6128+
ram_size=128      # RAM in KB
speed=4           # Clock speed MHz

[video]
scr_scale=2       # Window scale factor
scr_style=1       # Rendering style (0-11)

[sound]
snd_enabled=1
snd_playback_rate=2  # 0=11025, 1=22050, 2=44100, 3=48000, 4=96000
```

## Code Conventions

- Use `byte` (uint8_t), `word` (uint16_t), `dword` (uint32_t) types
- Z80 memory access: `z80_read_mem(addr)`, `z80_write_mem(addr, val)`
- Pause/resume: `cpc_pause()`, `cpc_resume()`
- Reset: `emulator_reset()`
- ImGui state is in global `imgui_state` struct

## Landing the Plane (Session Completion)

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd sync
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds
