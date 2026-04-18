// konCePCja — embedded shader blobs for the passthrough blit pipeline.
//
// Generated (MSL) by hand from src/shaders/blit.msl and the GLSL/HLSL
// files checked in alongside.  See scripts/compile_shaders.sh for the
// SPIRV and DXBC compilation steps — those blobs are currently empty
// placeholders and will be populated in a follow-up PR.

#pragma once

#include <cstddef>
#include <cstdint>

// ── Metal (macOS): raw MSL source ─────────────────────────────────────
// SDL_GPU accepts MSL source text directly (SDL_GPU_SHADERFORMAT_MSL).
// Metal compiles on-device at SDL_CreateGPUShader() time.
// Both vert_main and frag_main live in this single source string.
inline constexpr const char* kBlitMSLSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float2 uv;
};

vertex VSOut vert_main(uint vid [[vertex_id]]) {
    VSOut o;
    float2 xy = float2(float((vid << 1) & 2), float(vid & 2));
    o.position = float4(xy * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(xy.x, 1.0 - xy.y);
    return o;
}

fragment float4 frag_main(VSOut in [[stage_in]],
                          texture2d<float> tex [[texture(0)]],
                          sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv);
}
)MSL";

// ── Vulkan: SPIRV bytecode ────────────────────────────────────────────
// Populated by a follow-up PR after running scripts/compile_shaders.sh
// on a machine with glslangValidator (or via a CI job).  Empty for now.
inline constexpr std::uint8_t kBlitVertexSPIRV[] = {0};
inline constexpr std::uint8_t kBlitFragmentSPIRV[] = {0};
inline constexpr std::size_t kBlitVertexSPIRVSize = 0;
inline constexpr std::size_t kBlitFragmentSPIRVSize = 0;

// ── D3D12: DXBC bytecode ──────────────────────────────────────────────
// Populated by a follow-up PR after running scripts/compile_shaders.sh
// on a Windows runner with fxc or dxc.  Empty for now.
inline constexpr std::uint8_t kBlitVertexDXBC[] = {0};
inline constexpr std::uint8_t kBlitFragmentDXBC[] = {0};
inline constexpr std::size_t kBlitVertexDXBCSize = 0;
inline constexpr std::size_t kBlitFragmentDXBCSize = 0;
