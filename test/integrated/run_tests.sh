#!/bin/bash

OK=0
TOTAL=0

echo "Starting integrated tests"

DIFF=diff
if [ -f "/c/cygwin/bin/diff.exe" ]
then
	DIFF=/c/cygwin/bin/diff.exe
fi
export DIFF=$DIFF
echo "Using diff: $DIFF"

SED=sed
if ! sed --version | grep GNU
then
  SED=gsed
fi
export SED=$SED
echo "Using sed: $SED"

# Hard per-test cap: a hung emulator must FAIL its test, not stall the whole
# job until the CI timeout kills it. GNU `timeout` on Linux; `gtimeout`
# (coreutils) on macOS; unbounded only if neither exists.
TIMEOUT=""
for t in timeout gtimeout
do
  if command -v "$t" >/dev/null 2>&1
  then
    TIMEOUT="$t ${E2E_TEST_TIMEOUT:-300}"
    break
  fi
done
echo "Using per-test timeout: ${TIMEOUT:-none}"

for tst in */test.sh
do
  TSTDIR=`dirname "${tst}"`
  TSTNAME=`basename "$TSTDIR"`
  pushd "${TSTDIR}" >/dev/null

  echo ""
  echo " ** Running ${TSTNAME}: "
  BEGIN=`date +%s`
  result="FAIL"
  # $TIMEOUT is intentionally unquoted: empty -> no wrapper, else "timeout N".
  if $TIMEOUT sh test.sh
  then
    let OK=$OK+1
    result="PASS"
  fi
  END=`date +%s`
  let TIMING=$END-$BEGIN
  echo " => ${TSTNAME}: ${result} (${TIMING}s)"

  let TOTAL=$TOTAL+1
  popd >/dev/null
done

echo ""
echo -n "Integrated tests result: "
if [ $TOTAL -eq $OK ]
then
  echo "PASSED"
  exit 0
else
  echo "FAILED ($((100*$OK/$TOTAL))%)"
  exit 1
fi
