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

cd "$SDL_DIR"

# fetch latest tags and update to latest release tag (override with SDL_REF if set)
git fetch --tags origin
REF=${SDL_REF:-$(git tag -l 'release-*' | sort -V | tail -n1)}

echo "Updating SDL to $REF"

git checkout "$REF"

git status --porcelain

cd "$ROOT"

if [ -f "$PATCH_FILE" ]; then
  scripts/apply-sdl-patches.sh
else
  echo "Patch file not found: $PATCH_FILE (skipping apply)"
fi
