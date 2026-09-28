#!/bin/bash
#
# Reproduce the compile table in PR1-display-handover.md.
#
# Expects a workspace holding the K230 SDK builder image and the Xuantie
# toolchain, with the LVGL package tree unpacked at <workspace>/pr1:
#
#   pr1/lvgl/                 LVGL 59dc7e43, with 0001-0004 applied
#   pr1/0005-*.patch          the patch under test
#
#   docker run --rm --platform linux/amd64 \
#     -v "$WS:/work" -v "$WS/toolchain:/opt/toolchain" -w /work \
#     k230-sdk-build /work/pr1/compile-check.sh with-0005
#
# Run it once on the base tree and once with 0005 applied; the symbol counts
# are what distinguish a real build from an empty one.
# Compile-check the LVGL DRM driver TU on its own, for host and for riscv64,
# and prove the object really holds the driver: the whole file sits inside
# #if LV_USE_LINUX_DRM, so a misconfigured lv_conf.h compiles to nothing.
set -u
L=/work/pr1/lvgl
LABEL=${1:-unnamed}
CONF=/work/pr1/lv_conf.h
TC=/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/bin/riscv64-unknown-linux-gnu-gcc

sed -e '0,/^#if 0/s/^#if 0.*$/#if 1/' \
    -e 's/^ *#define LV_USE_LINUX_DRM .*/#define LV_USE_LINUX_DRM 1/' \
    "$L/lv_conf_template.h" > "$CONF"
grep -qE '^#if 1' "$CONF" || { echo "lv_conf.h not enabled!"; exit 2; }
grep -qE '^#define LV_USE_LINUX_DRM 1' "$CONF" || { echo "DRM not enabled!"; exit 2; }

# libdrm's own headers, isolated: passing -I/usr/include to the cross compiler
# would shadow its sysroot's glibc headers. These headers are architecture
# independent, so they are a valid check of the driver for riscv64 even though
# the target sysroot has no libdrm of its own.
DRMINC=/tmp/drminc
mkdir -p "$DRMINC"
cp /usr/include/xf86drm.h /usr/include/xf86drmMode.h "$DRMINC/" 2>/dev/null
cp /usr/include/libdrm/*.h "$DRMINC/" 2>/dev/null

fail=0
for cc in "gcc" "$TC -march=rv64gc"; do
  name=$(echo "$cc" | grep -q riscv && echo riscv64 || echo host)
  out=/tmp/drm-$name.o
  rm -f "$out"
  $cc -c -std=gnu11 -Wall -Wextra \
      -DLV_CONF_PATH="\"$CONF\"" \
      -I"$L" -I"$L/src" -I"$DRMINC" \
      -o "$out" "$L/src/drivers/display/drm/lv_linux_drm.c" 2>&1 \
    | grep -vE '#warning|pragma message|^ *[0-9]+ \||^ *\||^In file included|^ *from '
  rc=${PIPESTATUS[0]}
  sz=$(stat -c%s "$out" 2>/dev/null || echo 0)
  syms=$(nm -g --defined-only "$out" 2>/dev/null | grep -cE 'lv_linux_drm_(release|acquire)')
  echo "[$LABEL/$name] exit=$rc size=${sz}B release+acquire_symbols=$syms"
  [ "$rc" -eq 0 ] && [ "$sz" -gt 1000 ] || fail=1
done
exit $fail
