// konCePCja — passthrough blit fragment shader (HLSL 5.0 master source)
//
// Compiled to DXBC for the D3D12 backend via:
//   dxc -T ps_5_0 -E main blit.frag.hlsl -Fo blit.frag.dxbc
// or
//   fxc /T ps_5_0 /E main blit.frag.hlsl /Fo blit.frag.dxbc
// See scripts/compile_shaders.sh.
//
// SDL_GPU binding convention for DXBC fragment shaders:
//   t-register space 2 = textures, s-register space 2 = samplers
//   (vertex shader would use space 1)

Texture2D    u_tex : register(t0, space2);
SamplerState u_smp : register(s0, space2);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return u_tex.Sample(u_smp, uv);
}
