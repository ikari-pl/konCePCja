#!/usr/bin/env bash
# konCePCja — shader compilation helper (developer tool, not run by CI).
#
# Produces backend-specific shader blobs from the sources in src/shaders/
# and writes them into src/shaders/blit_shaders.h as C++ byte arrays.
#
# Required tools (install only the backends you need):
#   glslangValidator — SPIRV (Vulkan).  `brew install glslang` on macOS.
#   dxc or fxc       — DXBC (D3D12 on Windows).
#   xcrun metal      — optional, only if you want to precompile metallib
#                      instead of shipping MSL source.  `xcodebuild
#                      -downloadComponent MetalToolchain` on macOS.
#
# Metal-only workflow: NOT needed.  We ship MSL source directly in the
# header and Metal compiles it on the device.

set -euo pipefail
cd "$(dirname "$0")/.."

SHADERS=src/shaders
OUT=$SHADERS/blobs
mkdir -p "$OUT"

echo "==> SPIRV (Vulkan)"
if command -v glslangValidator >/dev/null; then
    glslangValidator -V "$SHADERS/blit.vert.glsl"        -o "$OUT/blit.vert.spv"
    glslangValidator -V "$SHADERS/blit.frag.glsl"        -o "$OUT/blit.frag.spv"
    glslangValidator -V "$SHADERS/crt_basic.frag.glsl"   -o "$OUT/crt_basic.frag.spv"
    glslangValidator -V "$SHADERS/crt_full.frag.glsl"    -o "$OUT/crt_full.frag.spv"
    glslangValidator -V "$SHADERS/crt_lottes.frag.glsl"  -o "$OUT/crt_lottes.frag.spv"
    echo "    ok: blit.{vert,frag}.spv, crt_{basic,full,lottes}.frag.spv"
else
    echo "    skip: glslangValidator not installed"
fi

echo "==> DXBC (D3D12)"
if command -v dxc >/dev/null; then
    dxc -T vs_5_0 -E main "$SHADERS/blit.vert.hlsl" -Fo "$OUT/blit.vert.dxbc"
    dxc -T ps_5_0 -E main "$SHADERS/blit.frag.hlsl" -Fo "$OUT/blit.frag.dxbc"
    echo "    ok: $OUT/blit.vert.dxbc, $OUT/blit.frag.dxbc"
elif command -v fxc >/dev/null; then
    fxc /T vs_5_0 /E main "$SHADERS/blit.vert.hlsl" /Fo "$OUT/blit.vert.dxbc"
    fxc /T ps_5_0 /E main "$SHADERS/blit.frag.hlsl" /Fo "$OUT/blit.frag.dxbc"
    echo "    ok: $OUT/blit.vert.dxbc, $OUT/blit.frag.dxbc"
else
    echo "    skip: neither dxc nor fxc installed"
fi

# Converting the blob files into the C array literals in blit_shaders.h
# is a manual step — run xxd or a Python one-liner, then paste into the
# header:
#   xxd -i < blit.vert.spv  # produces `0xHH, 0xHH, ...` output
echo
echo "==> Next: update src/shaders/blit_shaders.h with the new blob bytes."
echo "    Suggested: xxd -i < $OUT/blit.vert.spv"
