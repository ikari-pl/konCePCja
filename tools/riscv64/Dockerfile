# riscv64 build/test environment for konCePCja.
# Runs under QEMU binfmt on an aarch64/x86_64 host:
#   docker build --platform linux/riscv64 -t koncepcja-rv64 tools/riscv64
# Package versions verified available on Debian trixie riscv64, 2026-09-28.
FROM --platform=linux/riscv64 debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config git ca-certificates \
      libfreetype-dev zlib1g-dev libpng-dev \
      libdrm-dev libgbm-dev libasound2-dev libudev-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
