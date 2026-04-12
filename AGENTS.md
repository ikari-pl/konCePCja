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
├── telnet_console.cpp # TCP text console — mirrors CPC output, injects keyboard
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
  - **IDE (ATA PIO)** at `&FD06-&FD0F` — Standard ATA register file backed by raw `.img` files. Supports READ/WRITE SECTORS and IDENTIFY DEVICE
  - **RTC (DS12887)** at `&FD14`/`&FD15` — 14 time registers (BCD from host clock) + 50 bytes CMOS NVRAM
  - **PS/2 Mouse** at `&FD10`/`&FD18` — Multiplexed FIFO protocol: status byte with 2-bit mode (X/Y offset, buttons) + 6-bit payload
- **M4 Board** — Virtual filesystem expansion via command/response protocol. OUTs to `&FE00` accumulate command bytes, OUT to `&FC00` triggers execution. Response written to ROM overlay at `&E800`. Backs virtual SD card with a host directory (path traversal protected). Includes embedded HTTP server on port 8080 for web-based file management, compatible with `cpcxfer` and the M4 Board Android app
- **Drive/Tape Sounds** — Procedurally generated audio effects for FDC motor hum, head seek clicks, and tape loading hiss, mixed into the PSG audio output
- **Multiface II** — ROM-based debugging interface (original Caprice32 implementation)
- **Amstrad Magnum Phaser** — Light gun via CRTC register intercept

## Building

```bash
# macOS (preferred — use the wrapper script to avoid command-substitution prompts)
scripts/build-macos.sh

# macOS (manual)
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
| `version` | Get version | `version` → `OK koncepcja-0.2 port=6543` |
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

### Automated IPC Testing

Automated tests live in `test/integrated/ipc_harness.py`.  Run them against a
running emulator:

```bash
# Start emulator in the background (headless)
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./koncepcja &
python3 test/integrated/ipc_harness.py
kill %1
```

The harness provides two classes:

- **`KoncepcjaIPC`** — thin client, one TCP connection per command (the server
  closes after each response).  All methods return `(bool, str)` or `bool`.
- **`EmulatorRunner`** — context manager that launches and tears down the
  emulator process, waits for the IPC port to come up.

#### Key patterns

**Always pause before touching Z80 state.**  `reg set`, `mem write`, `step in`,
`snapshot save/load` all call `cpc_pause_and_wait()` internally (which spins
until the Z80 thread exits `z80_execute()`).  From the test side, call
`ipc.pause()` first anyway — it keeps intent explicit and the server is
idempotent on double-pause.

**Use `wait bp <timeout_ms>` as a deadlock detector.**  This command blocks
until a breakpoint fires *or* the timeout expires.  If it times out when a
breakpoint *should* have fired, the Z80 thread is stuck (deadlocked).  5 000 ms
is a safe budget for 0x0038 (RST 38h — fires every ~50 ms of real CPC time).

```python
ok, resp = ipc.wait_bp(timeout_ms=5000)
assert ok, f"Z80 thread deadlock or BP never reached: {resp}"
```

**After EC_BREAKPOINT, call `signal_ready(true)` to unblock the render thread.**
A breakpoint mid-frame skips EC_FRAME_COMPLETE, so the render thread would block
in `wait_ready()` forever.  The fix lives in `kon_cpc_ja.cpp`; the IPC test
(`test_breakpoint_pause_step_resume`) catches regressions.

**`SDL_VIDEODRIVER=dummy` triggers headless mode on macOS.**  OpenGL init fails
under the offscreen/dummy SDL driver, so the emulator falls back to headless and
runs single-threaded.  The IPC protocol is identical in both modes.
`KoncepcjaIPC.is_threaded()` probes this by sending `devtools` (fails in
headless, succeeds in GUI mode).

**Snapshot round-trip pattern.**  Pause → read reference bytes → `snapshot save`
→ corrupt bytes → `snapshot load` → verify bytes restored.  This catches state
corruption from missing quiescence guards around snapshot I/O.

**Rapid pause/resume stress.**  20+ alternating `pause`/`run` commands without
a gap.  A single deadlock or condvar misuse shows up as a `send_command` timeout.
Tune `KoncepcjaIPC(timeout=5.0)` if the machine is slow.

#### Adding new tests

1. Add a `test_<name>()` function that returns `True`/`False`.
2. Append it to the `tests` list in `main()`.
3. Use `EmulatorRunner` as a context manager — it cleans up the process even on
   exception.
4. If the test exercises a threaded-only path, gate it with `ipc.is_threaded()`
   and skip (return `True`) in headless mode.

## Telnet Console

A persistent TCP text console on **port 6544** (IPC+1). Mirrors everything the CPC prints and accepts keyboard input — like a remote terminal for the emulated CPC.

### Connecting

```bash
nc localhost 6544
```

### How it works

- **Output (AMSDOS)**: Hooks TXT_OUTPUT (&BB5A) — when the Z80 PC hits this firmware vector, register A (the character) is pushed to a lock-free SPSC ring buffer, drained to the TCP client by the server thread.
- **Output (CP/M)**: Hooks BDOS entry (PC=0x0005) — when C=2 (C_WRITE), register E (the character) is captured. Works automatically alongside the firmware hook.
- **Input**: Received bytes are buffered and fed to AutoTypeQueue each frame. ANSI escape sequences are mapped to CPC keys (arrows, ESC, DEL, TAB).
- **Connection model**: Single client at a time (new connection replaces existing). Port probes forward up to +10 if 6544 is taken.

### ANSI escape mapping

| Terminal input | CPC key | Notes |
|---------------|---------|-------|
| `\x1b[A/B/C/D` | Cursor Up/Down/Right/Left | Standard ANSI arrows |
| `\x1b` (alone) | ESC | Bare escape |
| `\x7f` / `\x08` | DEL | Backspace |
| `\t` | TAB | |
| `\x03` (Ctrl+C) | ESC | Common telnet interrupt |
| `\r` or `\n` | RETURN | CR+LF collapsed |
| Printable ASCII | Corresponding key | Direct pass-through |

### Source files

- `src/telnet_console.h` — TelnetConsole class, ring buffer, input mutex
- `src/telnet_console.cpp` — TCP server thread, ANSI parsing, Z80 hook callback
- Z80 hooks: `z80_set_txt_output_hook()` (firmware) and `z80_set_bdos_output_hook()` (CP/M) in `src/z80.cpp` / `src/z80.h`
- Main loop integration: `g_telnet.start()` / `drain_input()` / `stop()` in `src/kon_cpc_ja.cpp`

## M4 HTTP Server

An embedded HTTP server serves a web interface for the M4 Board's virtual SD card. Compatible with `cpcxfer` and the M4 Board Android app.

### Connecting

```bash
# Default: http://127.0.0.1:8080/
curl http://localhost:8080/            # Web file browser
curl http://localhost:8080/status      # JSON status
curl http://localhost:8080/sd/m4/dir.txt  # Directory listing (M4 format)
```

### API Endpoints (matching real M4 Board)

| Method | Path | Description |
|--------|------|-------------|
| `GET /` | File browser HTML (single-page app) |
| `GET /config.cgi?ls=<path>` | Directory listing (text/plain) |
| `GET /config.cgi?cd=<path>` | Change CPC directory |
| `GET /config.cgi?run2=<path>` | Remote run file on CPC |
| `GET /config.cgi?rm=<path>` | Delete file |
| `GET /config.cgi?mkdir=<path>` | Create directory |
| `GET /sd/<path>` | Download file from SD |
| `GET /sd/m4/dir.txt` | M4-format directory listing |
| `GET /status` | JSON status (extension) |
| `POST /` | Upload file (multipart/form-data) |
| `POST /reset` | Reset CPC (deferred to main thread) |
| `POST /pause` | Toggle pause (deferred to main thread) |

### IPC Commands

```bash
echo "m4 http status" | nc localhost 6543   # Check HTTP server status
echo "m4 http start" | nc localhost 6543    # Start HTTP server
echo "m4 http stop" | nc localhost 6543     # Stop HTTP server
echo "m4 ports" | nc localhost 6543         # List port mappings
echo "m4 port set 80 8080" | nc localhost 6543  # Map CPC port 80 → host 8080
echo "m4 port del 80" | nc localhost 6543   # Remove mapping
```

### Thread Safety

The HTTP server runs in its own thread. CPC-mutating operations (reset, pause toggle) are deferred to the main thread via atomic flags, drained each frame by `g_m4_http.drain_pending()`.

### Source files

- `src/m4board_http.h` — M4HttpServer class, M4PortMapping struct
- `src/m4board_http.cpp` — HTTP server, request routing, file operations
- `src/m4board_web_assets.h` — Embedded HTML/CSS web interface
- Config: `[peripheral]` section: `m4_http_port`, `m4_bind_ip`, `m4_port_map_N`

### Config

```ini
[peripheral]
m4_http_port=8080          # HTTP server port (default 8080)
m4_bind_ip=127.0.0.1       # Bind IP (127.0.0.2 works on macOS without root)
m4_port_map_0=80:8080:1    # Port forwarding: cpc_port:host_port:user_override
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

### SDL3 macOS Mouse Events & ImGui Viewports

When `ImGuiConfigFlags_ViewportsEnable` is active, ImGui creates separate OS windows (viewports) for floating devtools. On macOS this triggers a chain of issues:

**Root cause chain:**
1. `SDL_CaptureMouse()` is a permanent no-op in the Cocoa implementation (`Cocoa_CaptureMouse()` in `SDL_cocoamouse.m`) — just returns `true`.
2. `koncpc_order_viewports_above_main()` keeps viewport windows above the main window via `orderWindow:NSWindowAbove`. This confuses macOS mouse-up delivery during drags.
3. macOS may deliver `mouseUp:` to a different NSWindow or lose it entirely.
4. SDL's internal `source->buttonstate` gets stuck at "pressed".
5. `SDL_PrivateSendMouseButton()` has duplicate suppression (`if (buttonstate == source->buttonstate) return`) — once stuck, ALL future button events for that button are silently dropped at the SDL level.

**The fix** (three layers):
- **SDL Cocoa backend** (`vendor/SDL/src/video/cocoa/SDL_cocoamouse.m`): `Cocoa_ReconcileMouseButtons()` polls `[NSEvent pressedMouseButtons]` (OS-reported hardware state) once per frame in `Cocoa_PumpEvents()`. Calls `SDL_SendMouseButton(RELEASED)` through SDL's full pipeline for any stuck buttons.
- **ImGui SDL3 backend** (`vendor/imgui/backends/imgui_impl_sdl3.cpp`): Viewport windowID check removed for mouse button and MOUSE_ENTER events. Button state is global — the GLFW and Win32 backends never had this check.
- **Application** (`src/kon_cpc_ja.cpp`): No legacy `showGui()` click handler — topbar clicks are handled purely by ImGui's `Button("Menu (F1)")`.

**Key pitfalls when debugging mouse issues:**
- `SDL_GetMouseState()` reads SDL's **event-derived cache** — if the button-up event was lost, this cache is stuck too. Use `SDL_GetGlobalMouseState()` which queries OS-reported state via `[NSEvent pressedMouseButtons]`.
- SDL and ImGui use **different button-to-bit mappings**: SDL has Left=bit0, Middle=bit1, Right=bit2; ImGui's `MouseButtonsDown` has Left=bit0, Right=bit1, Middle=bit2. Always use an explicit mapping table, not `(1 << button_n)` for both.
- SDL is a pre-built submodule. Edits to `vendor/SDL/src/` require rebuilding: `cd vendor/SDL/build && make -j$(sysctl -n hw.ncpu)` then `cmake --install . --prefix ../install`.

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

## Code Review Themes

Recurring patterns from code reviews. Follow these to avoid common pitfalls:

### Type Safety
- **`enum class` over int magic numbers** — Mode/state fields that have a fixed set of values (e.g. `workspace_layout`, `cpc_screen_scale`) must use `enum class`, not bare `int` with `#define` or comment-documented constants. This enables compiler-checked exhaustive `switch` and prevents accidental comparisons with unrelated integers.
- **Reference out-params over pointer out-params** — When a function fills in output values, use `int& w` not `int* w`. References cannot be null and make the calling convention explicit.

### Exception Safety
- **Catch specific exceptions, not `catch(...)`** — Always catch the most specific type possible: `std::filesystem::filesystem_error`, `std::invalid_argument`, `std::out_of_range`, etc. `catch(...)` swallows programmer errors.
- **Guard `std::stoi`/`std::stoul`** — These throw `std::invalid_argument` or `std::out_of_range` on bad input. Either wrap each call, or use a top-level try/catch in the command dispatcher (as IPC server does).

### Resource & I/O
- **Check `fwrite`/`fclose`/`fflush` return values** — Disk-full or I/O errors are silent if unchecked. At minimum, log a warning on failure.
- **Avoid `while(!feof(f))`** — This reads one extra time past EOF. Check the return value of `fread`/`fgets` instead.
- **Use `new T[N]()` or `= {}` for value-initialization** — Don't allocate then `memset`. Value-init is cleaner and works with non-POD types.
- **Never `memset` non-POD types** — Use default constructors, `= {}`, or assignment. `memset` on a `std::string` or struct with constructors is undefined behavior.

### Performance in Render Loops
- **Cache per-frame expensive computations** — ImGui render functions run every frame. Scanning 256KB of memory, rebuilding string lists, or formatting large tables must use a dirty flag and only recompute when state changes.
- **Stack arrays for small fixed-size buffers** — Don't use `std::vector` in per-frame code for buffers whose size is known at compile time. `char buf[64]` or `std::array` avoids heap allocation.
- **`std::map::upper_bound` for range lookups** — O(log N) instead of linear scan when looking up "which region does address X belong to?"

### String & Buffer Safety
- **Track string lengths explicitly** — When packing strings into fixed-width fields (like ATA IDENTIFY), compute `strlen` once and bounds-check per-character access. Don't rely on null-terminator proximity to short-circuit evaluation.
- **Path traversal protection** — Any user-supplied path (IPC, M4 virtual FS) must be validated: reject `..`, absolute paths, and symlink escapes.

## CI / Merging

- **NEVER merge a PR with failing CI checks.** Fix failures first.
- **Auto-merge** (`gh pr merge --auto`) triggers only when **all CI checks pass AND all review comment threads are resolved**. A PR that is green on CI but has unresolved threads will stay blocked. Always resolve open threads before expecting auto-merge to fire.

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

<!-- BEGIN BEADS INTEGRATION -->
## Issue Tracking with bd (beads)

**IMPORTANT**: This project uses **bd (beads)** for ALL issue tracking. Do NOT use markdown TODOs, task lists, or other tracking methods.

### Why bd?

- Dependency-aware: Track blockers and relationships between issues
- Git-friendly: Dolt-powered version control with native sync
- Agent-optimized: JSON output, ready work detection, discovered-from links
- Prevents duplicate tracking systems and confusion

### Quick Start

**Check for ready work:**

```bash
bd ready --json
```

**Create new issues:**

```bash
bd create "Issue title" --description="Detailed context" -t bug|feature|task -p 0-4 --json
bd create "Issue title" --description="What this issue is about" -p 1 --deps discovered-from:bd-123 --json
```

**Claim and update:**

```bash
bd update <id> --claim --json
bd update bd-42 --priority 1 --json
```

**Complete work:**

```bash
bd close bd-42 --reason "Completed" --json
```

### Issue Types

- `bug` - Something broken
- `feature` - New functionality
- `task` - Work item (tests, docs, refactoring)
- `epic` - Large feature with subtasks
- `chore` - Maintenance (dependencies, tooling)

### Priorities

- `0` - Critical (security, data loss, broken builds)
- `1` - High (major features, important bugs)
- `2` - Medium (default, nice-to-have)
- `3` - Low (polish, optimization)
- `4` - Backlog (future ideas)

### Workflow for AI Agents

1. **Check ready work**: `bd ready` shows unblocked issues
2. **Claim your task atomically**: `bd update <id> --claim`
3. **Work on it**: Implement, test, document
4. **Discover new work?** Create linked issue:
   - `bd create "Found bug" --description="Details about what was found" -p 1 --deps discovered-from:<parent-id>`
5. **Complete**: `bd close <id> --reason "Done"`

### Auto-Sync

bd automatically syncs via Dolt:

- Each write auto-commits to Dolt history
- Use `bd dolt push`/`bd dolt pull` for remote sync
- No manual export/import needed!

### Important Rules

- ✅ Use bd for ALL task tracking
- ✅ Always use `--json` flag for programmatic use
- ✅ Link discovered work with `discovered-from` dependencies
- ✅ Check `bd ready` before asking "what should I work on?"
- ❌ Do NOT create markdown TODO lists
- ❌ Do NOT use external issue trackers
- ❌ Do NOT duplicate tracking systems

For more details, see README.md and docs/QUICKSTART.md.

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

<!-- END BEADS INTEGRATION -->
