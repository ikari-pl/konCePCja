#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDL_DIR="$ROOT/vendor/SDL"
BUILD_DIR="$SDL_DIR/build"

if [ ! -d "$SDL_DIR" ]; then
  echo "SDL submodule not found at $SDL_DIR" >&2
  exit 1
fi

cmake -S "$SDL_DIR" -B "$BUILD_DIR" \
  -DSDL_SHARED=ON \
  -DSDL_STATIC=ON \
  -DSDL_TEST=OFF \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD_DIR" -j

