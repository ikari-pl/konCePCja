#!/bin/sh
# Unattended riscv64 validation run (Phase 1 + Tier 1 of the port plan).
# Intentionally does NOT set -e: every step is recorded so a morning reader can
# see exactly how far it got and which step failed.
#   docker run --rm --platform linux/riscv64 -v "$PWD:/src" koncepcja-rv64 \
#     tools/riscv64/overnight.sh
LOG=/src/build-rv64/overnight.log
mkdir -p /src/build-rv64
JOBS="${JOBS:-$(nproc)}"
SDL_INSTALL=/src/build-rv64/sdl-install

say(){ printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$1" | tee -a "$LOG"; }
step(){ # step <name> <cmd...>
  name="$1"; shift
  say "START  $name"
  if "$@" >>"$LOG" 2>&1; then say "OK     $name"; echo "OK $name" >> /src/build-rv64/STATUS
  else rc=$?; say "FAIL   $name (rc=$rc)"; echo "FAIL $name rc=$rc" >> /src/build-rv64/STATUS; fi
}

: > "$LOG"; : > /src/build-rv64/STATUS
say "arch=$(uname -m) jobs=$JOBS gcc=$(gcc -dumpversion)"

step "sdl3-configure" cmake -S /src/vendor/SDL -B /src/build-rv64/sdl \
  -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
  -DSDL_KMSDRM=ON -DSDL_ALSA=ON \
  -DSDL_VULKAN=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_UNIX_CONSOLE_BUILD=ON \
  -DCMAKE_INSTALL_PREFIX="$SDL_INSTALL"
step "sdl3-build"   cmake --build /src/build-rv64/sdl -j "$JOBS"
step "sdl3-install" cmake --install /src/build-rv64/sdl

PKG_CONFIG_PATH="$SDL_INSTALL/lib/pkgconfig"; export PKG_CONFIG_PATH
LD_LIBRARY_PATH="$SDL_INSTALL/lib"; export LD_LIBRARY_PATH
say "sdl3 version: $(pkg-config --modversion sdl3 2>/dev/null || echo UNKNOWN)"
say "sdl3 video drivers compiled in:"
grep -E "SDL_(KMSDRM|ALSA|VULKAN|X11|WAYLAND|OPENGL)  *\\(Wanted" "$LOG" | tail -20 >> "$LOG".drivers 2>/dev/null

# MODERN_UI stays ON: CMakeLists says OFF fails to link until P1.5.2.
step "koncepcja-configure" cmake -S /src -B /src/build-rv64/kon \
  -DCMAKE_BUILD_TYPE=Release -DKONCPC_BUILD_MODERN_UI=ON \
  -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_KMSDRM=ON -DSDL_ALSA=ON \
  -DSDL_VULKAN=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF
step "koncepcja-build" cmake --build /src/build-rv64/kon -j "$JOBS"

BIN=$(find /src/build-rv64/kon -maxdepth 2 -type f -name 'koncepcja*' -perm -u+x 2>/dev/null | head -1)
say "binary: ${BIN:-NOT FOUND}"
if [ -n "$BIN" ]; then
  say "file: $(file -b "$BIN" 2>/dev/null)"
  step "headless-smoke" sh -c "SDL_VIDEODRIVER=dummy timeout 60 '$BIN' --headless --help"
fi

step "ctest" sh -c "cd /src/build-rv64/kon && ctest --output-on-failure --no-tests=error -j $JOBS"

say "=== STATUS ==="; /bin/cat /src/build-rv64/STATUS | tee -a "$LOG"
say "DONE"
