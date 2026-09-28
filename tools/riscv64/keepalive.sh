#!/bin/sh
# Keep the K230 Wi-Fi link out of power-save, and log stalls.
#
# The RTL8189FS driver oopses in its LPS teardown (rtw_lps_state_chk ->
# LPS_Leave) and occasionally the link never returns; only a power-cycle
# recovers it. rtw_power_mgnt=0 applies only on module re-init, so for a live
# session the mitigation is to never let the link go idle.
#
# This only ever REPORTS. Two earlier versions exited on 1 and then 3
# consecutive ping failures and both cried wolf: with LPS active the link
# stalls for ~10s at a time (round-trips swing 7-210 ms) and recovers fine.
# A genuine death needs a power-cycle and is obvious because SSH stays dead
# for minutes -- so let the human/agent judge that, and just keep the link
# busy and note the stalls.
HOST="${K230_HOST:-192.168.1.182}"
run=0; worst=0; stalls=0
while :; do
  if ping -c 1 -W 3 "$HOST" >/dev/null 2>&1; then
    [ "$run" -gt 0 ] && echo "[$(date +%H:%M:%S)] recovered after ${run} failed ping(s)"
    run=0
  else
    run=$((run + 1)); stalls=$((stalls + 1))
    [ "$run" -gt "$worst" ] && worst=$run
    [ $((run % 5)) -eq 0 ] && echo "[$(date +%H:%M:%S)] still stalled (${run} consecutive, worst ${worst}, total ${stalls})"
  fi
  sleep 3
done
