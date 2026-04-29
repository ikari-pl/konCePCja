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
# SM 5_1: fxc/dxc profile that supports register-space syntax.  Plain
# 5_0 errors with X3721 on `register(..., space2)`.
if command -v dxc >/dev/null; then
    dxc -T vs_5_1 -E main "$SHADERS/blit.vert.hlsl"        -Fo "$OUT/blit.vert.dxbc"
    dxc -T ps_5_1 -E main "$SHADERS/blit.frag.hlsl"        -Fo "$OUT/blit.frag.dxbc"
    dxc -T ps_5_1 -E main "$SHADERS/crt_basic.frag.hlsl"   -Fo "$OUT/crt_basic.frag.dxbc"
    dxc -T ps_5_1 -E main "$SHADERS/crt_full.frag.hlsl"    -Fo "$OUT/crt_full.frag.dxbc"
    dxc -T ps_5_1 -E main "$SHADERS/crt_lottes.frag.hlsl"  -Fo "$OUT/crt_lottes.frag.dxbc"
    echo "    ok: blit.{vert,frag}.dxbc, crt_{basic,full,lottes}.frag.dxbc"
elif command -v fxc >/dev/null; then
    fxc //T vs_5_1 //E main "$SHADERS/blit.vert.hlsl"        //Fo "$OUT/blit.vert.dxbc"
    fxc //T ps_5_1 //E main "$SHADERS/blit.frag.hlsl"        //Fo "$OUT/blit.frag.dxbc"
    fxc //T ps_5_1 //E main "$SHADERS/crt_basic.frag.hlsl"   //Fo "$OUT/crt_basic.frag.dxbc"
    fxc //T ps_5_1 //E main "$SHADERS/crt_full.frag.hlsl"    //Fo "$OUT/crt_full.frag.dxbc"
    fxc //T ps_5_1 //E main "$SHADERS/crt_lottes.frag.hlsl"  //Fo "$OUT/crt_lottes.frag.dxbc"
    echo "    ok: blit.{vert,frag}.dxbc, crt_{basic,full,lottes}.frag.dxbc"
else
    echo "    skip: neither dxc nor fxc installed"
fi

# Regenerate the embedded blob C++ headers from whatever blobs we have.
# Missing blobs become empty placeholders (the runtime treats them as
# "DXBC not available" and falls back).
echo
echo "==> Regenerating C++ headers"
if command -v python3 >/dev/null; then
    python3 "$(dirname "$0")/blob_to_header.py"
else
    echo "    skip: python3 not installed — re-run blob_to_header.py manually"
fi
