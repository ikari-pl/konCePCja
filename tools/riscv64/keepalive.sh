#!/bin/sh
# Keep the K230 Wi-Fi link out of power-save, and report a genuine loss.
#
# The RTL8189FS driver oopses in its LPS teardown (rtw_lps_state_chk ->
# LPS_Leave) and the link never returns. rtw_power_mgnt=0 only applies on
# module re-init, so for a live session the fix is to never let it go idle.
#
# Tolerate transient loss: with LPS active, round-trip times swing from 7 ms to
# 200+ ms and single pings drop. Only three consecutive failures count as dead.
HOST="${K230_HOST:-192.168.1.182}"
fails=0
while :; do
  if ping -c 1 -W 3 "$HOST" >/dev/null 2>&1; then
    fails=0
  else
    fails=$((fails + 1))
    echo "[$(date +%H:%M:%S)] ping failed ($fails/3)"
    [ "$fails" -ge 3 ] && { echo "[$(date +%H:%M:%S)] K230 UNREACHABLE"; exit 1; }
  fi
  sleep 3
done
