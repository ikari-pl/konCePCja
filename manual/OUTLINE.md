# konCePCja Manual — Content Outline

This is the master table of contents for the konCePCja User Manual.
Each entry is a planned section. Sections marked **[UNDOCUMENTED]** exist as
source code but lack any public-facing documentation — the manual author needs
to investigate the feature and write it from scratch.

Audience: end users, including technically inclined power users. Not AI agents.

---

## Front Matter

- Title page
- Copyright and licence
- About this manual
- About Amstrad CPC emulation
- Version history / edition notice

---

## Chapter 1 — Getting Started

### 1.1 What is konCePCja?
- Brief description: CPC emulator based on Caprice32, SDL3, Dear ImGui
- Supported platforms: macOS, Linux, Windows (via MINGW)
- What makes it different from other CPC emulators
- Supported CPC models: CPC 464, CPC 664, CPC 6128, CPC 6128+

### 1.2 System Requirements
- macOS requirements (version, Apple Silicon vs Intel)
- Linux requirements
- Windows requirements
- Minimum hardware (RAM, GPU for OpenGL)

### 1.3 Installation

#### 1.3.1 macOS
- Homebrew dependencies: freetype, zlib, libpng, cmake
- Building the vendored SDL3 submodule
- Building konCePCja: `make -j$(sysctl -n hw.ncpu) ARCH=macos`
- Wrapper script: `scripts/build-macos.sh`
- First launch

#### 1.3.2 Linux
- APT dependencies: g++, make, pkg-config, cmake, libfreetype6-dev, zlib1g-dev, libpng-dev, libgl-dev
- Building the vendored SDL3 or using system SDL3
- Building konCePCja: `make APP_PATH="$PWD" -j$(nproc)`
- Debian/Ubuntu package build

#### 1.3.3 Windows
- MSYS2 / MinGW setup
- Building with MINGW32 / MINGW64

### 1.4 First Launch
- Default window layout
- Loading a ROM (built-in BASIC ROM vs custom)
- The CPC 6128 BASIC prompt: what you are looking at
- Where to find disk images and software

### 1.5 Loading Software

#### 1.5.1 Disk images (DSK)
- Drag-and-drop onto the window
- File → Load, or command line
- Running a disk: `run"disk`

#### 1.5.2 Tape files (CDT/VOC)
- Inserting a tape
- `|TAPE` + `LOAD""` + play
- Tape play/pause (F4)

#### 1.5.3 Cartridge files (CPR)
- CPC 6128+ cartridges only
- Inserting and running

#### 1.5.4 Snapshots (SNA)
- Instant-state resume
- Load via menu or command line

#### 1.5.5 ZIP files
- konCePCja can open ZIP archives containing DSK/CDT/SNA files **[UNDOCUMENTED]**
- Behaviour when ZIP contains multiple compatible files

---

## Chapter 2 — The konCePCja Interface

### 2.1 The Main Window
- The emulated CPC screen
- Window scaling and aspect ratio
- Fullscreen mode (F2)

### 2.2 The Menu Bar (F1)
- Overview of menus: File, Machine, Peripherals, View, Help
- Pausing on menu open
- Returning to emulation

### 2.3 Function Keys

| Key | Action |
|-----|--------|
| F1 | Show menu (pauses emulation) |
| F2 | Toggle fullscreen |
| F3 | Take screenshot (BMP) |
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

### 2.4 Virtual Keyboard (Shift+F1)
- On-screen CPC keyboard overlay
- Clicking keys vs physical keyboard
- Useful for keys not on modern keyboard layouts

### 2.5 OSD (On-Screen Display)
- Speed indicator, FPS counter
- Tape status, disk activity
- Toggling the OSD elements

### 2.6 Screenshots (F3)
- Default output path
- BMP format
- Custom path via IPC command

### 2.7 Command Palette **[UNDOCUMENTED]**
- Quick-access to all emulator actions via keyboard shortcut
- How to invoke
- Searchable command list

### 2.8 Configuration Profiles **[UNDOCUMENTED]**
- Saving and loading named configurations
- What is included in a profile (machine model, peripherals, display settings)
- Switching profiles without restarting

---

## Chapter 3 — Disk, Tape, and Snapshot Management

### 3.1 Disk Drives

#### 3.1.1 Drive A and Drive B
- Standard DSK format (Amstrad/PCW)
- Extended DSK format (EDSK)
- IPF format (copy-protected disks) **[UNDOCUMENTED]**
- Read-only mode

#### 3.1.2 Disk operations
- Creating a new blank disk
- Writing/ejecting a disk
- Copying sectors between disks (Disc Tools in DevTools)

#### 3.1.3 Drive and seek sounds
- Procedurally generated FDC motor hum
- Head seek click effects
- Enabling and disabling drive sounds

### 3.2 Tape

#### 3.2.1 Tape formats
- CDT (CPC-native format)
- TZX (Spectrum format, also supported)
- VOC (raw audio)

#### 3.2.2 Tape operations
- Play/pause (F4)
- Rewind
- Tape loading sounds (hiss effect)
- Tape counter / block display

### 3.3 Snapshots

#### 3.3.1 Saving snapshots (Shift+F3)
- SNA format: full Z80 state, RAM, CRTC registers, PSG state
- Save to path

#### 3.3.2 Loading snapshots
- Load via menu or command line argument
- State restored immediately

#### 3.3.3 Snapshot via IPC
- `snapshot save <path>` and `snapshot load <path>` commands

---

## Chapter 4 — Configuration

### 4.1 Config File Location
- Search order: `--cfg_file` argument, `$CWD/koncepcja.cfg`, `$XDG_CONFIG_HOME/...`, `~/.koncepcja.cfg`, `/etc/koncepcja.cfg`

### 4.2 Command Line Arguments

| Argument | Description |
|----------|-------------|
| `-a` / `--autocmd=<cmd>` | Execute BASIC command after CPC boots |
| `-c` / `--cfg_file=<file>` | Use custom config file |
| `-h` / `--help` | Show help and exit |
| `-i` / `--inject=<file>` | Inject binary into memory after boot |
| `-o` / `--offset=<address>` | Injection address (default: `0x6000`) |
| `-O` / `--override=<opt=val>` | Override config option (repeatable) |
| `-s` / `--sym_file=<file>` | Load symbols file for disassembly |
| `-V` / `--version` | Show version and exit |
| `-v` / `--verbose` | Verbose logging |
| (positional) | Slot files: `.dsk`, `.cdt`, `.voc`, `.cpr`, `.sna` |

Examples:
```
koncepcja game.dsk
koncepcja -a 'run"game' game.dsk
koncepcja -i loader.bin -o 0x4000
koncepcja -O system.model=3 game.dsk
```

### 4.3 Config File Reference

#### 4.3.1 [system]

| Key | Values | Description |
|-----|--------|-------------|
| `model` | 0=464, 1=664, 2=6128, 3=6128+ | CPC model to emulate |
| `ram_size` | 64, 128 (KB) | RAM size |
| `speed` | MHz (default 4) | CPU clock speed |

#### 4.3.2 [video]

| Key | Values | Description |
|-----|--------|-------------|
| `scr_scale` | 1–4 | Window scale factor |
| `scr_style` | 0–11 | Rendering style (scanlines, palette, etc.) |
| `vsync` | 0, 1 | VSync on main window (0=MAILBOX/IMMEDIATE; viewports always VSYNC) |

#### 4.3.3 [sound]

| Key | Values | Description |
|-----|--------|-------------|
| `snd_enabled` | 0, 1 | Enable/disable audio |
| `snd_playback_rate` | 0–4 | Sample rate: 0=11025, 1=22050, 2=44100, 3=48000, 4=96000 |

#### 4.3.4 [peripheral]

| Key | Description |
|-----|-------------|
| `m4_http_port` | M4 Board HTTP server port (default 8080) |
| `m4_bind_ip` | HTTP server bind address (default 127.0.0.1) |
| `m4_port_map_N` | Port forwarding: `cpc_port:host_port:user_override` |

### 4.4 Overriding Config at Command Line
- `-O system.model=3` overrides any config file value
- Repeatable: `-O system.model=3 -O video.scr_scale=2`

---

## Chapter 5 — Emulated Hardware

This chapter describes the hardware components konCePCja emulates. For most users
this is background reading; for developers using the DevTools and IPC, understanding
the hardware is essential.

### 5.1 Z80A CPU
- 4 MHz clock
- Full Z80 instruction set
- Breakpoint and watchpoint support (see Chapter 7)
- Single-step execution via DevTools or IPC

### 5.2 Gate Array
- Palette: 27-colour hardware palette, 32 colour registers
- Screen modes: 0 (160×200, 16 colours), 1 (320×200, 4 colours), 2 (640×200, 2 colours)
- ROM banking: lower ROM (BASIC/OS), upper ROM (AMSDOS / other)
- Interrupt generation (50 Hz vertical sync)

### 5.3 CRTC (Cathode Ray Tube Controller)
- Four CRTC types emulated: Type 0 (HD6845S), Type 1 (UM6845R), Type 2 (MC6845), Type 3 (AMS40489)
- Programmable horizontal/vertical timing
- Selecting CRTC type via config

### 5.4 PPI 8255 (Programmable Peripheral Interface)
- Port A: PSG data bus
- Port B: tape input, CPC model identifier, VSYNC signal
- Port C: keyboard row select, PSG control, tape motor

### 5.5 PSG AY-3-8912 (Sound Chip)
- 3-channel square wave tone
- Noise generator
- Envelope generator (8 shapes)
- Amplitude control per channel

### 5.6 FDC uPD765A (Floppy Disk Controller)
- Standard and extended DSK formats
- Two virtual drives (A: and B:)
- DMA-like data transfer to Z80 memory

### 5.7 ASIC (CPC 6128+ only)
- DMA sound (3 additional channels)
- Programmable raster interrupts
- Hardware sprites (16 sprites, 16×16 pixels)
- Analogue joystick ports
- Cartridge port (CPR files)
- ASIC debug view in DevTools

---

## Chapter 6 — Peripheral Expansions

konCePCja emulates a wide range of hardware expansions that were available for
the real CPC. All peripherals can be enabled/disabled in the Peripherals menu.

### 6.1 Digiblaster (Printer Port DAC)
- 8-bit DAC on the parallel printer port
- Reads `CPC.printer_port` value via 256-level lookup table (`Level_PP[]`)
- Mixed into PSG audio output
- Use cases: sampled audio playback software

### 6.2 AmDrum (Cheetah Drum Machine Interface)
- 8-bit DAC on I/O port `&FFxx` (the "uncontested" upper-address space)
- Same mixing pattern as Digiblaster via `Level_AmDrum[]`
- Enables drum machine and sample replay software

### 6.3 Dobbertin SmartWatch (DS1216 Real-Time Clock)
- Dallas DS1216 phantom RTC in upper ROM socket
- Does not occupy any address space — intercepts ROM reads
- 64-bit recognition pattern sent via address lines A0 (data) and A2 (mode)
- Returns BCD-encoded current host system time via data bit D0
- Compatible with all CPC BASIC/CP/M clock software using this hardware

### 6.4 AMX Mouse
- Joystick-port mouse on keyboard matrix row 9
- Direction bits pulse LOW for one "mickey" per deselect/reselect cycle of row 9
- Monostable reset tracking

### 6.5 Symbiface II

#### 6.5.1 IDE (ATA PIO) — ports `&FD06`–`&FD0F`
- Standard ATA register file backed by raw `.img` files
- Supports READ SECTORS, WRITE SECTORS, IDENTIFY DEVICE commands
- Appears as a hard disk to CPC software

#### 6.5.2 RTC (DS12887) — ports `&FD14`/`&FD15`
- 14 time registers in BCD (from host clock)
- 50 bytes CMOS NVRAM

#### 6.5.3 PS/2 Mouse — ports `&FD10`/`&FD18`
- Multiplexed FIFO protocol
- Status byte with 2-bit mode (X/Y offset, buttons) + 6-bit payload

### 6.6 M4 Board (Virtual Filesystem)
See Chapter 11 for full coverage.
- Virtual SD card backed by a host directory
- OUTs to `&FE00` accumulate command bytes; OUT to `&FC00` triggers execution
- Response written to ROM overlay at `&E800`
- HTTP server for web-based file management (Chapter 11)
- Compatible with `cpcxfer` and the M4 Board Android app

### 6.7 Drive and Tape Sounds
- Procedurally generated audio effects
- FDC motor hum (variable pitch with rotational speed)
- Head seek clicks (timing matches FDC step commands)
- Tape loading hiss
- Enabling/disabling in Peripherals menu

### 6.8 Multiface II (ROM debugging interface)
- `|SNAP` command saves current machine state
- Stop button (F6) triggers Multiface interrupt
- Provides machine-code monitor at interrupt entry

### 6.9 Amstrad Magnum Phaser (Light Gun)
- Intercepts CRTC register reads to compute beam position
- Reports X/Y screen position via joystick port
- Compatible with light gun games expecting Magnum Phaser protocol

### 6.10 Serial Interface **[UNDOCUMENTED]**
- Source: `src/serial_interface.cpp`
- Serial COM port emulation
- Details to be investigated and documented

### 6.11 HP-GL Plotter Emulation **[UNDOCUMENTED]**
- Source: `src/plotter.cpp`
- Emulates an HP-GL plotter connected to the CPC printer port
- Vector output: can export plot to SVG or PNG
- For use with CPC plotter/CAD software

### 6.12 Silicon Disc **[UNDOCUMENTED]**
- Source: `src/silicon_disc.cpp`
- RAM-backed virtual disc
- Appears as a drive to CPC software
- Details to be investigated

---

## Chapter 7 — Developer Tools (DevTools)

Press **F12** to open the DevTools window. DevTools is a suite of debug and
inspection windows for examining and modifying the emulated machine's state.

In headless mode (SDL_VIDEODRIVER=dummy), DevTools is not available.

### 7.1 Overview
- The DevTools toolbar — 17 sub-windows accessible from the toolbar
- Auto-opening windows on first F12 press: registers, disassembly, memory hex

### 7.2 Z80 Registers
- All general-purpose registers: AF, BC, DE, HL, IX, IY, SP, PC
- Alternate register set: AF', BC', DE', HL'
- Flags: S, Z, H, P/V, N, C
- Live update during execution
- Editing registers directly

### 7.3 Disassembly
- Real-time Z80 disassembly at current PC
- Follow PC mode
- Navigate to arbitrary address
- Symbol labels in disassembly (if symbol file loaded)

### 7.4 Memory Hex Viewer
- 256 KB address space view
- Highlight changes since last frame
- Navigate to address, search bytes
- Edit bytes inline (poke)
- ROM regions shown with visual distinction

### 7.5 Stack
- Current stack contents decoded as return addresses
- Symbol labels on stack entries (if symbols loaded)

### 7.6 Breakpoints
- Set execution breakpoints by address
- I/O breakpoints (port read/write) **[UNDOCUMENTED — IOBP]**
- Watchpoints (memory read/write at address)
- Breakpoint list: enable/disable individual breakpoints
- Clear all breakpoints

### 7.7 Symbols
- Load a symbol file (`-s` / `--sym_file`)
- Symbol list view: name, address, type
- Symbol navigation: jump to address from symbol list

### 7.8 Silicon Disc **[UNDOCUMENTED]**
- DevTools view of the silicon disc contents
- Load/save silicon disc image

### 7.9 ASIC Debug (CPC 6128+ only)
- ASIC register dump
- Sprite viewer: 16 sprites with position/palette
- DMA channel states
- Programmable raster interrupt status

### 7.10 Disc Tools
- Sector-level browser for loaded DSK images
- Read/write individual sectors
- Format track
- Copy sectors between drives

### 7.11 Data Areas
- Named memory regions from symbol file
- Jump to data area in hex viewer

### 7.12 Disassembly Export
- Export a range of disassembly to text file
- Format options: plain Z80, annotated with symbols

### 7.13 Session Recording Controls
- Start/stop recording (see Chapter 12)
- Recording format selection
- Playback controls

### 7.14 Graphics Finder (gfx_finder) **[UNDOCUMENTED]**
- Source: `src/gfx_finder.cpp`
- Search video RAM for specific graphic patterns
- Useful for finding sprites, screen buffers, font data
- Visual result display in hex viewer

### 7.15 Video State
- CRTC register dump (R0–R17)
- Gate array register dump
- Current screen mode, palette
- Raster position, VSYNC/HSYNC signals

### 7.16 Audio State
- PSG register dump (R0–R13)
- Per-channel: tone, volume, envelope settings
- Active Digiblaster / AmDrum levels

### 7.17 Memory Search
- Source: `src/search_engine.cpp`
- Search entire memory for byte sequences, words, or text strings **[UNDOCUMENTED]**
- Track changes between scans (useful for finding game values)

### 7.18 Pokes **[UNDOCUMENTED]**
- Source: `src/pokes.cpp`
- Named poke database: store address+value pairs
- Apply single pokes or poke sets
- Import/export poke files

---

## Chapter 8 — Z80 Assembler

konCePCja includes an integrated two-pass Z80 assembler, accessible from DevTools
and controllable via IPC.

### 8.1 Overview
- Two-pass assembler: pass 1 builds symbol table and sizes, pass 2 encodes and writes to memory
- Full Z80 instruction set: 1268 opcodes in 7 prefix groups (NONE, ED, CB, DD, DDCB, FD, FDCB)
- No macros, conditionals, or repeat directives in V1

### 8.2 Using the Assembler in DevTools

#### 8.2.1 Source editor
- Typing Z80 assembly source
- Saving/loading source files

#### 8.2.2 Assembling
- "Assemble" button runs both passes
- Errors shown with line numbers
- On success, writes encoded bytes to Z80 memory at the origin address

#### 8.2.3 Symbol table
- Labels defined in source become symbols
- Visible in the Symbols DevTools window

### 8.3 Assembly Language Syntax

#### 8.3.1 Instructions
- Standard Z80 mnemonic syntax: `LD A,B`, `JP NZ,loop`, `PUSH HL`
- Uppercase mnemonics required

#### 8.3.2 Labels and symbols
- Labels end with `:`: `loop:`
- Local labels: `.local_label:`
- Forward references supported

#### 8.3.3 Directives
- `ORG <address>` — set origin address
- `DB <byte>[,...]` — define bytes
- `DW <word>[,...]` — define words
- `DS <count>` — reserve bytes (zero-filled)
- `EQU <value>` — define constant

#### 8.3.4 Expressions
- Decimal, hex (`&FC00` or `0xFC00`), binary (`%10110101`)
- `+`, `-`, `*`, `/`, `AND`, `OR`, `XOR`, `NOT`, `SHL`, `SHR`
- `&` is context-sensitive: hex prefix when preceding a digit, bitwise AND otherwise
- `%` is context-sensitive: binary prefix when preceding 0/1, modulo otherwise

#### 8.3.5 Comments
- Semicolon: `; this is a comment`

### 8.4 Assembler via IPC

| IPC Command | Description |
|-------------|-------------|
| `asm text <source>` | Set assembler source text |
| `asm load <path>` | Load source from file |
| `asm assemble` | Run assembler (both passes) |
| `asm errors` | Get list of assembly errors |
| `asm symbols` | Dump assembled symbol table |
| `asm source` | Get current source text |

---

## Chapter 9 — IPC Debugging Interface

konCePCja runs a TCP server on **port 6543** (localhost only) for external control
and debugging.

### 9.1 Connecting

```bash
# netcat (one command per invocation)
echo "ping" | nc localhost 6543

# socat (line-buffered, interactive)
socat - TCP:localhost:6543
```

### 9.2 Protocol
- One command per TCP connection (server closes after response)
- Commands are plain ASCII, one per line
- Responses: `OK ...` on success, `ERR ...` on failure

### 9.3 Command Reference

#### 9.3.1 Control

| Command | Response | Description |
|---------|----------|-------------|
| `ping` | `OK pong` | Test connectivity |
| `version` | `OK koncepcja-N.N port=6543` | Get version |
| `help` | `OK commands: ...` | List all commands |
| `pause` | `OK` | Pause emulation |
| `run` | `OK` | Resume emulation |
| `reset` | `OK` | Hard-reset the CPC |

#### 9.3.2 CPU State

| Command | Response | Description |
|---------|----------|-------------|
| `regs` | `OK A=xx F=xx ...` | Dump all Z80 registers |
| `reg get <R>` | `OK <value>` | Get single register (A, BC, HL, PC, SP, ...) |
| `reg set <R> <V>` | `OK` | Set register value |

#### 9.3.3 Memory

| Command | Description |
|---------|-------------|
| `mem read <addr> <len> [ascii]` | Read bytes; optional ASCII column |
| `mem write <addr> <hexstring>` | Write bytes |

#### 9.3.4 Breakpoints

| Command | Description |
|---------|-------------|
| `bp add <addr>` | Add breakpoint |
| `bp del <addr>` | Remove breakpoint |
| `bp list` | List all breakpoints |
| `bp clear` | Clear all breakpoints |

#### 9.3.5 Execution

| Command | Description |
|---------|-------------|
| `step [n]` | Step N instructions (default 1) |
| `disasm <addr> <count>` | Disassemble N instructions at address |

#### 9.3.6 Wait / Synchronisation

| Command | Description |
|---------|-------------|
| `wait pc <addr> [timeout_ms]` | Block until PC reaches address |
| `wait mem <addr> <val> [mask] [timeout_ms]` | Block until memory matches |
| `wait bp [timeout_ms]` | Block until any breakpoint fires |
| `wait vbl <n> [timeout_ms]` | Block until N vertical blanks pass |

#### 9.3.7 Files and State

| Command | Description |
|---------|-------------|
| `load <path>` | Load file (DSK, SNA, CPR, binary) |
| `screenshot [path]` | Save BMP screenshot |
| `snapshot save <path>` | Save SNA snapshot |
| `snapshot load <path>` | Load SNA snapshot |
| `devtools` | Open DevTools window (GUI mode only) |

#### 9.3.8 Keyboard and Joystick Input

| Command | Description |
|---------|-------------|
| `input keydown <key>` | Press a CPC key (by name) |
| `input keyup <key>` | Release a CPC key |
| `input key <key>` | Press and release a CPC key |
| `input type <string>` | Type a string character by character |
| `input joy <port> <mask>` | Set joystick port state |
| `input autotype <string>` | Type using AutoTypeQueue (scan-synced) |

AutoTypeQueue tokens for special keys:
```
~RETURN~   ~ESC~   ~SHIFT~   ~CTRL~   ~DEL~
~UP~   ~DOWN~   ~LEFT~   ~RIGHT~
~F1~ through ~F10~
~CLR~   ~TAB~   ~CAPS~
```

#### 9.3.9 Z80 Assembler

| Command | Description |
|---------|-------------|
| `asm text <source>` | Set assembler source |
| `asm load <path>` | Load source from file |
| `asm assemble` | Assemble |
| `asm errors` | Get error list |
| `asm symbols` | Get symbol table |
| `asm source` | Get current source |

### 9.4 Scripting Examples

#### Wait for game to reach entry point
```bash
#!/bin/bash
echo "load game.dsk" | nc localhost 6543
sleep 1
echo "run" | nc localhost 6543
echo "wait pc 0xC000 30000" | nc localhost 6543
echo "screenshot /tmp/game.bmp" | nc localhost 6543
```

#### Read memory region
```bash
echo "mem read 0xBE00 64 ascii" | nc localhost 6543
```

#### Inject Z80 code and run
```bash
echo "pause" | nc localhost 6543
echo "mem write 0x8000 213E4001C9" | nc localhost 6543   # LD A,1 / LD (0x4001),A / RET
echo "reg set PC 0x8000" | nc localhost 6543
echo "step 3" | nc localhost 6543
echo "mem read 0x4001 1" | nc localhost 6543
```

### 9.5 Automated Testing with ipc_harness.py
- Location: `test/integrated/ipc_harness.py`
- `KoncepcjaIPC` class: one TCP connection per command
- `EmulatorRunner` context manager: launches and tears down the emulator
- Running tests: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./koncepcja & python3 test/integrated/ipc_harness.py`
- Headless vs GUI mode detection: `ipc.is_threaded()` (sends `devtools` — fails in headless)

### 9.6 Headless Mode
- Trigger: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./koncepcja`
- No window, no OpenGL — runs single-threaded
- IPC protocol identical to GUI mode
- DevTools unavailable in headless

---

## Chapter 10 — Telnet Console

konCePCja runs a plain TCP text console on **port 6544** (IPC+1). It mirrors
everything the CPC prints to screen and accepts keyboard input — like a remote
terminal for the emulated CPC.

### 10.1 Connecting

```bash
nc localhost 6544
```

A new connection replaces any existing connection. If port 6544 is busy, konCePCja
probes forward up to port 6554.

### 10.2 CPC Output
- Hooks `TXT_OUTPUT` (firmware vector `&BB5A`) — intercepts character output
- CP/M BDOS hook at PC=`0x0005`, function C=2 (C_WRITE)
- Characters pushed to a lock-free ring buffer, drained to TCP client each frame
- Both firmware and CP/M paths work simultaneously

### 10.3 Keyboard Input
- Bytes received from TCP are fed to `AutoTypeQueue` each frame
- ANSI escape sequences mapped to CPC keys:

| Terminal input | CPC key |
|---------------|---------|
| `\x1b[A` | Cursor Up |
| `\x1b[B` | Cursor Down |
| `\x1b[C` | Cursor Right |
| `\x1b[D` | Cursor Left |
| `\x1b` (alone) | ESC |
| `\x7f` / `\x08` | DEL |
| `\t` | TAB |
| `\x03` (Ctrl+C) | ESC |
| `\r` or `\n` | RETURN |
| Printable ASCII | Matching key |

### 10.4 Use Cases
- Running CP/M programs remotely without a GUI
- Automated BASIC interaction (scripting via pipe)
- Remote monitoring of CPC text output

### 10.5 Example: Remote BASIC Session

```bash
# Connect and type a BASIC program
echo -e '10 PRINT "Hello"\r20 GOTO 10\rRUN\r' | nc localhost 6544
```

---

## Chapter 11 — M4 Board and Virtual Filesystem

The M4 Board expansion gives CPC software access to a virtual SD card backed by a
host filesystem directory. An embedded HTTP server (port 8080) provides a
web-based file manager and `cpcxfer` / Android app compatibility.

### 11.1 Overview
- Virtual SD card: any host directory acts as the SD card root
- Path traversal protection: `..` and absolute paths are rejected
- The M4 Board protocol uses I/O ports: OUTs to `&FE00` accumulate command bytes, OUT to `&FC00` executes
- Response written to ROM overlay at `&E800`

### 11.2 HTTP File Manager

Access at `http://127.0.0.1:8080/`:

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Web file browser (single-page app) |
| GET | `/config.cgi?ls=<path>` | Directory listing |
| GET | `/config.cgi?cd=<path>` | Change directory |
| GET | `/config.cgi?run2=<path>` | Remote-run file on CPC |
| GET | `/config.cgi?rm=<path>` | Delete file |
| GET | `/config.cgi?mkdir=<path>` | Create directory |
| GET | `/sd/<path>` | Download file |
| GET | `/sd/m4/dir.txt` | M4-format directory listing |
| GET | `/status` | JSON status |
| POST | `/` | Upload file (multipart/form-data) |
| POST | `/reset` | Reset CPC |
| POST | `/pause` | Toggle pause |

### 11.3 IPC Commands for M4

```
m4 http status      — check HTTP server status
m4 http start       — start HTTP server
m4 http stop        — stop HTTP server
m4 ports            — list port mappings
m4 port set 80 8080 — map CPC port 80 → host 8080
m4 port del 80      — remove mapping
```

### 11.4 Configuration

```ini
[peripheral]
m4_http_port=8080
m4_bind_ip=127.0.0.1
m4_port_map_0=80:8080:1
```

### 11.5 Compatibility
- Compatible with `cpcxfer` command-line tool
- Compatible with M4 Board Android app
- Works with CPC software that supports M4 Board (file managers, BASIC loaders)

---

## Chapter 12 — Recording

konCePCja can record the emulated CPC session in multiple formats.

### 12.1 AVI Video Recording **[UNDOCUMENTED]**
- Source: `src/avi_recorder.cpp`
- Records screen output to AVI file
- Frame rate matches CPC video (50 Hz)
- Start/stop from DevTools recording controls or IPC

### 12.2 GIF Recording **[UNDOCUMENTED]**
- Source: `src/gif_recorder.cpp`
- Records short animated GIF clips
- Useful for sharing demos and screenshots
- Configurable frame limit and speed

### 12.3 YM Audio Recording **[UNDOCUMENTED]**
- Source: `src/ym_recorder.cpp`
- Records PSG audio register stream to YM format
- YM files are playable on real hardware and in emulators
- Does not capture Digiblaster or AmDrum audio

### 12.4 Session Recording and Playback **[UNDOCUMENTED]**
- Source: `src/session_recording.cpp`
- Records all input events and timing
- Deterministic playback: replay produces identical screen output
- Used for regression testing and demonstration

---

## Chapter 13 — Troubleshooting

### 13.1 macOS Crash on Startup (SIGBUS)
If a crash dialog appears on launch referencing "reopening windows":
```bash
rm -rf ~/Library/Saved\ Application\ State/*koncepcja*
```

### 13.2 Debugging with lldb
```bash
lldb ./koncepcja
(lldb) run game.dsk

# Capture backtrace on crash
lldb ./koncepcja -o "run game.dsk" -k "bt all" -k "quit"
```

### 13.3 Tape File Errors
Malformed CDT/TZX files produce a clear error:
```
ERROR   src/slotshandler.cpp:1219 - CDT block 0xff extends past end of file
```

### 13.4 Sound Issues
- Check `snd_enabled=1` in config
- Check `snd_playback_rate` matches system audio capabilities
- On Linux, ensure the audio device is not in use by another process

### 13.5 IPC Port Already in Use
If port 6543 is occupied, check:
```bash
lsof -i :6543
```
Kill any stale `koncepcja` process, or change the IPC port in config.

### 13.6 Telnet Console Port Collision
konCePCja probes ports 6544–6554. Check:
```bash
lsof -i :6544
```

### 13.7 M4 HTTP Server Not Starting
- Check `m4_http_port` is not in use
- On macOS, `m4_bind_ip=127.0.0.2` works without root (loopback alias)
- Default bind is `127.0.0.1`

### 13.8 OpenGL Init Failure in Headless Mode
SDL_VIDEODRIVER=dummy triggers headless fallback automatically — OpenGL failure is
expected and not an error. IPC functions normally.

### 13.9 Logging
Enable verbose logging with `-v` / `--verbose`. Key log sources:
- `slotshandler.cpp` — file loading errors
- `psg.cpp` — audio timing
- `keyboard.cpp` — key mapping

---

## Appendix A — Key Reference

### A.1 CPC Keyboard Layout
(Figure: keyboard-layout.svg)

### A.2 CPC Key Names for IPC Input
Full list of key names accepted by `input keydown`, `input key`, and autotype:
- All printable ASCII
- Special keys: RETURN, ESC, SHIFT, CTRL, CAPS LOCK, DEL, TAB, CLR
- Cursor keys: UP, DOWN, LEFT, RIGHT
- Function keys: F1–F10
- Numeric keypad keys

### A.3 konCePCja Function Key Summary

| Key | Action |
|-----|--------|
| F1 | Menu |
| F2 | Fullscreen |
| F3 | Screenshot |
| F4 | Tape play/pause |
| F5 | Reset CPC |
| F6 | Multiface II |
| F7 | Joystick toggle |
| F8 | FPS display |
| F9 | Speed limiter |
| F10 | Exit |
| F12 | DevTools |
| Shift+F1 | Virtual keyboard |
| Shift+F3 | Save snapshot |

---

## Appendix B — IPC Command Quick Reference

One-page table of all IPC commands with syntax and brief description.
(Generated from Chapter 9 command tables)

---

## Appendix C — Configuration File Reference

Full alphabetical list of all configuration keys, their types, defaults, and valid
values.

---

## Appendix D — Supported File Formats

| Extension | Format | Role |
|-----------|--------|------|
| `.dsk` | Standard Amstrad DSK | Floppy disk image |
| `.edsk` | Extended DSK | Copy-protected disk image |
| `.ipf` | IPF (SPS format) | Copy-protected disk image |
| `.cdt` | CPC Tape format | Tape image |
| `.tzx` | ZX Spectrum Tape format | Tape image (also supported) |
| `.voc` | Creative Voice audio | Raw tape audio |
| `.sna` | Snapshot | Full machine state |
| `.cpr` | Cartridge ROM | CPC 6128+ cartridge |
| `.img` | Raw disk image | Symbiface II IDE |
| `.bin` | Binary | Code/data injection (`-i`) |
| `.sym` | Symbol file | Z80 debugger labels |
| `.zip` | ZIP archive | Container for the above |
| `.ym` | YM audio | PSG register stream (output) |
| `.avi` | AVI video | Screen recording (output) |
| `.gif` | GIF animation | Screen recording (output) |

---

## Appendix E — Hardware Specifications (Emulated)

| Component | Specification |
|-----------|--------------|
| CPU | Z80A, 4 MHz |
| RAM | 64 KB (CPC 464/664) / 128 KB (CPC 6128/6128+) |
| ROM | 32 KB BASIC/OS + 16 KB AMSDOS upper ROM |
| Video | Gate Array + CRTC; 3 screen modes; 27-colour palette |
| Sound | AY-3-8912; 3 channels + noise + envelope |
| Disk | uPD765A FDC; 2 drives; 180 KB / 720 KB per disk |
| Tape | 1000 baud / 2000 baud CDT/TZX |
| ASIC (6128+) | DMA sound, hardware sprites, programmable raster |

---

## Appendix F — Glossary

- **AMSDOS** — Amstrad Disk Operating System; handles disk file I/O from BASIC
- **ASIC** — Application-Specific Integrated Circuit; the 6128+ chip combining Gate Array + CRTC + extras
- **AutoTypeQueue** — konCePCja's scan-synchronised keyboard injection queue
- **CDT** — CPC Data Tape format; stores tape blocks with timing information
- **CRTC** — Cathode Ray Tube Controller; generates video timing signals
- **DSK** — Amstrad disk image format; sector-by-sector floppy disk image
- **FDC** — Floppy Disk Controller; the uPD765A chip managing disk I/O
- **Gate Array** — Amstrad custom chip managing palette, ROM banking, and interrupts
- **IPC** — Inter-Process Communication; konCePCja's TCP control protocol on port 6543
- **M4 Board** — Third-party CPC expansion providing SD card storage
- **PSG** — Programmable Sound Generator; the AY-3-8912 audio chip
- **PPI** — Programmable Peripheral Interface; the 8255 chip managing keyboard and tape
- **RSX** — Resident System eXtension; commands with a `|` prefix extending BASIC
- **SNA** — Snapshot format storing complete Z80 machine state
- **Symbiface II** — Third-party CPC expansion with IDE, RTC, and PS/2 mouse
- **YM** — PSG register dump format used for chiptune archiving
- **Z80** — Zilog Z80 8-bit CPU, the heart of the Amstrad CPC
