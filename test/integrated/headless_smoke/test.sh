#!/bin/sh
# Runs the -H/--headless smoke (headless_smoke.py) as part of `make e2e_test`.
# run_tests.sh globs */test.sh, so this directory is all the wiring it needs.
#
# The IPC suite (../ipc) drives the GUI build under SDL's dummy driver, which
# still runs the Z80 thread + render thread. This one runs the single-threaded
# -H loop that an out-of-process frontend talks to (beads-cv2), so a break in
# that loop fails CI instead of surfacing only in a KONCPC_E2E_HEADLESS run.
set -e

PYTHON=${PYTHON:-python3}
if ! command -v "$PYTHON" >/dev/null 2>&1; then
  # Fail closed, same reasoning as ../ipc/test.sh.
  if [ "${KONCPC_ALLOW_SKIP_IPC:-0}" = "1" ]; then
    echo "SKIP: no $PYTHON on PATH (KONCPC_ALLOW_SKIP_IPC=1)"
    exit 0
  fi
  echo "FAIL: no $PYTHON on PATH -- the headless smoke cannot run"
  exit 1
fi

cd "$(dirname "$0")/../../.." || exit 1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy exec "$PYTHON" test/integrated/headless_smoke.py
