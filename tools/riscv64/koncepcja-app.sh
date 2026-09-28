#!/bin/sh
# Run konCePCja as a handheld "app": take the display from the launcher, run,
# then hand it back — the way the vendor UI's own apps would if they were
# separate DRM clients.
#
# Why stopping the launcher is unavoidable: DRM allows exactly one master per
# device, and k230_phone_ui never calls drmDropMaster (verified: no such symbol
# in its binary). So drmSetMaster returns EBUSY and drmModeSetPlane returns
# EACCES while it runs — even as root, and even for the five idle overlay
# planes. Leasing would be the sanctioned alternative, but only the master can
# create a lease, so it would have to offer one.
#
# NOTE: /etc/init.d/S99zz_k230_phone_ui stop reports OK without actually
# killing the process, which silently leaves two DRM clients fighting over the
# panel. Kill it directly and verify.
set -u
APP=/root/koncepcja
LAUNCHER=/etc/init.d/S99zz_k230_phone_ui

stop_launcher() {
  "$LAUNCHER" stop >/dev/null 2>&1
  killall -9 k230_phone_ui k230_meshtastic_probe 2>/dev/null
  i=0
  while [ $i -lt 30 ]; do
    holders=$(for p in /proc/[0-9]*; do
                ls -l "$p/fd" 2>/dev/null | grep -q "dri/card0" && echo x
              done | wc -l)
    [ "$holders" -eq 0 ] && return 0
    i=$((i + 1)); sleep 1
  done
  echo "warning: something still holds /dev/dri/card0" >&2
}

start_launcher() { "$LAUNCHER" start >/dev/null 2>&1; }

trap 'start_launcher' EXIT INT TERM

stop_launcher
cd "$APP" || exit 1
KONCPC_DRM=1 \
KONCPC_DRM_BPP=16 \
KONCPC_WIDE=1 \
KONCPC_UI_SCALE=2 \
KONCPC_TOUCH=/dev/input/event1 \
SDL_VIDEODRIVER=offscreen \
SDL_AUDIODRIVER=dummy \
SDL_RENDER_DRIVER=software \
./koncepcja \
  -O video.scr_style=11 \
  -O video.scr_crt_aspect=0 \
  -O video.win_w=1232 \
  -O video.win_h=568 \
  -O rom.rom_path="$APP/rom" \
  "$@"
