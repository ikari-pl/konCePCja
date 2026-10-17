#!/bin/sh
# Runs the KCPC binary-protocol suite (test_exit_criterion.py, test_kcpc.py)
# as part of `make e2e_test`. run_tests.sh globs */test.sh, so this directory
# is all the wiring it needs.
#
# test_kcpc.py twice, because the server sits beside two different emulation loops: under
# -H the main thread runs the frames, in the GUI build a Z80 thread does while
# a render thread draws. Under SDL's dummy drivers the GUI build still runs
# both threads, so --gui exercises the pause lease against the threaded loop.
set -e

PYTHON=${PYTHON:-python3}
if ! command -v "$PYTHON" >/dev/null 2>&1; then
  # Fail closed, same reasoning as ../ipc/test.sh.
  if [ "${KONCPC_ALLOW_SKIP_IPC:-0}" = "1" ]; then
    echo "SKIP: no $PYTHON on PATH (KONCPC_ALLOW_SKIP_IPC=1)"
    exit 0
  fi
  echo "FAIL: no $PYTHON on PATH -- the KCPC suite cannot run"
  exit 1
fi

cd "$(dirname "$0")/../../.." || exit 1
# The exit criterion of phase 3 first: one test that says the port does what
# it is for (docs/protocol/kcpc-binary-v1.md, section 16).
"$PYTHON" test/integrated/kcpc/test_exit_criterion.py
"$PYTHON" test/integrated/kcpc/test_kcpc.py
"$PYTHON" test/integrated/kcpc/test_kcpc.py --gui
