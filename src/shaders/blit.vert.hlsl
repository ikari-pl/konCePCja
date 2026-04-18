// konCePCja — passthrough blit vertex shader (HLSL 5.0 master source)
//
// Compiled to DXBC for the D3D12 backend via:
//   dxc -T vs_5_0 -E main blit.vert.hlsl -Fo blit.vert.dxbc
// or
//   fxc /T vs_5_0 /E main blit.vert.hlsl /Fo blit.vert.dxbc
// See scripts/compile_shaders.sh.

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    // Fullscreen-triangle trick — see blit.vert.glsl for derivation
    float2 xy = float2(float((vid << 1) & 2), float(vid & 2));
    o.pos = float4(xy * 2.0 - 1.0, 0.0, 1.0);
    o.uv  = float2(xy.x, 1.0 - xy.y);  // flip Y: SDL_GPU top-left origin
    return o;
}
