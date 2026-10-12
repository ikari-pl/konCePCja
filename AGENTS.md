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
├── net_socket.cpp     # the socket shim the KCPC server uses (the one #ifdef _WIN32 for it)
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
| `wait bp [timeout]` | Wait for breakpoint hit (reports only after pause lands; drops hits from a previous arming) | `wait bp 10000` |
| `wait vbl <n> [timeout]` | Run N frames of emulation (vertical blanks, not wall time), then pause; same count as `step frame N` from a paused machine. A breakpoint that fires first is `ERR 409 stopped-elsewhere` | `wait vbl 50` |
| `disasm <addr> <count>` | Disassemble | `disasm 0x4000 10` |
| `screenshot [path]` | Take screenshot | `screenshot /tmp/shot.bmp` |
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

**`SDL_VIDEODRIVER=dummy` does NOT mean headless.**  The video plugins
initialise fine on the dummy driver, so `EmulatorRunner.start()` runs the full
GUI build: a Z80 thread plus a render thread that draws ImGui and DevTools
offscreen. Only `-H/--headless`, or a video init that fails outright ("falling
back to headless" in the log), runs the single-threaded headless loop. So
render-thread code (DevTools windows, which a breakpoint stop opens) runs during
e2e tests; that is how the Registers window corrupted the machine in
beads-vwwq. The IPC protocol is identical in both modes.
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
targets RAM; `disasm` and `mem read` show the ROM overlay.  The lower ROM
covers `0x0000-0x3FFF`, which is *all eight* Z80 restart vectors — so a
handler poked to `0x0030` lands in RAM the CPU will never execute, while
`disasm 0x0030` cheerfully shows you firmware.  Two step-out tests were
written this way and **passed for the wrong reason**: they were exercising
Amstrad's restart handlers, which happen to return.  Both had to be deleted.

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

A second server for out-of-process frontends on **port 6545** (probes forward
to 6554; read `ports.binary` from the startup manifest, never assume it).
Binary frames, up to 8 clients at once, loopback only. The contract, and the
table of which implementation slices have landed, is
[docs/protocol/kcpc-binary-v1.md](docs/protocol/kcpc-binary-v1.md). So far it
answers `HELLO`, `PING`, `MACHINE_INFO`, the machine reads `REGS_GET`,
`MEM_INFO` and `MEM_READ` (64 KiB of memory is one reply), and, for the one
client that holds control (`CONTROL`; a forced request takes it from another
client), the writes `REGS_SET` and `MEM_WRITE` and run control: `PAUSE`,
`RUN`, `RESET`, `STEP` (in, over, out, to, frames), with `RUN_STATE` for
anyone, and the breakpoints `BP_ADD`, `BP_DEL`, `BP_CLEAR` (`BP_LIST` for
anyone): execution breakpoints, watchpoints and I/O breakpoints, on the
lists the text IPC and DevTools edit, and input: `KEY` (a keyboard matrix
position; row 9 is joystick 0), `KEYS_RELEASE`, `TEXT` (the AutoTypeQueue)
and `MOUSE`, with `INPUT_STATE` for anyone. A client
that sends `SUBSCRIBE` is pushed events (`EVT_STOPPED`, `EVT_RESUMED`,
`EVT_RESET`, `EVT_FRAME`, `EVT_BREAKPOINTS_CHANGED`, ...), whoever caused
them: a pause or a `bp add` from the text IPC or the GUI is pushed too, and
a breakpoint, watchpoint or I/O hit says what was hit. The protocol is at
major version 0: client and server must come from the same checkout.

```ini
[ipc]
binary_port_enabled=1      # 0 = do not open the port (manifest: `binary: null`)
```

- `src/kcpc/kcpc_proto.h` — opcodes, status codes and the bounds-checked
  field reader/writer. Pure; unit-tested in `test/kcpc_proto_test.cpp`.
- `src/kcpc/kcpc_server.{h,cpp}` — acceptor thread plus a reader and a writer
  thread per client. Knows nothing about the emulator, so
  `test/kcpc_server_test.cpp` runs it over loopback with a fake machine.
- `src/kcpc/kcpc_gate.{h,cpp}` — the machine gate: how a read is taken
  without pausing anything. A reader thread queues it and the frame runner
  runs it at the next frame boundary (`kcpc_service_frame()`, called from
  `frame_epilogue()`); on a stopped machine the reader runs it itself under
  `cpc_read_while_stopped()`. That is not a `CpcPauseLease` on purpose: a
  Run is refused while any lease is held, so a client polling a paused
  machine through leases swallows `run` now and then. Emulator-free like
  the server; `test/kcpc_gate_test.cpp` drives it with a fake machine.
- `src/kcpc/kcpc_host.{h,cpp}` — the seam to the emulator. **Nothing touches
  the machine from a socket thread except through here**, and only the ways
  the document's threading model allows: reads through the gate, changes
  under a `CpcPauseLease` while it is the only lease held
  (`change_alone()`, so a write never runs beside a machine rebuild) and
  ended with `restore_run_state()`. A read callback must only copy:
  it runs on the frame runner. **Do not call anything that takes
  `g_pause_mutex` (`cpc_stopped()`, a lease) from a path a running machine
  hits per read**: the runner holds that mutex for the whole of every
  machine frame, and on an unpaced machine a socket thread then waits about
  15 ms for it each time.
- Register edits from every source reach the machine through
  `subcycle_bridge_write_regs()` (`src/subcycle_bridge.h`): KCPC by mask, the
  text IPC and DevTools through `subcycle_bridge_regs_to_machine()`, which
  diffs the host view. At a breakpoint the machine's own PC is one past the
  address everything shows; `subcycle_bridge_shown_regs()` is the block with
  the address that is shown.
- `src/kcpc/kcpc_events.h` — the event record and the ring events are
  posted into (`Server::post()`): lock-free, several producers, one
  consumer (the server's fan-out thread). Posting is the only thing the
  frame runner does for an event.
- The run state is **announced by the core**, not sampled by the server:
  `cpc_pause()`, an applied resume and the last pause lease letting go
  compare the settled state with the one last announced and call the one
  observer (`cpc_set_run_state_observer()`, `src/koncepcja.h`). A lease's
  own pause and resume are never announced. The observer runs under the
  pause mutex, on the frame runner too: it must only post. Pass a
  `CpcRunCause` to `cpc_pause()` / `release_stopped()` / `release_running()`
  when the stop or run should name who asked.
- To run the machine on somebody's request use `cpc_run()`, not
  `cpc_resume()`: the latter is dropped while any pause lease is held.
- A step that runs and stops the machine on its way (over a call, out of a
  routine, to an address) is a **walk**: `CpcWalk` in `src/koncepcja.h`,
  opened by `z80_run_until_ephemeral()`, `z80_step_out_until()` and
  `z80_step_over()` themselves, so the text IPC's and the DevTools' walks
  get it too. It is announced as one step (one `EVT_RESUMED` at most, one
  `EVT_STOPPED`). **A walk waits on `cpc_announced_stopped()`, never on
  `g_emu_paused`**: a pause lease sets the latter for the length of another
  actor's memory write, and a walk that took that for a stop ended half way
  (beads-pja3). It runs the machine with `cpc_walk_run()` and looks at
  `cpc_walk_interrupted()` before every instruction it steps: somebody's
  pause must end it, and on a machine the walk has stopped already a pause
  changes nothing else it could notice.
- The KCPC walker (`walk()` in `src/kcpc/kcpc_host.cpp`) is the one thread
  that runs this port's walks. `kcpc_host_step()` opens the walk
  (`cpc_walk_begin()`) on the reader thread and the walker closes it.
- Breakpoints for this port go through `z80_bp_put()` / `z80_bp_remove()` /
  `z80_bp_clear()` / `z80_bp_list()` (`src/z80_view.h`): one entry per kind
  and address, refused with `LimitReached` when the probe would not arm it
  (32 execution, 16 watch, 8 I/O comparators, `PROBE_MAX_*`). Every mutator
  of the lists ends in `bump_bp_generation()`, which calls the one hook
  (`z80_set_breakpoints_changed_hook()`) under the pause mutex: a new
  mutator must bump, and the hook must only post.
- Input from this port uses the paths the text IPC's `input` commands use
  and takes no pause lease: `KEY` writes the pending matrix through
  `kbd_matrix_set_line()` (`src/keyboard.h`), `TEXT` enqueues on
  `g_autotype_queue`, `MOUSE` stages through `ipc_stage_mouse()` for
  `ipc_drain_input()`. **`KEY` must not go through `ipc_apply_keypress()` or
  `applyKeypressDirect()`**: those release SHIFT and CONTROL around a plain
  key, and a frontend sends its modifiers as matrix positions of their own.
- The server (`Server::press_key()`, `release_held()`) remembers the keys
  and mouse buttons the controller holds and releases exactly those when it
  loses control, before `EVT_CONTROL_CHANGED` is posted. A new request that
  holds something down (a key, a button) must be recorded there, under
  `held_mutex_`, after re-checking the controller under that mutex.
- `z80_step_instruction()` acknowledges the probe after the step. The probe
  is ticked along with the board, so a stepped fetch at an armed address
  latches a hit nobody collects; left there, the next frame reported it as a
  breakpoint stop at an address the CPU had left.
- `KONCPC_KCPC_CONTROLLER_TIMEOUT_MS` in the environment lowers the 30 s a
  controller may stay silent, and `KONCPC_KCPC_EVENT_QUEUE_BYTES` the 4 MiB
  of queue beyond which a client's events are dropped. They exist for the
  tests.
- `src/net_socket.{h,cpp}` — non-blocking loopback sockets with timeouts.
- `test/integrated/kcpc/kcpc_client.py` is the reference client (standard
  library only); `test_kcpc.py` next to it starts its own cores
  (`-H` by default, `--ui-free`, `--gui`) and runs inside `make e2e_test`.

A test must only ever connect to ports its own core printed in its manifest.
Probing 6543–6554 to see who answers reaches the text IPC and telnet console
of any other emulator on the machine, and the telnet console types what it
receives into its CPC.

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

Cmd/Ctrl chords belong to the host UI and never reach the CPC; unmodified
keys (F-keys included) are the CPC's. The chords are resolved in one place,
`src/host_chords.h`/`.cpp`, which the SDL event loop, both menu bars and the
About box all read. SDL is the only dispatcher — the native macOS menu shows
a chord as text and registers no key equivalent, since AppKit and SDL both
see the key and an accelerator on it double-fires (the F9 lesson). The F-key
text on native menu items comes from the live InputMapper bindings
(`koncpc_action_menu_title()`), so the menu is built after the mapper exists
and re-reads the text each time it opens. On Linux
and Windows, Control is a real CPC key, so Ctrl+K/O/S are the three
combinations taken from the CPC; every other Control combination reaches it.

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

Config file locations (in order of precedence — the first one found wins):
1. `-c/--cfg_file=<path>` argument
2. `$XDG_CONFIG_HOME/koncepcja/koncepcja.cfg` (or `~/.config/koncepcja/koncepcja.cfg`)
3. `$XDG_CONFIG_HOME/koncepcja.cfg` (or `~/.config/koncepcja.cfg`), `~/.koncepcja.cfg` — legacy flat paths
4. `$CWD/koncepcja.cfg`, then `koncepcja.cfg` next to the binary
5. `/etc/koncepcja.cfg`
6. `koncepcja.cfg` in the macOS app bundle's `Resources/` (next to the binary's
   parent)

The profile config outranks a checkout-local file: running a debug-style
build from the repo root no longer picks up the untracked `koncepcja.cfg`
sitting there when `~/.config/koncepcja/koncepcja.cfg` exists. Settings ▸
System shows the file in use, as does `config get file` over IPC. The
sidecars — `imgui.ini` and the DevTools `layouts/` directory — live next to
whichever config file wins, so they move with it.

### Scripted runs and dialogs

Only a user gesture (window close, F10, the native Quit item) may raise the
"unsaved changes to a disk" dialog. IPC `quit`, SIGTERM/SIGINT,
`-E/--exit-after`, `-B/--exit-on-break` and a scripted `-a KONCPC_EXIT` quit
without asking — nobody is there to answer. A signal exits with 128+signo
(130 for Ctrl+C, 143 for SIGTERM) and does not save settings; a second signal
before shutdown finishes kills the process outright. `KONCPC_NO_DIALOGS=1`
in the environment suppresses the dialog on every path;
`test/integrated/ipc_harness.py` sets it, so a test that dirties a disk and
is then terminated can never block on a modal.

### Key Config Options

```ini
[system]
model=2           # 0=464, 1=664, 2=6128, 3=6128+
ram_size=128      # RAM in KB: 64, 128, 192, 256, 320, 512, 576 … 4160
speed=4           # Clock speed MHz
run_tier=0        # Run-tier policy: 0=auto (Fast;
                  # drops to Wake while any breakpoint/watchpoint/IO-BP is
                  # set — per-cycle observability for the debugger), 1=fast,
                  # 2=wake, 3=soldered, 4=faithful. Shift+F9 cycles
                  # auto→fast→wake at runtime; the Machine menu has the full
                  # list; `tier` on the IPC port reads/sets it. KONCPC_TIER/
                  # KONCPC_WAKE env vars pin the tier and disable the policy.

[video]
scr_scale=2       # Window scale factor
scr_style=1       # Rendering style (0-11)
scr_window=1      # 1 = start windowed, 0 = start fullscreen. NOTE the IPC key
                  # `config get|set fullscreen` has the opposite polarity
                  # (1 = fullscreen).
vsync=1           # 1=VSYNC present (default). 0=MAILBOX/IMMEDIATE on the MAIN
                  # window only (viewport/DevTools windows always stay VSYNC —
                  # IMMEDIATE breaks their swapchains). Escape hatch for the
                  # remote-desktop present stall; emulation pacing is unaffected
                  # (decoupled from render), so FPS stays 50 either way.
                  # Cost of 0: when only IMMEDIATE is available (no MAILBOX —
                  # seen on a 5K/144 Hz Mac display) the 50 Hz presents land
                  # unsynced. On a variable-refresh-rate (VRR) display that is
                  # visibly broken — frames snap back — because VRR locks the
                  # panel to the present cadence and IMMEDIATE has none. Use 1
                  # with VRR (beads-7azm).

[sound]
enabled=1         # NOTE: the INI keys are enabled/playback_rate/bits/stereo/
playback_rate=2   # volume — NOT the snd_* names of the C++ struct members.
                  # 0=11025, 1=22050, 2=44100, 3=48000, 4=96000
volume=80         # 0-100

[input]
lightgun=0        # Light gun: 0=off, 1=Amstrad Magnum Phaser, 2=Trojan Light
                  # Phazer. Same as the F-key toggle; lets config/headless runs
                  # enable a gun (the IPC `input gun` contract keys off it).
host_chords=1     # Cmd/Ctrl+K/O/S drive the host UI (palette, Load Disk A,
                  # Save Snapshot). 0 hands Ctrl+K/O/S back to the CPC on
                  # Linux/Windows — CP/M software (WordStar) uses them. No
                  # effect on macOS, where the chords use Command.
```

## Code Conventions

- Use `byte` (uint8_t), `word` (uint16_t), `dword` (uint32_t) types
- Z80 memory access: `z80_read_mem(addr)`, `z80_write_mem(addr, val)`
- Pause/resume: `cpc_pause()`, `cpc_resume()`
- A `CpcPauseLease` holder lets go with exactly one of `restore_run_state()`
  (borrowed the machine: put back what was there), `release_stopped()` (a
  stop somebody asked for: a step) or `release_running()` (promises a running
  machine: reset, the start of a frame step). Never `if (!was_paused)
  cpc_resume();` by hand: it resumes over a breakpoint that fired meanwhile
  and does nothing while another lease is held. A lease that only reads uses
  `read_alone()`.
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
