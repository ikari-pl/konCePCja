#!/bin/sh
# Fully static riscv64 build for the K230 board.
# The board is Buildroot glibc 2.33; the Debian container is 2.41, so a
# dynamically linked binary dies with "GLIBC_2.34 not found". Static avoids it.
#
# NOTE: do NOT set CMAKE_FIND_LIBRARY_SUFFIXES=".a" — it makes every CMake
# TryCompile probe do a full static link, which under QEMU takes longer than
# the build itself. `gcc -static` already prefers .a archives.
set -eu
JOBS="${JOBS:-6}"
BUILD=/src/build-rv64/static

cmake -S /src -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DKONCPC_BUILD_MODERN_UI=ON \
  -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_KMSDRM=ON -DSDL_ALSA=ON \
  -DSDL_VULKAN=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON \
  -DCMAKE_EXE_LINKER_FLAGS="-static"

cmake --build "$BUILD" -j "$JOBS"

B="$BUILD/koncepcja"
ls -la "$B"
echo "--- ELF type (02=ELF64, machine f3=RISCV) ---"
od -A d -t x1 -N 20 "$B"
echo "--- dynamic section (empty => static) ---"
readelf -d "$B" 2>/dev/null | head -5 || echo "(no readelf)"
