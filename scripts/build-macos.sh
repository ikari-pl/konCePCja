#!/bin/sh
# Build konCePCja for macOS with maximum parallelism.
# Usage: scripts/build-macos.sh [extra make args]
set -e
JOBS=$(sysctl -n hw.ncpu)
exec make -j"$JOBS" ARCH=macos "$@"
