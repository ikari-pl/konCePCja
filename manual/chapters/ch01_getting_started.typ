#import "../template.typ": *
#set-chapter("Getting Started")

= Getting Started

#intro[
  This chapter takes you from nothing to a running Amstrad CPC: what konCePCja
  is, what you need, how to build it on your platform, and how to load your
  first piece of software. If you have already built and launched the emulator,
  skip ahead to #emph[Loading Software] on page #pref(<sec-loading>).
]

== What is konCePCja?

#idx("konCePCja")konCePCja is an emulator of the Amstrad CPC range of 8-bit home
computers. It is built on the well-established Caprice32 emulator core, extended
with a modern SDL3 video and audio backend and a Dear ImGui developer interface.

It runs on macOS, Linux, and Windows (the latter via the MinGW toolchain), and
emulates the whole classic CPC line as well as the Plus range:

- #idx("CPC 464")*CPC 464* --- cassette-based, 64 KB RAM
- *CPC 664* --- disc-based, 64 KB RAM
- *CPC 6128* --- disc-based, 128 KB RAM (the default)
- #idx("CPC 6128+")*CPC 6128+* --- the Plus machine, with ASIC hardware sprites,
  an enhanced palette, DMA sound channels, and cartridge support

What sets konCePCja apart from other CPC emulators is its emphasis on
inspection and automation: a full #idx("DevTools")developer-tools panel (see
#fkey[F12]), a scriptable TCP debugging interface, a telnet console, and an
emulated M4 Board with a web file manager. These power-user features are
documented in the later chapters.

== System Requirements

konCePCja is lightweight by modern standards. The emulated machine is an 8-bit
computer; the host requirements come almost entirely from the build toolchain
and the windowing layer.

- *macOS* --- a recent macOS release on either Apple Silicon or Intel. Homebrew
  is used to install the build dependencies.
- *Linux* --- any reasonably current distribution with a C++17 compiler.
- *Windows* --- an #idx("MSYS2")MSYS2 / MinGW environment.
- *Hardware* --- a few hundred megabytes of RAM and an OpenGL-capable GPU. A
  software-render fallback exists for headless and remote use.

== Installation

konCePCja is built from source. The build needs a C++17 compiler, plus SDL3
(vendored as a git submodule or supplied by your package manager), FreeType,
zlib, libPNG, and CMake. Always clone with submodules:

```
git clone --recurse-submodules https://github.com/ikari-pl/konCePCja.git
cd konCePCja
```

#note[
  The `APP_PATH="$PWD"` argument shown below tells konCePCja where to look for
  its `koncepcja.cfg` configuration file at runtime. Without it, the emulator
  uses the current working directory. See Chapter 4 for configuration details.
]

=== macOS

Install the dependencies with Homebrew, build the vendored SDL3, then build the
emulator:

```
brew install freetype zlib libpng cmake
cd vendor/SDL && cmake -B build -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON \
  -DSDL_STATIC=OFF -DCMAKE_INSTALL_PREFIX=$(pwd)/install \
  && cmake --build build -j$(sysctl -n hw.ncpu) && cmake --install build && cd ../..
make ARCH=macos APP_PATH="$PWD" -j$(sysctl -n hw.ncpu)
./koncepcja
```

To produce a double-clickable application bundle (`.dmg`), run
#cmd[make ARCH=macos macos_bundle].

=== Linux

Install the build dependencies, then build SDL3 and the emulator:

```
sudo apt-get install g++ make pkg-config cmake \
  libfreetype6-dev zlib1g-dev libpng-dev libgl-dev
cd vendor/SDL && cmake -B build -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON \
  -DSDL_STATIC=OFF -DCMAKE_INSTALL_PREFIX=$(pwd)/install \
  && cmake --build build -j$(nproc) && cmake --install build && cd ../..
PKG_CONFIG_PATH=vendor/SDL/install/lib/pkgconfig \
  LD_LIBRARY_PATH=vendor/SDL/install/lib make APP_PATH="$PWD" -j$(nproc)
./koncepcja
```

If your distribution already ships SDL3 (Ubuntu 25.04 and later), you can skip
the submodule build:

```
sudo apt-get install libsdl3-dev
make APP_PATH="$PWD" -j$(nproc)
```

Useful build options: #cmd[make debug] (or `DEBUG=TRUE`) for a debug build,
`WITHOUT_GL=TRUE` to build without OpenGL.

=== Windows (MSYS2 / MinGW)

Install MSYS2 from #link("https://www.msys2.org")[msys2.org] and build from a
MinGW shell. konCePCja's continuous integration builds both the MINGW32 and
MINGW64 targets, so either works.

== First Launch

Launch konCePCja with no arguments and it boots straight to the default machine
--- a CPC 6128 with 128 KB of RAM:

```
./koncepcja
```

You are greeted by the CPC firmware sign-on and the Locomotive BASIC prompt:

```
Amstrad 128K Microcomputer  (v3)
...
BASIC 1.1
Ready
```

#idx("Ready prompt")The word #cmd[Ready] followed by a flashing cursor means the
machine is waiting for a BASIC command. This is a fully working CPC: type
#cmd[PRINT "HELLO"] and press #fkey[RETURN] to see it respond.

#figure(image("../images/main-display.png", width: 78%), caption: [konCePCja at the Locomotive BASIC 1.1 prompt, ready for input])

Press #fkey[F1] at any time to open the in-emulator menu (it pauses emulation),
or #fkey[F12] to open the developer tools. The complete function-key reference
is in Chapter 2.

To start on a different machine --- for example the Plus --- override the model
on the command line (model `3` is the 6128+):

```
./koncepcja -O system.model=3
```

== Loading Software

#anchor(<sec-loading>)#idx("loading software")konCePCja loads the standard CPC
media formats. The quickest way is to pass a file on the command line, or drag
it onto the window; both also work through #menu-path("File", "Load").

=== Disc images (DSK)

The most common format. Load and auto-run a disc in one step with the
#cmd[--autocmd] (`-a`) option:

```
./koncepcja -a 'run"game' game.dsk
```

Or load it and run by hand from BASIC with #cmd[run\"game].

=== Tape files (CDT / VOC)

Insert a tape image, then in BASIC select the tape, issue #cmd[LOAD\"\"], and
start playback with #fkey[F4] (tape play/pause):

```
|TAPE
LOAD""
```

=== Cartridge files (CPR)

Cartridges are a Plus-range feature. Pass a `.cpr` file on a 6128+ machine
(#cfg-key[system.model] `= 3`) and it boots directly from the cartridge.

=== Snapshots (SNA)

A snapshot is a frozen machine state --- RAM, registers, and hardware --- that
resumes instantly. Load one from the command line or via
#menu-path("File", "Load"); save the current state with #fkey[Shift+F3].

=== ZIP archives

konCePCja can also open a `.zip` archive containing CPC media (DSK, CDT, or SNA)
and load the appropriate file from inside it, so you do not have to unpack
downloads by hand.

== Command-line options

#idx("command-line options")Run #cmd[koncepcja --help] for the full list. The
options you will reach for most:

#table(
  columns: (auto, 1fr), stroke: 0.4pt + rule-grey, inset: 5pt,
  [*Option*], [*Effect*],
  [`-a`, `--autocmd=<cmd>`], [Run a BASIC command once the CPC has booted],
  [`-c`, `--cfg_file=<file>`], [Use a specific configuration file],
  [`-i`, `--inject=<file>`], [Inject a binary into memory after boot],
  [`-o`, `--offset=<addr>`], [Injection address (default #port[0x6000])],
  [`-O`, `--override=<opt=val>`], [Override one config option (repeatable)],
  [`-s`, `--sym_file=<file>`], [Load symbols for the disassembler],
  [`-v`, `--verbose`], [Verbose logging],
  [`-V`, `--version`], [Show the version and exit],
)

So, for example, to boot a 6128+, inject a loader at #port[0x4000], and auto-run
a disc:

```
./koncepcja -O system.model=3 -i loader.bin -o 0x4000 -a 'run"game' game.dsk
```

== Troubleshooting

#idx("troubleshooting")*The build fails immediately, complaining about SDL.* You
almost certainly cloned without submodules. Either re-clone with
#cmd[git clone --recurse-submodules], or fetch them in place with
#cmd[git submodule update --init --recursive], then build the vendored SDL3 as
shown above (or install your distribution's `libsdl3-dev`).

*On macOS, a crash dialog mentions "reopening windows" at startup.* This is
stale macOS saved-application state, not a konCePCja bug. Clear it:

```
rm -rf ~/Library/Saved\ Application\ State/*koncepcja*
```

*The window is black or OpenGL fails to start.* Build or run with
`WITHOUT_GL=TRUE` for the software-render fallback; this also lets konCePCja run
on machines and remote sessions without a usable GPU.

*A disc or tape image is rejected.* Malformed images are refused rather than
crashing the emulator; run with #cmd[--verbose] and look for an `ERROR` line
naming the problem.
