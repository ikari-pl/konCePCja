#!/bin/bash

if [ "$SDL_VIDEODRIVER" = "dummy" ]; then
  echo "SKIP: scripted_operations_timing not supported with dummy video driver"
  exit 0
fi

case "$(uname -s)" in
  Darwin*|MINGW*|MSYS*|CYGWIN*)
    echo "SKIP: scripted_operations_timing screenshot reference is Linux-specific"
    exit 0
    ;;
esac

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
find . -iname "*~" -exec rm -fv {} \;

cd "$TSTDIR"


$KONCPCDIR/koncepcja -c koncepcja.cfg -a 'border 13:ink 0,13:ink 1,0:mode 1:for a=1 to 24:print"Hello World",a:next:call &bd19:call 0' -a 'KONCPC_WAITBREAKKONCPC_SCRNSHOT KONCPC_EXIT' >> "${LOGFILE}" 2>&1
# Intended test when ready (doesn't work for now because \n are added automatically at the end of -a):
# $KONCPCDIR/koncepcja -c koncepcja.cfg -a 'border 13:ink 0,13:ink 1,0:mode 1:for a=1 to 24:print"Hello World",a:next:call &bd19:call 0' -a KONCPC_WAITBREAK -a KONCPC_SCRNSHOT -a KONCPC_EXIT

# Screenshot file name is not predictible (a feature, not a bug)
mv -v ${OUTPUT_DIR}/screenshot_*png ${OUTPUT_DIR}/screenshot.png

if $DIFF -ur model ${OUTPUT_DIR} >> "${LOGFILE}"
then
  exit 0
else
  cat "${LOGFILE}"
  exit 1
fi
