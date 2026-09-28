#!/bin/sh
# Keep the K230 Wi-Fi link out of power-save.
#
# The RTL8189FS driver oopses in its LPS teardown (rtw_lps_state_chk ->
# LPS_Leave -> rtw_set_ps_mode) and the link never returns. rtw_power_mgnt=0
# only applies on module re-init, so for a live session the practical fix is
# to never let the link go idle.
HOST="${K230_HOST:-192.168.1.182}"
while ping -c 1 -W 2 "$HOST" >/dev/null 2>&1; do sleep 3; done
echo "[$(date +%H:%M:%S)] K230 became unreachable"
