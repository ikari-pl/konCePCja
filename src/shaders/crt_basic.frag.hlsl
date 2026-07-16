// konCePCja — CRT Basic fragment shader (HLSL 5.0 master source)
//
// Port of crt_basic.frag.glsl for the D3D12 backend.  Compiled to DXBC
// via:
//   dxc -T ps_5_0 -E main crt_basic.frag.hlsl -Fo crt_basic.frag.dxbc
// or
//   fxc /T ps_5_0 /E main crt_basic.frag.hlsl /Fo crt_basic.frag.dxbc
// See scripts/compile_shaders.sh.
//
// SDL_GPU binding convention for DXBC fragment shaders:
//   t-register space 2 = textures, s-register space 2 = samplers,
//   b-register space 3 = uniform buffers.

Texture2D    u_tex : register(t0, space2);
SamplerState u_smp : register(s0, space2);

cbuffer CrtBasicUniforms : register(b0, space3) {
    float2 input_size;   // CPC framebuffer resolution (width, height)
    float2 output_size;  // swapchain resolution (affects RGB mask cell size)
};

float2 barrel(float2 uv, float k) {
    float2 cc = uv - 0.5;
    float d = dot(cc, cc);
    return uv + cc * d * k;
}

float4 main(float4 pos : SV_Position, float2 v_uv : TEXCOORD0) : SV_Target {
    float2 uv = barrel(v_uv, 0.22);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    float3 col = u_tex.Sample(u_smp, uv).rgb;

    // Scanlines tied to source resolution
    float scanline = sin(uv.y * input_size.y * 3.14159265) * 0.5 + 0.5;
    col *= lerp(1.0, scanline, 0.35);

    // RGB phosphor mask at output pixel density
    float2 outPos = uv * output_size;
    float m = fmod(outPos.x, 3.0);
    float3 mask = float3(
        m < 1.0 ? 1.0 : 0.75,
        (m >= 1.0 && m < 2.0) ? 1.0 : 0.75,
        m >= 2.0 ? 1.0 : 0.75);
    col *= mask;
    col *= 1.3;
    return float4(saturate(col), 1.0);
}
