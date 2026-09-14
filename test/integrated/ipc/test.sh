#!/bin/sh
# Runs the IPC harness as part of `make e2e_test`, which is what CI invokes.
#
# The harness existed for a long time without ever being wired in: run_tests.sh
# globs */test.sh, and nothing invoked ipc_harness.py. Its step-out regression
# tests therefore gated nothing, which is how a step-out that stopped one
# instruction short of the RET shipped with CI fully green.
#
# The harness spawns and tears down its own emulator per test and locates the
# binary relative to its own path, so it does not care about the working
# directory. Headless drivers keep it usable on a CI box with no display.
set -e

PYTHON=${PYTHON:-python3}
if ! command -v "$PYTHON" >/dev/null 2>&1; then
  # Fail closed. This suite exists because a harness that gates nothing let a
  # real step-out bug ship through green CI; silently passing when the
  # interpreter is missing would recreate exactly that. Set
  # KONCPC_ALLOW_SKIP_IPC=1 to opt out deliberately.
  if [ "${KONCPC_ALLOW_SKIP_IPC:-0}" = "1" ]; then
    echo "SKIP: no $PYTHON on PATH (KONCPC_ALLOW_SKIP_IPC=1)"
    exit 0
  fi
  echo "FAIL: no $PYTHON on PATH -- the IPC suite cannot run"
  exit 1
fi

# Run from the project root: the emulator resolves rom/ and resources/
# relative to its working directory, and run_tests.sh invokes us from this
# subdirectory.
cd "$(dirname "$0")/../../.." || exit 1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy exec "$PYTHON" test/integrated/ipc_harness.py
