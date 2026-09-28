#!/bin/sh
# Fully static riscv64 build for the K230 board.
#
# Why static: the board is Buildroot glibc 2.33, the Debian container is 2.41,
# so a dynamic binary dies with "GLIBC_2.34 not found".
#
# Two traps, both hit once already:
#  1. Do NOT set CMAKE_FIND_LIBRARY_SUFFIXES=".a" to force archives. It makes
#     every CMake TryCompile probe perform a full static link; under QEMU the
#     configure phase then outlasts the whole build. Point the individual
#     find_package results at their .a files instead (below).
#  2. find_package(Freetype|PNG|ZLIB) otherwise resolves to .so and the link
#     fails with "attempted static link of dynamic object". Note the value that
#     reaches the link line is <PKG>_LIBRARY_RELEASE, not <PKG>_LIBRARY.
#  3. SDL_KMSDRM=OFF. We present through src/drm_present.cpp, not SDL, and
#     SDL's KMSDRM driver is built SHARED: it dlopens libdrm.so.2/libgbm.so.1
#     at video-init time, which in a static binary faults against the board's
#     glibc 2.33 (SIGSEGV at 0x28 in libdl-2.33.so). Not building it removes
#     the dlopen entirely.
#  4. SDL_DYNAMIC_API=0 is required. SDL_dynapi.c calls dlopen so a different
#     SDL can be swapped in at runtime; in a static binary that pulls the
#     host's libdl and segfaults against the board's older glibc.
set -eu
JOBS="${JOBS:-6}"

# SDL's dynamic API calls dlopen so a different SDL can be swapped in at
# runtime. In a static binary that pulls the host's libdl and segfaults against
# the board's older glibc (observed: SIGSEGV at 0x28 in libdl-2.33.so).
# SDL_dynapi.h deliberately rejects -DSDL_DYNAMIC_API=0 from the command line
# ("Nope, you have to edit this file to force this off"), so flip the first
# arm of its platform chain instead. Idempotent.
DYNAPI=/src/vendor/SDL/src/dynapi/SDL_dynapi.h
if ! grep -q 'koncepcja: forced off' "$DYNAPI"; then
  sed -i 's|^#if defined(SDL_PLATFORM_PRIVATE).*$|#if 1 // koncepcja: forced off for static builds (dlopen breaks on foreign glibc)|' "$DYNAPI"
  echo "patched SDL_dynapi.h"
fi
BUILD=/src/build-rv64/static
LIBDIR=/usr/lib/riscv64-linux-gnu

cmake -S /src -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DKONCPC_BUILD_MODERN_UI=ON \
  -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_KMSDRM=OFF -DSDL_ALSA=ON \
  -DSDL_VULKAN=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON \
  -DFREETYPE_LIBRARY_RELEASE="$LIBDIR/libfreetype.a" \
  -DPNG_LIBRARY_RELEASE="$LIBDIR/libpng16.a" \
  -DZLIB_LIBRARY_RELEASE="$LIBDIR/libz.a" \
  -DCMAKE_EXE_LINKER_FLAGS="-static" \
  -DCMAKE_CXX_STANDARD_LIBRARIES="-lbz2 -lbrotlidec -lbrotlicommon"

# Only the emulator target: test_runner is not needed on the board and its
# link failure previously masked the real result.
cmake --build "$BUILD" --target koncepcja -j "$JOBS"

B="$BUILD/koncepcja"
ls -la "$B"
echo "--- ELF header (02=ELF64, f3=EM_RISCV) ---"
od -A d -t x1 -N 20 "$B"
echo "--- dynamic section (empty => static) ---"
readelf -d "$B" 2>/dev/null | head -4 || true
