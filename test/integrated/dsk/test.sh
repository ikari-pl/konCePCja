#!/bin/bash

TSTDIR=`dirname "$0"`
OUTPUT_DIR="output"
LOGFILE="test.log"
KONCPCDIR="${TSTDIR}/../../../"

if [ -z "$DIFF" ]
then
  DIFF=diff
fi

rm -rvf ${OUTPUT_DIR}
mkdir -p ${OUTPUT_DIR}
echo "" > "${LOGFILE}"

cd "$TSTDIR"

$KONCPCDIR/koncepcja -c koncepcja.cfg -a "run\"hello" -a "call 0" -a KONCPC_WAITBREAK -a KONCPC_EXIT hello.zip >> "${LOGFILE}" 2>&1

if $DIFF output/printer.dat expected.dat >> "${LOGFILE}"
then
  exit 0
else
  cat "${LOGFILE}"
  exit 1
fi
