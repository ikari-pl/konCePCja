#!/bin/bash
# Smoke test for a Linux release package on a clean container (run as root).
#
#   linux_package_smoke.sh <koncepcja_*.deb | konCePCja-*.AppImage> <version>
#
# A .deb is installed with apt (so a missing dependency fails here, not on a
# user's machine); an AppImage runs as it is, unpacked by its own runtime
# (APPIMAGE_EXTRACT_AND_RUN: containers have no FUSE). Then, from an empty
# directory so no checkout config can be picked up:
#   1. --version must print <version>;
#   2. the CPC must boot from the packaged ROMs and run BASIC: it prints a
#      line to the printer port, captured in a file, and quits. Once in the
#      GUI build (dummy video and audio driver), once in the -H loop.
# Every check asserts the evidence it expects; a run that printed nothing
# fails. The package contents are asserted too (check_contents below).
set -euo pipefail

pkg=$(readlink -f "$1")
version=$2
expected_cfg=""

case "$pkg" in
  *.deb)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends "$pkg"
    exe=(koncepcja)
    expected_cfg=/etc/koncepcja.cfg
    ;;
  *.AppImage)
    [ -x "$pkg" ] || chmod +x "$pkg"
    export APPIMAGE_EXTRACT_AND_RUN=1
    exe=("$pkg")
    ;;
  *)
    echo "linux_package_smoke.sh: not a .deb or an AppImage: $pkg" >&2
    exit 2
    ;;
esac

# check_contents: the package must carry the libraries it needs. The .deb
# ships the vendored SDL3 privately and must not depend on a libjpeg package
# (the soname differs between Debian and Ubuntu; the binary links libjpeg
# statically). The AppImage bundles SDL3 and libcom_err (needed on Debian 13).
check_contents() {
  case "$pkg" in
    *.deb)
      # Capture first: with pipefail, "dpkg-deb -c | grep -q" would fail on
      # the SIGPIPE grep -q gives the tar subprocess.
      local listing
      listing=$(dpkg-deb -c "$pkg")
      grep -q 'usr/lib/koncepcja/libSDL3\.so' <<<"$listing" ||
        fail "the .deb does not ship the private SDL3 under /usr/lib/koncepcja"
      if grep -q libjpeg <<<"$(dpkg-deb -f "$pkg" Depends)"; then
        fail "the .deb depends on a libjpeg package (soname differs between Debian and Ubuntu)"
      fi
      ;;
    *.AppImage)
      local d
      d=$(mktemp -d)
      (cd "$d" && env -u APPIMAGE_EXTRACT_AND_RUN "$pkg" --appimage-extract >/dev/null) ||
        fail "could not extract the AppImage"
      test -f "$d/squashfs-root/usr/lib/libSDL3.so.0" ||
        fail "the AppImage does not bundle libSDL3"
      test -f "$d/squashfs-root/usr/lib/libcom_err.so.2" ||
        fail "the AppImage does not bundle libcom_err.so.2 (needed on Debian 13)"
      rm -rf "$d"
      ;;
  esac
}

work=$(mktemp -d)
cd "$work"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy KONCPC_NO_DIALOGS=1

fail() {
  echo "!! $*" >&2
  [ -f run.log ] && cat run.log >&2
  exit 1
}

check_contents

out=$("${exe[@]}" --version 2>&1) || fail "--version exited non-zero: $out"
echo "--version: $out"
case "$out" in
  *"konCePCja v$version"*) ;;
  *) fail "--version does not print 'konCePCja v$version': $out" ;;
esac

# run_basic <label> [extra koncepcja options...]
run_basic() {
  local label=$1
  shift
  rm -f printer.dat run.log
  printf 'Hello, world!\r\n' > expected.dat
  if ! timeout 120 "${exe[@]}" "$@" \
      -O system.printer=1 -O "file.printer_file=$work/printer.dat" \
      -a 'print #8,"Hello, world!"' -a KONCPC_EXIT > run.log 2>&1; then
    fail "$label: koncepcja exited non-zero"
  fi
  [ -f printer.dat ] || fail "$label: the CPC printed nothing (no printer.dat)"
  cmp -s expected.dat printer.dat ||
    fail "$label: printer output differs: $(od -c printer.dat | head -3)"
  if [ -n "$expected_cfg" ]; then
    grep -q "Using configuration file: $expected_cfg" run.log ||
      fail "$label: did not load $expected_cfg"
  fi
  echo "$label: the CPC booted and printed 'Hello, world!'"
}

run_basic "GUI build, dummy drivers"
run_basic "headless (-H)" -H
echo "PASS: $(basename "$pkg") on $(. /etc/os-release && echo "$PRETTY_NAME") ($(uname -m))"
