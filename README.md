<img alt="konCePCja logo" width="300" src="resources/koncepcja-logo.png" />

# konCePCja - Amstrad CPC Emulator

(c) Copyright 1997-2015 Ulrich Doewich

(c) Copyright 2016-2025 Colin Pitrat

(c) 2026 Cezar "ikari" Pokorski

https://github.com/ikari/konCePCja

Linux build: 
![Linux build Status](https://github.com/ikari/konCePCja/actions/workflows/linux.yml/badge.svg?branch=latest)

Windows build:
![Windows build Status](https://github.com/ikari/konCePCja/actions/workflows/windows.yml/badge.svg?branch=latest)

MacOS build:
![MacOS build status](https://github.com/ikari/konCePCja/actions/workflows/macos.yml/badge.svg?branch=latest)

# What is it ?

konCePCja is a software emulator of the Amstrad CPC 8-bit home computer series running on Linux, macOS and Windows. The emulator faithfully imitates the CPC464, CPC664, and CPC6128 models. By recreating the operations of all hardware components at a low level, the emulator achieves a high degree of compatibility with original CPC software. These programs or games can be run unmodified at real-time or higher speeds, depending on the emulator host environment.

# Changes vs Caprice32

konCePCja is a fork of Caprice32 with modernized tooling and UI integration.
Key differences so far:
  * SDL3 migration + macOS menu integration
  * Project rename, bundle ID, and updated defaults/paths
  * PNG logo + macOS icns icon
  * Ongoing work on IPC/debug hooks for automation

# Features

konCePCja provides:
  * Complete emulation of CPC464, CPC664 and CPC6128
  * Mostly working support of Plus Range: CPC464+/CPC6128+/GX4000 (missing vectored & DMA interrupts, analog joysticks and 8 bit printer)
  * Joystick support - it can be fully used with joystick only, thanks to an integrated virtual keyboard.
  * Joystick emulation - joystick-only games can be played using the keyboard
  * English, French or Spanish keyboards
  * DSK, [IPF](http://softpres.org/glossary:ipf) and CT-RAW files for disks - VOC and CDT files for tapes - CPR files for cartridge
  * Snapshots (SNA files)
  * Direct load of ZIP files
  * Developers' tools with debugger, memory editor, disassembler...
  * Custom disk formats
  * Printer support
  * Experimental support of Multiface 2 (you should prefer using memory tool)

You see something missing ? Do not hesitate to open an issue to suggest it.

# Installation

## macOS

See the [INSTALL.md](INSTALL.md)

## Linux

### Debug behavior and release behavior when locating configuration file

If you compile konCePCja yourself with plain make, behavior is
debug-oriented.  By default at run-time it will look for `koncepcja.cfg`
in the *current directory of the process* that launches it, not in the
executable location as stated in the documentation.  To get the
documented behavior, use `APP_PATH` like in the examples below.

### From Git

```
git clone https://github.com/ikari/konCePCja.git
cd konCePCja
make APP_PATH="$PWD"
./koncepcja
```

### From releases

Download a release from https://github.com/ikari/konCePCja/releases.
Decompress it and then from a terminal in the resulting directory:

```
./koncepcja
```

### Snap

A SNAP (maintained by a third party) is available at https://snapcraft.io/konCePCja.

## Windows

Download a release from https://github.com/ikari/konCePCja/releases.
Decompress it and double click on koncepcja.exe

# Basic usage

See the [manual page](http://htmlpreview.github.io/?https://github.com/ikari/konCePCja/blob/master/doc/man.html) for more details. If you are really lost, you can simply invoke the emulator without any argument, then press F1 to get the in-emulator menu.

# Help needed

Maintaining konCePCja is a lot of work and you can help with it.
You can:
  * Use it, show it, talk and write about it
  * Thank the maintainer
  * Report any bug or missing feature
  * Write some documentation
  * Package it for your favourite distribution (if not yet available)
  * Port it to iOS so that Mac users can enjoy it too

# Building and compiling

See the [INSTALL.md](INSTALL.md) files for konCePCja build instructions.

# Using the source

The source for konCePCja is distributed under the terms of the GNU General Public License version 2 (GPLv2), which is included in this archive as COPYING.txt. Please make sure that you understand the terms and conditions of the license before using the source.

The screen dump part of konCePCja uses [driedfruit SDL_SavePNG](https://github.com/driedfruit/SDL_SavePNG) code, released under zlib/libpng license, which is compatible with GPLv2.

I encourage you to get involved in the project.

# Comments or ready to contribute?

If you have suggestions, a bug report or even want to participate to the development, please feel free to open an issue or submit a pull request.

# Why another GitHub repository ?

There are many repositories for koncepcja on GitHub:

  * https://github.com/burzumishi/koncepcja
  * https://github.com/rofl0r/koncepcja
  * https://github.com/MrZammler/koncepcja
  * https://github.com/burzumishi/koncepcjawx

So why create another one ? All these repositories are highly inactive. The ones that touched the code added dependencies (wxWidget, GTK) without really adding features.
