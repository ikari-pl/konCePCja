#!/bin/sh
# Run a command on the K230 board over SSH, or open a shell with no arguments.
#   tools/riscv64/k230.sh 'ls /dev/dri'
#   K230_HOST=192.168.1.182 tools/riscv64/k230.sh
# The stock LilyGO/Canaan image ships sshd with PermitRootLogin yes and
# PermitEmptyPasswords yes, and root has no password.
HOST="${K230_HOST:-192.168.1.182}"
KH="${TMPDIR:-/tmp}/k230_known_hosts"
exec ssh \
  -o StrictHostKeyChecking=accept-new \
  -o UserKnownHostsFile="$KH" \
  -o PubkeyAuthentication=no \
  -o PreferredAuthentications=password,keyboard-interactive \
  -o NumberOfPasswordPrompts=1 \
  -o ConnectTimeout=8 \
  "root@$HOST" "$@"
