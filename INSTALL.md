# Prerequisites

You will need the following to successfully compile an executable:

  * MinGW (only for Windows) — via [MSYS2](https://www.msys2.org)
  * [SDL3](https://libsdl.org/) — vendored as a git submodule, or installed via your package manager
  * [FreeType](https://www.freetype.org/)
  * [zLib](http://www.gzip.org/zlib/)
  * [libPNG](http://libpng.org/pub/png/libpng.html)
  * CMake (to build the vendored SDL3)

# Compiling

## Linux

Install the build dependencies:

```
sudo apt-get install g++ make pkg-config cmake libfreetype6-dev zlib1g-dev libpng-dev libgl-dev
```

Clone and build:

```
git clone --recurse-submodules https://github.com/ikari-pl/konCePCja.git
cd konCePCja
cd vendor/SDL && cmake -B build -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF && cmake --build build -j$(nproc) && cd ../..
PKG_CONFIG_PATH=vendor/SDL/build make APP_PATH="$PWD" -j$(nproc)
./koncepcja
```

Or, if your distribution ships `libsdl3-dev` (Ubuntu 25.04+):

```
sudo apt-get install libsdl3-dev
make APP_PATH="$PWD" -j$(nproc)
```

### Build options

 * `DEBUG=TRUE` or `make debug` — debug build
 * `WITHOUT_GL=TRUE` — build without OpenGL support
 * `APP_PATH=<path>` — where konCePCja looks for `koncepcja.cfg` at runtime (without this, it uses the current working directory)

### Debian/Ubuntu package

Install additional packaging tools:

```
sudo apt-get install dpkg-dev devscripts fakeroot debhelper
```

Then build with `make VERSION=<version>`, go to `release/koncepcja_linux/koncepcja-<version>/debian` and run `debuild -us -uc`.

## macOS

Install dependencies via Homebrew:

```
brew install freetype zlib libpng cmake
```

Clone and build:

```
git clone --recurse-submodules https://github.com/ikari-pl/konCePCja.git
cd konCePCja
cd vendor/SDL && cmake -B build -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF && cmake --build build -j$(sysctl -n hw.ncpu) && cd ../..
make ARCH=macos APP_PATH="$PWD" -j$(sysctl -n hw.ncpu)
./koncepcja
```

To build a `.dmg` bundle:

```
make ARCH=macos macos_bundle
```

## Windows (MSYS2)

Download and install [MSYS2](https://www.msys2.org). Open the **MINGW64** shell and install dependencies:

```
pacman -S git make zip mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-freetype mingw-w64-x86_64-libpng mingw-w64-x86_64-zlib \
  mingw-w64-x86_64-pkg-config mingw-w64-x86_64-SDL3
```

Clone and build:

```
git clone --recurse-submodules https://github.com/ikari-pl/konCePCja.git
cd konCePCja
make ARCH=win64 -j4
./koncepcja.exe
```

For 32-bit (i686), SDL3 must be built from source as there is no MSYS2 package:

```
# In a MINGW32 shell
pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-freetype \
  mingw-w64-i686-libpng mingw-w64-i686-zlib mingw-w64-i686-pkg-config
cd vendor/SDL && cmake -B build -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF && cmake --build build -j4 && cd ../..
PKG_CONFIG_PATH=vendor/SDL/build make ARCH=win32 -j4
```

# IPF disk image support

To use [IPF](http://softpres.org/glossary:ipf) disk images, you need the CAPS/IPF library.

### Linux/macOS

```
tar -xvzf ipflib42_linux-x86_64.tar.gz
cd x86_64-linux-gnu-capsimage/
sudo cp libcapsimage.so.4.2 /usr/lib
sudo ln -s /usr/lib/libcapsimage.so.4.2 /usr/lib/libcapsimage.so.4
sudo ln -s /usr/lib/libcapsimage.so.4.2 /usr/lib/libcapsimage.so
sudo cp -r include/caps /usr/include
```

### Windows

The IPF DLL is included in the Windows release packages.

# Using the emulator

Press **F1** for the main menu. The *About* section lists keyboard shortcuts.

Command line flags: `koncepcja --help`

From the F1 menu, use **Load/Save** to:
 - Load a disk, tape, cartridge or snapshot image
 - Insert a new (empty) disk
 - Save a disk or emulator snapshot

By default konCePCja emulates a CPC 6128. To see what is on a disk loaded in drive A, type `cat`. To start a program, type `run"program`.

More resources: [CPCWiki User Manual](https://www.cpcwiki.eu/index.php/User_Manual)
