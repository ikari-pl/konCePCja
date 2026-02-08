<img alt="konCePCja logo" width="300" src="resources/koncepcja-logo.png" />

# konCePCja — Amstrad CPC Emulator

&copy; Copyright 1997–2015 Ulrich Doewich \
&copy; Copyright 2016–2025 Colin Pitrat \
&copy; 2026 Cezar "ikari" Pokorski

https://github.com/ikari-pl/konCePCja

![Linux](https://github.com/ikari-pl/konCePCja/actions/workflows/linux.yml/badge.svg) ![Windows](https://github.com/ikari-pl/konCePCja/actions/workflows/windows.yml/badge.svg) ![macOS](https://github.com/ikari-pl/konCePCja/actions/workflows/macos.yml/badge.svg)

## What is it?

konCePCja is a software emulator of the Amstrad CPC 8-bit home computer series, running on Linux, macOS and Windows. It faithfully imitates the CPC464, CPC664 and CPC6128 models. By recreating the operations of all hardware components at a low level, the emulator achieves a high degree of compatibility with original CPC software. Programs and games run unmodified at real-time or higher speeds, depending on the host environment.

## Changes vs Caprice32

konCePCja is a fork of [Caprice32](https://github.com/ColinPitrat/caprice32) with modernized tooling and UI integration.
Key differences so far:
  * SDL3 migration + macOS menu integration
  * Project rename, bundle ID and updated defaults/paths
  * PNG logo + macOS icns icon
  * **Headless mode** (`--headless`) — run without a window for CI and automation
  * **IPC protocol** — TCP server on port 6543 for remote control by scripts and LLM agents
  * **Input replay** — type text, press keys and control joysticks over IPC
  * **Frame stepping** — advance exact frame counts for deterministic testing
  * **Instruction trace** — ring-buffer Z80 execution trace with dump to file
  * **Frame dumps** — save sequential PNG screenshots for animation/regression
  * **Event system** — fire IPC commands on PC match, memory write or VBL interval
  * **Hash commands** — CRC32 of VRAM, memory ranges and registers for CI assertions
  * **Exit control** — `--exit-after`, `--exit-on-break` and `quit` for scripted runs

See [docs/ipc-protocol.md](docs/ipc-protocol.md) for the full IPC command reference.

## Features

  * Complete emulation of CPC464, CPC664 and CPC6128
  * Mostly working Plus Range support: CPC464+/CPC6128+/GX4000 (missing vectored & DMA interrupts, analog joysticks and 8-bit printer)
  * Joystick support — fully usable with joystick only, thanks to an integrated virtual keyboard
  * Joystick emulation — joystick-only games can be played using the keyboard
  * English, French or Spanish keyboards
  * DSK, [IPF](http://softpres.org/glossary:ipf) and CT-RAW files for disks — VOC and CDT files for tapes — CPR files for cartridges
  * Snapshots (SNA files)
  * Direct load of ZIP files
  * Developer tools: debugger, memory editor, disassembler
  * Custom disk formats
  * Printer support
  * Experimental Multiface 2 support (prefer the memory tool where possible)

Something missing? Open an issue to suggest it.

## Installation

### macOS

See [INSTALL.md](INSTALL.md).

### Linux

#### From Git

```
git clone --recurse-submodules https://github.com/ikari-pl/konCePCja.git
cd konCePCja
make APP_PATH="$PWD"
./koncepcja
```

> **Note:** A plain `make` without `APP_PATH` produces a debug-oriented build that looks for `koncepcja.cfg` in the current working directory, not next to the executable. Set `APP_PATH` to get the documented behaviour.

#### From releases

Download a release from https://github.com/ikari-pl/konCePCja/releases, decompress it and run:

```
./koncepcja
```

### Windows

Download a release from https://github.com/ikari-pl/konCePCja/releases, decompress it and double-click `koncepcja.exe`.

## Usage

See the [manual page](https://htmlpreview.github.io/?https://github.com/ikari-pl/konCePCja/blob/master/doc/man.html) for details. If you are lost, launch the emulator without arguments and press **F1** for the in-emulator menu.

## Building

See [INSTALL.md](INSTALL.md) for build instructions.

## License

The source for konCePCja is distributed under the terms of the GNU General Public License version 2 (GPLv2), included in this archive as `COPYING.txt`.

The screen-capture code uses [driedfruit SDL_SavePNG](https://github.com/driedfruit/SDL_SavePNG), released under the zlib/libpng license (compatible with GPLv2).

The bundled ROM images in `rom/` are &copy; Amstrad plc and Locomotive Software Ltd, redistributed with permission.
Amstrad have kindly given their permission for the redistribution of their copyrighted material but retain that copyright.
See [`rom/ROM-LICENSE.txt`](rom/ROM-LICENSE.txt) for full details including the Multiface II ROM.

## Contributing

Bug reports, feature suggestions and pull requests are welcome — just open an issue or submit a PR.
