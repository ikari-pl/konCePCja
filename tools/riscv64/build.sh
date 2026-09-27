#!/bin/sh
# Build konCePCja for riscv64 inside the container. Mirrors .github/workflows/linux.yml.
# Usage (from repo root):
#   docker run --rm --platform linux/riscv64 -v "$PWD:/src" koncepcja-rv64 tools/riscv64/build.sh
set -eu
JOBS="${JOBS:-$(nproc)}"
SDL_INSTALL=/src/build-rv64/sdl-install

echo "== arch: $(uname -m) =="

if [ ! -f "$SDL_INSTALL/lib/pkgconfig/sdl3.pc" ]; then
  echo "== building vendored SDL3 (KMSDRM + ALSA, no Vulkan/X11/Wayland) =="
  cmake -S /src/vendor/SDL -B /tmp/sdl-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
    -DSDL_KMSDRM=ON -DSDL_ALSA=ON \
    -DSDL_VULKAN=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_UNIX_CONSOLE_BUILD=ON \
    -DCMAKE_INSTALL_PREFIX="$SDL_INSTALL"
  cmake --build /tmp/sdl-build -j "$JOBS"
  cmake --install /tmp/sdl-build
else
  echo "== SDL3 already installed, skipping =="
fi

export PKG_CONFIG_PATH="$SDL_INSTALL/lib/pkgconfig"
export LD_LIBRARY_PATH="$SDL_INSTALL/lib"

echo "== configuring konCePCja =="
# MODERN_UI stays ON: CMakeLists documents that OFF fails to link until P1.5.2.
cmake -S /src -B /src/build-rv64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DKONCPC_BUILD_MODERN_UI=ON

echo "== building =="
cmake --build /src/build-rv64 -j "$JOBS"
echo "== done =="
ls -la /src/build-rv64/koncepcja 2>/dev/null || echo "(binary name differs — check build-rv64/)"
