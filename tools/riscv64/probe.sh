#!/bin/sh
set -e
echo "== arch: $(uname -m) =="
echo "== gcc =="; gcc --version 2>/dev/null | head -1 || echo "no gcc"
echo "== available dev packages =="
apt-get update -qq 2>/dev/null
for p in cmake g++ pkg-config libfreetype-dev zlib1g-dev libpng-dev libdrm-dev libgbm-dev libasound2-dev libudev-dev; do
  v=$(apt-cache policy "$p" 2>/dev/null | awk '/Candidate:/{print $2}')
  echo "$p -> ${v:-MISSING}"
done
