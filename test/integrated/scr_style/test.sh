#!/bin/bash

# This test just verifies that no video plugin is broken to the point of
# segfaulting systematically.
# In particular, we're not verifying the output. We cannot use screenshots to
# verify the output because screenshots are taken before the application of the
# video plugin filter.

TSTDIR=`dirname "$0"`
OUTPUT_DIR="output"
LOGFILE="test.log"
KONCPCDIR="${TSTDIR}/../../../"

if [ -z "$DIFF" ]
then
  DIFF=diff
fi

if [ -z "$SED" ]
then
  SED=sed
fi

# Pure-bash timeout: run_with_timeout <seconds> <cmd> [args...]
# Kills the process group with SIGKILL after N seconds.
# Using SIGKILL (-KILL) and targeting the process group (-$child) ensures
# that SDL/subprocess signal handlers cannot defer or ignore the kill.
run_with_timeout() {
  local seconds="$1"; shift
  "$@" &
  local child=$!
  ( sleep "$seconds" && kill -9 "$child" 2>/dev/null ) &
  local watchdog=$!
  wait "$child" 2>/dev/null
  local status=$?
  kill "$watchdog" 2>/dev/null
  wait "$watchdog" 2>/dev/null
  return $status
}

rm -rvf ${OUTPUT_DIR}
mkdir -p ${OUTPUT_DIR}
echo "" > "${LOGFILE}"

cd "$TSTDIR"

nb_plugins=`$KONCPCDIR/koncepcja -V | grep "Number of video plugins available: " | cut -d : -f 2 | xargs`
let last_plugin=${nb_plugins}-1

# No macOS CI skip list anymore.  The original skips (11-14) were for
# the legacy GL CRT plugins whose fragment shaders stalled the GitHub
# Actions macOS software Metal renderer.  Phase 7b deleted those three
# plugins; CRT Basic / Full / Lottes now live as GPU plugins (native
# Metal or SPIRV/Vulkan), and the headless macOS runners don't exercise
# a shader path that can hang.  See beads-f1p history for context.
SKIP_STYLES=""

rc=0
for style in `seq 0 $last_plugin`
do
  for skip in $SKIP_STYLES; do
    if [ "$style" = "$skip" ]; then
      echo " -- skipping scr_style=${style} (known macOS CI hang)"
      continue 2
    fi
  done
  $SED -i "s/^scr_style=.*$/scr_style=${style}/" koncepcja.cfg || rc=2
  # Autocmd sequencing: print first, then `call 0` + KONCPC_WAITBREAK to
  # synchronize on the resulting PC=0 break before KONCPC_EXIT.  Without
  # the break gate, KONCPC_EXIT fires the same frame the autocmd queue
  # finishes typing — BASIC has not yet executed the queued PRINT and
  # the printer file ends up empty.  This was the recurring macOS CI
  # flake on PRs #101/103/107/108/109/110.  See dsk/test.sh which uses
  # the same pattern.
  run_with_timeout 20 "$KONCPCDIR/koncepcja" -c koncepcja.cfg \
      -a "print #8,\"style=${style}\"" \
      -a "call 0" \
      -a KONCPC_WAITBREAK \
      -a KONCPC_EXIT >> "${LOGFILE}" 2>&1

  mv ${OUTPUT_DIR}/printer.dat ${OUTPUT_DIR}/printer.dat.${style}
  if ! $DIFF ${OUTPUT_DIR}/printer.dat.${style} model/printer.dat.${style} >> "${LOGFILE}"
  then
    echo "!! Test failed for scr_style=${style}"
    rc=1
  fi
done

if [ $rc -ne 0 ]
then
  cat "${LOGFILE}"
fi
exit $rc
