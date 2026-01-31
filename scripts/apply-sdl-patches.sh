#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDL_DIR="$ROOT/vendor/SDL"
PATCH_DIR="$ROOT/vendor/SDL/patches"
PATCH_FILE="$PATCH_DIR/cap32-menu.patch"

if [ ! -d "$SDL_DIR" ]; then
  echo "SDL submodule not found at $SDL_DIR" >&2
  exit 1
fi

mkdir -p "$PATCH_DIR"

if [ ! -f "$PATCH_FILE" ]; then
  echo "Patch file not found: $PATCH_FILE" >&2
  exit 1
fi

cd "$SDL_DIR"

git apply --check "$PATCH_FILE"
if git apply "$PATCH_FILE"; then
  echo "Applied SDL patch: $PATCH_FILE"
fi
