# konCePCja - Agent Instructions

Amstrad CPC emulator based on Caprice32, with Dear ImGui interface and IPC debugging support.

## Project Structure

```
src/
├── hw/                # THE hardware: one Device per chip, pin-level
│   ├── z80.cpp        #   CPU
│   ├── crtc.cpp       #   CRTC 6845
│   ├── gate_array.cpp #   Gate Array
│   ├── psg.cpp        #   AY-3-8912
│   ├── fdc.cpp        #   uPD765A + flux media
│   ├── tape.cpp       #   cassette deck
│   ├── memory.cpp     #   banking, ROM slots, expansion RAM
│   ├── video.cpp      #   raster output
│   ├── asic.cpp       #   6128+ ASIC
│   └── …              #   ppi, mf2, m4, symbiface, amx, rs232, plotter, …
├── subcycle/
│   ├── machine.cpp    # the board: builds and clocks the Devices together
│   └── record_replay.cpp
├── subcycle_bridge.cpp       # host ↔ board seam (media, ROMs, input, views)
├── kon_cpc_ja.cpp     # main loop, host state, config load/save, cleanup
├── slotshandler.cpp   # file loading (DSK, IPF, flux, CDT, SNA, CPR)
├── imgui_ui.cpp       # Dear ImGui interface (topbar, options, menus)
├── devtools_ui.cpp    # DevTools windows (disassembly, memory, breakpoints…)
├── koncepcja_ipc_server.cpp  # TCP IPC server for external debugging
├── telnet_console.cpp # TCP text console — mirrors CPC output, injects keyboard
├── kcpc/              # KCPC binary frontend server (port 6545): codec, threads, host seam
├── net_socket.cpp     # the socket shim the KCPC server and text IPC use (the one #ifdef _WIN32 for them)
└── z80_view.cpp       # host-side Z80 view + the firmware/BDOS output hooks
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

# UI-free build: no ImGui, no window, no SDL linked (only its headers; code
# that needs SDL sits behind KONCPC_SDL, the clock and frame surface behind
# host_clock.h / host_surface.h), always the -H loop (objects in
# obj/<arch>-noui; CMake: -DKONCPC_BUILD_MODERN_UI=OFF, no test_runner)
make -j$(nproc) KONCPC_MODERN_UI=0 koncepcja
python3 test/integrated/headless_smoke.py --ui-free
```

## Command Line Arguments

`./koncepcja --help` is the source of truth — this table is a copy and has
drifted before (it listed 9 of 15 flags, omitting `--headless`):

```
Usage: koncepcja [options] <slotfile(s)>

Options:
  -a/--autocmd=<command>   Execute BASIC command after CPC boots (repeatable)
  -B/--exit-on-break       Exit with code 1 when a breakpoint is hit
  -c/--cfg_file=<file>     Use custom config file
  -D/--debug               Frame timing + audio diagnostics in the DevTools bar
  -E/--exit-after=<spec>   Exit after N frames (100f), seconds (5s) or ms (3000ms)
  -H/--headless            Run without display or audio (IPC and emulation only)
  -h/--help                Show help
  -i/--inject=<file>       Inject binary into memory after boot
  -L/--list-plugins        List video plugins (index: name) and exit
  -o/--offset=<address>    Injection address (default: 0x6000)
  -O/--override=<opt=val>  Override config option (repeatable)
  -s/--sym_file=<file>     Load symbols for disassembly
  -V/--version             Show version
  -v/--verbose             Verbose logging
  -F/--fps                 Log once-per-second FPS to stdout

Slot files: .dsk/.ipf/.raw (disk), .scp/.hfe/.a2r (flux disk, drive A only),
.cdt/.voc (tape), .cpr (cartridge), .sna (snapshot), .zip (archive of any
of the above)
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
```

The response is categorised and far longer than the table below — ask the
server rather than trusting any list in this file:

```
OK available commands (usage: help <command>):
  Protocol: persistent connections, ';' chains commands, 'disconnect' closes.
  Numbers: 0x, $, &, # hex prefixes accepted (e.g. $C000, &4000, #BB5A).
  CORE:      disconnect, gui, load <file>, metrics, pause, ping, plugins, …
  DEBUG:     bp …, data …, disasm …, mem …, reg …, step …, trace …, wait …
  …
```

`help <command>` prints that command's own usage. The full protocol lives in
[docs/ipc-protocol.md](docs/ipc-protocol.md).

### IPC Command Reference

| Command | Description | Example |
|---------|-------------|---------|
| `ping` | Test connection | `ping` → `OK pong` |
| `version` | Get version | `version` → `OK koncepcja-<version> port=6543` |
| `help` | List commands, categorised | `help` → `OK available commands (usage: help <command>): ...` |
| `pause` | Pause emulation | `pause` → `OK` |
| `run` | Resume emulation | `run` → `OK` |
| `reset` | Reset CPC | `reset` → `OK` |
| `regs` | Dump all registers | `regs` → `OK A=00 F=00 ...` |
| `reg get <R>` | Get register | `reg get PC` → `OK 1234` |
| `reg set <R> <V>` | Set register (`HALT` is read-only: `ERR 400 reg-read-only`) | `reg set PC 0x4000` → `OK` |
| `mem read <addr> <len> [--view=read|ram] [--bank=N] [ascii]` | Read memory | `mem read 0x4000 16 ascii` |
| `mem write <addr> <hex>` | Write memory | `mem write 0x4000 C3004000` |
| `bp add <addr>` | Add breakpoint | `bp add 0x4000` → `OK` |
| `bp del <addr>` | Remove breakpoint | `bp del 0x4000` → `OK` |
| `bp list` | List breakpoints | `bp list` → `OK count=1 4000` |
| `bp clear` | Clear all breakpoints | `bp clear` → `OK` |
| `step [n]` | Step N instructions | `step 10` → `OK` |
| `wait pc <addr> [timeout]` | Run until PC reaches addr exactly (one-shot breakpoint), then pause. A breakpoint of your own AT addr counts as arriving (`OK`); one that fires earlier is `ERR 409 stopped-elsewhere`, and a shutdown mid-wait is `ERR 503 shutting-down` | `wait pc 0x4000 5000` |
| `wait mem <addr> <val> [mask] [timeout]` | Wait for memory value | `wait mem 0xBE80 0xFF` |
| `wait bp [timeout]` | Wait for breakpoint hit (reports only after pause lands; drops hits from a previous arming and the hit of a stop the machine has since been run out of) | `wait bp 10000` |
| `wait vbl <n> [timeout]` | Run N frames of emulation (vertical blanks, not wall time), then pause; same count as `step frame N` from a paused machine. A breakpoint that fires first is `ERR 409 stopped-elsewhere` | `wait vbl 50` |
| `disasm <addr> <count>` | Disassemble | `disasm 0x4000 10` |
| `screenshot [path]` | Save the machine's frame as a 768x544 PNG (rows doubled, the `hash frame` bytes; same in every build) | `screenshot /tmp/shot.png` |
| `snapshot save <path>` | Save state | `snapshot save game.sna` |
| `snapshot load <path>` | Load state | `snapshot load game.sna` |
| `load <path>` | Load file (.dsk/.sna/.cpr/.bin); a successful disk/tape/snapshot/cartridge load also lands on the matching Recent list, with the path made absolute (GUI saves the config, `--headless` does not) | `load game.dsk` |
| `devtools` | Open DevTools window (idempotent); `devtools off` closes; F12 toggles | `devtools` → `OK` |
| `config get\|set <key> [val]` | Read/modify settings (`model`, `crtc_type`, `ram_size`, …) | `config set model 3` |
| `config apply` | Rebuild the machine with staged settings (needed after `config set model`) | `config apply` → `OK` |
| `tier` | Run-tier policy | `tier` → `OK policy=auto effective=fast pinned=0` |
| `tier set <p>` | Set policy: auto/fast/wake/soldered/faithful | `tier set wake` → `OK policy=wake` |
| `input keydown <name>` | Press and hold a key | `input keydown SHIFT` |
| `input keyup <name>` | Release a key | `input keyup SHIFT` |
| `input key <name> [hold=N]` | Tap a key (press, hold N frames [default 2], release). The hold counts frames that began with the key down, so the firmware scans it N times | `input key RETURN hold=5` |
| `input chord <M+K> [hold=N]` | Atomic modified tap (modifiers then one key, all down at once). Same hold semantics as `input key`: N frames that began with the keys down | `input chord CTRL+SHIFT+ESC` |
| `input type <text>` | Type text via AutoTypeQueue (supports `~KEY~`, newlines; async like `autotype`) | `input type run"game~RETURN~` |
| `input joy <0\|1> <dir>` | Joystick dir (U/D/L/R/F1/F2, `0`=release all, `-`=release one) | `input joy 0 F1` |
| `input mouse move <dx> <dy>` | Relative mouse motion (needs AMX/Symbiface mouse enabled) | `input mouse move 10 -4` |
| `input mouse button <L\|M\|R> <down\|up>` | Press/release a mouse button | `input mouse button L down` |
| `input mouse buttons <mask>` | Set whole SDL button mask (L=1,M=2,R=4) | `input mouse buttons 0` |
| `input gun move <x> <y>` | Absolute light-gun aim in window px (needs a phazer type enabled) | `input gun move 100 60` |
| `input gun trigger <down\|up>` | Press/release the gun trigger (Trojan also pulses J0 FIRE1) | `input gun trigger down` |
| `input state [row]` | Read back held keys (all, or one row's byte + names) | `input state`, `input state 2` |
| `autotype <text>` | Queue text via AutoTypeQueue (supports WinAPE `~KEY~`) | `autotype run"game~RETURN~` |
| `autotype status` | Show pending queue length | `autotype status` |
| `autotype clear` | Cancel pending input | `autotype clear` |

`<name>` is a friendly key name (`RETURN`, `SPACE`, `ESC`, `F1`, `UP`, ...) or a
single character (`a`, `A`, `1`). For multi-line entry or special keys inside a
string, either command works: `input type` is routed through the same
AutoTypeQueue as `autotype`, so `~KEY~` tokens and newlines behave identically.

### Scripting Example

```bash
#!/bin/bash
# wait-and-screenshot.sh - Wait for game to load, take screenshot

echo "load game.dsk" | nc localhost 6543
sleep 1
echo "run" | nc localhost 6543
echo "wait pc 0xC000 30000" | nc localhost 6543  # Wait for game entry
echo "screenshot /tmp/game.png" | nc localhost 6543
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

- **`KoncepcjaIPC`** — thin client, one TCP connection per command for
  simplicity (the server itself keeps connections open; `disconnect` closes
  one).  All methods return `(bool, str)` or `bool`.
- **`EmulatorRunner`** — context manager that launches and tears down the
  emulator process, waits for the IPC port to come up.

`make e2e_test` runs the same suite the macOS CI job runs
(`test/integrated/run_tests.sh`, which includes this harness).
`make e2e_test_slow` runs it pinned to the efficiency cores (`taskpolicy -b`
on macOS, `nice` elsewhere), the cheap way to reproduce a flake that only a
slow shared CI Mac shows.
`KONCPC_E2E_HEADLESS=1` in the environment makes `EmulatorRunner` add
`--headless` to every launch, so the same suite exercises the `-H` loop.
Both loops run each frame through `emu_run_frame()` (`src/emu_frame.h`).

#### Key patterns

**Always pause before touching Z80 state.**  `reg set`, `step in` and
`snapshot save/load` take a `CpcPauseLease` (the machine is idle between two
frames while they run, and its run state is put back after). `mem write`,
`mem fill`, `mem cpu-write`, `poke write` and `asm assemble` run at a frame
boundary when KCPC clients read or the machine runs unpaced, and under a lease
held alone otherwise; `poke apply`/`unapply` run under a lease alone. None of
them pauses the machine for you, so from the test side call `ipc.pause()` first
when the test needs it stopped. The server is idempotent on double-pause.

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

**`SDL_VIDEODRIVER=dummy` does NOT mean headless.**  The video plugins
initialise fine on the dummy driver, so `EmulatorRunner.start()` runs the full
GUI build: a Z80 thread plus a render thread that draws ImGui and DevTools
offscreen. Only `-H/--headless`, or a video init that fails outright ("falling
back to headless" in the log), runs the single-threaded headless loop. So
render-thread code (DevTools windows, which a breakpoint stop opens) runs during
e2e tests. The IPC protocol is identical in both modes.
`KoncepcjaIPC.is_threaded()` tells them apart by sending `gui` (`gui=0` for
the `-H` loop, `gui=1` for the GUI build, dummy driver included). `devtools`
cannot tell them apart: it returns `OK` in both. `make e2e_test` also runs
`test/integrated/headless_smoke.py`, which boots `-H`, reads its ports from
the startup manifest and drives ping/regs/pause/run/quit.

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

#### Testing the restart vectors: use a prepared ROM, not `mem write`

**`mem write` and `disasm` do not address the same memory.**  `mem write`
targets RAM; `disasm` and `mem read` show the ROM overlay (unless `--view=ram`
is passed). The lower ROM covers `0x0000-0x3FFF`, which is *all eight* Z80
restart vectors — so a handler poked to `0x0030` lands in RAM the CPU will
never execute, while `disasm 0x0030` shows firmware.

Use `make_test_rom()` in `test/integrated/ipc_harness.py` instead.  It builds
a synthetic 32K system ROM with no firmware in it and returns a temp
directory; `rom.rom_path` is only a directory name, so:

```python
romdir = make_test_rom({0x0030: bytes([0xD1, 0xC3, 0x04, 0x60])})  # POP DE : JP $6004
try:
    with EmulatorRunner() as emu:
        emu.start('-O', f'rom.rom_path={romdir}', '-O', 'system.model=2')
finally:
    shutil.rmtree(romdir, ignore_errors=True)
```

Three things are load-bearing:

- **Pin `system.model=2`.**  The image is written as `cpc6128.rom` because the
  filename comes from `chROMFile[model]`.  Under another model the emulator
  aborts looking for `cpc464.rom` and the test reports a misleading
  "would not start".
- **Assert the ROM is really yours** by disassembling a vector and matching an
  **exact mnemonic**.  Filler is `0xFF`, which decodes as `rst 38h`, so a
  substring check for `'rst'` is satisfied by empty ROM.
- **Don't fight the scaffolding.**  `0x0000` jumps to a park loop at `0x0100`
  (`EmulatorRunner` treats `PC == 0` as "not ready yet"), and `0x0038` holds a
  bare `RET`.  `make_test_rom` refuses handlers that overlap these.

The emulator refuses to boot a missing or wrong-sized ROM
(`ERR_CPC_ROM_MISSING` / `ERR_NOT_A_CPC_ROM`), so a botched fixture fails loudly
rather than silently falling back to firmware.

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
- Z80 hooks: `z80_set_txt_output_hook()` (firmware) and `z80_set_bdos_output_hook()` (CP/M) in `src/z80_view.cpp` / `src/z80_view.h`
- Main loop integration: `g_telnet.start()` / `drain_input()` / `stop()` in `src/kon_cpc_ja.cpp`

## KCPC Binary Server

A binary server for out-of-process frontends on **port 6545** (probes forward
to 6554; read `ports.binary` from the startup manifest, never assume it). Up
to 8 loopback clients at once; one holds `CONTROL`. Full wire specification:
[docs/protocol/kcpc-binary-v1.md](docs/protocol/kcpc-binary-v1.md) and
[docs/protocol/fb-ring-v1.md](docs/protocol/fb-ring-v1.md).

```ini
[ipc]
binary_port_enabled=1      # 0 = do not open the port (manifest: `binary: null`)
```

### Source map
- `src/kcpc/kcpc_proto.h` — opcodes, status codes, bounds-checked field reader/writer (`test/kcpc_proto_test.cpp`).
- `src/kcpc/kcpc_server.{h,cpp}` — acceptor + reader/writer thread per client (`test/kcpc_server_test.cpp`).
- `src/kcpc/kcpc_gate.{h,cpp}` — lock-free frame-boundary read queue (`kcpc_service_frame()` in `frame_epilogue()`), or `cpc_read_while_stopped()` when paused (`test/kcpc_gate_test.cpp`).
- `src/kcpc/kcpc_host.{h,cpp}` — the single seam between socket threads and the emulator.
- `src/kcpc/kcpc_events.h` — MPSC lock-free event ring and codec (`encode_event` / `decode_event`).
- `src/kcpc/fb_ring.h`, `fb_publish.{h,cpp}`, `fb_ring_reader.{h,cpp}` — canonical RGB24 768×272 shared-memory frame ring (`kcpc-fb-<pid>`, `src/shm_region.{h,cpp}`). `tools/fb_dump.c` is a standalone C reader with no emulator headers.
- `src/kcpc/kcpc_client.{h,cpp}` — C++ client library (`test/kcpc_client_test.cpp`); `test/integrated/kcpc/kcpc_client.py` is the Python reference client.

### Threading & boundary invariants (apply across KCPC, text IPC, and DevTools)
- **Socket and UI threads never touch the running machine directly.** Reads go through `kcpc::Gate` (serviced at `frame_epilogue()` when running, or under `cpc_read_while_stopped()` when stopped). Mutations take `CpcPauseLease` via `change_alone()` and finish with `restore_run_state()` (except batched `REGS_SET`/`MEM_WRITE` when KCPC clients read or the machine runs unpaced, which run at the frame boundary without a lease).
- **Never acquire `g_pause_mutex` (`cpc_stopped()`, `CpcPauseLease`) on a per-frame or per-read path** — the frame runner holds `g_pause_mutex` for the entire frame (~15 ms).
- **Callbacks invoked on the frame runner or under a lock (`Gate` read callbacks, `cpc_set_run_state_observer`, breakpoint/symbol/media change hooks) must only copy memory or post to the lock-free event ring** — never perform file I/O, block, or acquire `CpcPauseLease`.
- **Nothing inside `alone_under_lease()`'s work callback may re-acquire `g_pause_mutex`** (`cpc_announce_reset()`, `emulator_reset()`, nested leases); put those in the `let_go` callback where the mutex is already released.
- **Read chip state from Device `*_peek()` through the gate, never from the host view structs** (`CRTC`, `GateArray`, `PSG`, `asic`, `pbRegisterPage`), which the frame runner rewrites each frame.
- **Read video frames from `subcycle_bridge_framebuffer()` through the gate, never from `back_surface`** (which is being blitted/swapped in plugin pixel format).
- **Multi-frame step walks (`CpcWalk`) wait on `cpc_announced_stopped()`, never on `g_emu_paused`** (which briefly flips during another thread's `CpcPauseLease`), and check `cpc_walk_interrupted()` before each instruction.
- **Always connect to the ports printed in your own instance's startup manifest** — never scan 6543–6554 or pattern-kill processes.

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
| `GET /preview.bmp` | The machine's frame as a 768x544 BMP (extension; see below) |
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
m4board=1                  # Enable the board (off by default). While on, its ROM
                           # owns the disc vectors: CAT/RUN" go to the SD card
                           # ("Drive C:/") until you type |DISC — same as real hw.
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
- `src/hw/psg.cpp` - Audio timing info
- `keyboard.cpp` - Key mapping issues

### DevTools

Press **F12** or send `devtools` via IPC to open the developer tools: a
toolbar plus floating windows (Registers, Disassembly, Memory, Stack,
Breakpoints, Symbols, Disc Tools, …), listed in the Window menu.

- **Step group**: Step In / Step Over / Step Out / Resume-Pause sit in the
  DevTools toolbar AND, as In / Over / Out / Run-Pause, in the Disassembly
  window's menu bar. Both call the same `dbg_step_*` helpers in
  `src/imgui_ui.cpp`, and `debug_step_controls()` (`src/imgui_ui_testable.h`)
  decides for both when stepping is allowed (paused, no step walk running).
  While a DevTools window has keyboard focus: F7 Step In, Shift+F7 Step Over,
  Shift+F11 Step Out, F5 run/pause.
- **Disc Tools**: the Files listing rebuilds on its own when the drive gets a
  different medium (load, swap, eject), keyed on `dsk_media_generation()`.

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
| Shift+F9 | Cycle run-tier policy (auto/fast/wake) |
| F10 | Exit |
| F12 | Toggle DevTools |
| Shift+F1 | Virtual keyboard |
| Shift+F3 | Save snapshot |
| Cmd+K (macOS) / Ctrl+K (Linux, Windows) | Command palette |
| Cmd+O (macOS) / Ctrl+O (Linux, Windows) | Load Disk A... |
| Cmd+S (macOS) / Ctrl+S (Linux, Windows) | Save Snapshot... |

Cmd/Ctrl+K/O/S belong to the host UI and never reach the CPC; unmodified keys
(F-keys included) reach the CPC. Host chords are resolved in one place
(`src/host_chords.{h,cpp}`) and dispatched only by SDL — the native macOS menu
displays chord labels without registering AppKit key equivalents (so keys never
double-fire).

### SDL3 macOS Mouse Events & ImGui Viewports

With `ImGuiConfigFlags_ViewportsEnable`, macOS may drop `mouseUp:` events across floating viewport windows, leaving SDL's event-derived button cache stuck pressed:
- Query `SDL_GetGlobalMouseState()` (OS-reported state) rather than `SDL_GetMouseState()` (event-derived cache) when reconciling button state (`Cocoa_ReconcileMouseButtons()` in `vendor/SDL/src/video/cocoa/SDL_cocoamouse.m`).
- SDL and ImGui use **different button-to-bit mappings**: SDL has Left=bit0, Middle=bit1, Right=bit2; ImGui's `MouseButtonsDown` has Left=bit0, Right=bit1, Middle=bit2. Always use an explicit mapping table, not `(1 << button_n)` for both.
- SDL is a pre-built submodule. Edits to `vendor/SDL/src/` require rebuilding: `cd vendor/SDL/build && make -j$(sysctl -n hw.ncpu)` then `cmake --install . --prefix ../install`.

## Configuration

Config file locations (in order of precedence — the first one found wins):
1. `-c/--cfg_file=<path>` argument
2. `$XDG_CONFIG_HOME/koncepcja/koncepcja.cfg` (or `~/.config/koncepcja/koncepcja.cfg`)
3. `$XDG_CONFIG_HOME/koncepcja.cfg` (or `~/.config/koncepcja.cfg`), `~/.koncepcja.cfg` — legacy flat paths
4. `$CWD/koncepcja.cfg`, then `koncepcja.cfg` next to the binary
5. `/etc/koncepcja.cfg`
6. `koncepcja.cfg` in the macOS app bundle's `Resources/` (next to the binary's
   parent)

The profile config outranks a checkout-local file. Settings ▸ System shows the
file in use, as does `config get file` over IPC. The sidecars — `imgui.ini` and
the DevTools `layouts/` directory — live next to whichever config file wins, so
they move with it.

### Scripted runs and dialogs

Only a user gesture (window close, F10, native Quit, menu Quit) may raise the
"unsaved changes" dialog (`quitLosesWrites()`, `koncpc_quit_unsaved_message()`
in `src/quit_policy.h`). Non-interactive exits (IPC `quit`, SIGTERM/SIGINT,
`-E/--exit-after`, `-B/--exit-on-break`, `-a KONCPC_EXIT`, or when
`KONCPC_NO_DIALOGS=1` is set) never prompt and flush dirty disc/IDE images back
to disk automatically (archive-backed discs save to `<entry>.dsk` beside the
archive). Signals exit with `128+signo` without saving settings; a second
signal kills the process immediately.

### Key Config Options

```ini
[system]
model=2           # 0=464, 1=664, 2=6128, 3=6128+
ram_size=128      # RAM in KB: 64, 128, 192, 256, 320, 512, 576 … 4160
speed=4           # Clock speed MHz
run_tier=0        # 0=auto (Fast; drops to Wake while any breakpoint/watchpoint/IO-BP is set),
                  # 1=fast, 2=wake, 3=soldered, 4=faithful. Shift+F9 cycles auto→fast→wake;
                  # `tier` on IPC reads/sets it; KONCPC_TIER / KONCPC_WAKE pin it.

[video]
scr_scale=2       # Window scale factor
scr_style=1       # Rendering style (0-11)
scr_window=1      # 1 = windowed, 0 = fullscreen (IPC `config fullscreen` has opposite polarity).
vsync=1           # 1=VSYNC (default, required on VRR displays). 0=MAILBOX/IMMEDIATE on the main
                  # window only (viewport windows stay VSYNC).

[sound]
enabled=1         # INI keys are enabled/playback_rate/bits/stereo/volume (not snd_*).
playback_rate=2   # 0=11025, 1=22050, 2=44100, 3=48000, 4=96000
volume=80         # 0-100

[input]
lightgun=0        # 0=off, 1=Amstrad Magnum Phaser, 2=Trojan Light Phazer.
host_chords=1     # 1=Cmd/Ctrl+K/O/S drive host UI; 0=pass Ctrl+K/O/S to CPC on Linux/Windows.
```

## Code Conventions

- Use `byte` (uint8_t), `word` (uint16_t), `dword` (uint32_t) types.
- Z80 memory access: `z80_read_mem(addr)`, `z80_write_mem(addr, val)`.
- Pause/resume: `cpc_pause()`, `cpc_resume()`, `cpc_run()` (use `cpc_run()` for user/client resume requests; `cpc_resume()` is ignored while any pause lease is held).
- **`CpcPauseLease` lifecycle**: end every lease with `restore_run_state()` (put back prior state), `release_stopped()` (step), or `release_running()` (reset / frame step) — never manual `if (!was_paused) cpc_resume()`. Read-only leases use `read_alone()`. If a lease holder waits on another thread that also acquires a lease, call `lend()` around the wait to avoid deadlock.
- **DevTools mutations**: change machine or memory state from the UI thread through `change_machine_alone()` (`src/devtools_ui.cpp`), never a bare `z80_write_mem()` beside a running frame.
- **Media insertion**: insert cartridges via `cpc_cartridge_insert()` (or `cpc_cartridge_swap()` when already holding the lease) and discs via `disc_insert()` — never bare `file_load()`, which frees live pointers or clobbers slot state on error. Archive-backed discs must pass `archive_entry` so writes save to `<entry>.dsk` beside the archive.
- **User-writable output paths**: save user exports (traces, dumps, screenshots) to `CPC.sdump_dir` with fallback to `SDL_GetPrefPath("konCePCja", "konCePCja")`, never `SDL_GetBasePath()` (read-only in installed builds).
- ImGui state is in global `imgui_state` struct.

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

### Performance & Cross-Thread State in UI / IPC
- **Copy under the guard, format and write outside it** — Hold `CpcPauseLease` (or a shared lock) only long enough to `memcpy` raw POD state into a caller-owned buffer. Never format text, build large strings, or perform file/socket I/O while holding a lease or lock that blocks the emulation thread.
- **Bridge live emulation state to UI at `frame_epilogue()`, never via per-frame leases** — Acquiring `CpcPauseLease` per frame contends with `g_pause_mutex` (held for the entire ~15 ms frame), and reading live Z80-written buffers from the UI thread is a data race. When a viewer is open, copy the needed slice in `frame_epilogue()` (where the Z80 is between frames) and hand it to the UI thread under a light UI-only mutex + generation counter.
- **Virtualise large lists and tables (`ImGuiListClipper`)** — Keep buffers as raw structs and format only the ~40–50 visible rows inside `ImGuiListClipper`; never materialize a whole buffer or table into strings up front.
- **Cache per-frame expensive computations** — ImGui render functions run every frame. Scanning memory, rebuilding file/symbol lists, or recomputing layouts must key off a dirty flag or media/symbol generation counter.
- **Stack arrays for small fixed-size buffers** — Prefer `char buf[64]` or `std::array` over `std::vector` in per-frame code when the bound is known at compile time.
- **`std::map::upper_bound` for range lookups** — O(log N) instead of linear scan when looking up which region an address belongs to.

### String & Buffer Safety
- **Track string lengths explicitly** — When packing strings into fixed-width fields (like ATA IDENTIFY), compute `strlen` once and bounds-check per-character access. Don't rely on null-terminator proximity to short-circuit evaluation.
- **Path traversal protection** — Any user-supplied path (IPC, M4 virtual FS) must be validated: reject `..`, absolute paths, and symlink escapes.

## CI / Merging

- **NEVER merge a PR with failing CI checks.** Fix failures first.
- **Auto-merge** (`gh pr merge --auto`) triggers only when **all CI checks pass AND all review comment threads are resolved**. A PR that is green on CI but has unresolved threads will stay blocked. Always resolve open threads before expecting auto-merge to fire.

### Automated PR review (`review` label)

`.github/workflows/pr-review.yml` runs the compound-engineering plugin's
`ce-code-review` (a Claude Code skill that sends the diff to several reviewer
sub-agents, checks their findings and merges them into one report) on a PR,
and posts the report as **one** PR comment. A re-run edits that comment
instead of adding a new one.

- **Trigger:** add the `review` label to the PR. The workflow removes the
  label when it finishes, so adding it again runs a fresh review. Or by hand:
  `gh workflow run pr-review.yml --repo ikari-pl/konCePCja -f pr=<number>`.
  It never runs on a push, and PRs from forks are skipped (they get no
  secrets).
- **Setup, once:** `gh secret set CROWBOT_API_KEY --repo ikari-pl/konCePCja`
  (the crowbot.sh API key). Optional: `gh variable set REVIEW_MODEL --body
  <crowbot model id>` (default `claude-opus-5-5`), and `REVIEW_SUBAGENT_MODEL`
  for the reviewer sub-agents (default: the same model). `crowbot models`
  lists the ids.
- **How it reaches the model:** crowbot.sh serves only the OpenAI API format,
  Claude Code speaks only Anthropic's, so the job starts a LiteLLM proxy (a
  local translator, `.github/review/litellm.yaml`) between them.
- **Cost:** one review is one Claude Code run with roughly 6-10 reviewer
  sub-agents plus a validator per finding, each reading the diff and the
  files around it. Expect a few million tokens for a mid-sized PR (more for a
  big diff), billed to the crowbot account at crowbot's rates, and 10-40
  minutes of runner time (limit 60). The comment's footer shows the tokens
  the run used. Label only the PRs worth it.
- **Safety:** the PR is untrusted input. The job checks out the base commit
  and reads the PR head with `git show` (none of the PR's code runs), the
  agent holds only a read-only `GITHUB_TOKEN`, may run only reads, read-only
  `git`/`gh` and writes under its `/tmp` run directory, and the crowbot key
  lives only in the proxy, run as a separate user after `sudo` and Docker are
  taken away. Only the separate posting job has write access, and it runs
  nothing from the PR. The full run directory is a workflow artifact for 30
  days.

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
