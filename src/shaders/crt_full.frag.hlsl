// konCePCja — CRT Full fragment shader (HLSL 5.0 master source)
//
// Port of crt_full.frag.glsl for the D3D12 backend.  CRT Basic plus
// bloom + vignette + slot mask + curvature knobs.  Compiled via:
//   dxc -T ps_5_0 -E main crt_full.frag.hlsl -Fo crt_full.frag.dxbc
// or
//   fxc /T ps_5_0 /E main crt_full.frag.hlsl /Fo crt_full.frag.dxbc
//
// SDL_GPU DXBC bindings: t/s register space 2, b register space 3.

Texture2D    u_tex : register(t0, space2);
SamplerState u_smp : register(s0, space2);

cbuffer CrtFullUniforms : register(b0, space3) {
    float2 input_size;
    float2 output_size;
};

static const float kCurvature = 0.22;
static const float kScanline  = 0.7;
static const float kMask      = 0.7;
static const float kBloom     = 0.15;
static const float kVignette  = 0.4;

float2 barrel(float2 uv, float k) {
    float2 cc = uv - 0.5;
    float d = dot(cc, cc);
    return uv + cc * d * k;
}

float3 sampleBloom(float2 uv, float2 in_size) {
    float2 t = 1.0 / in_size;
    float3 s = float3(0.0, 0.0, 0.0);
    s += u_tex.Sample(u_smp, uv + float2(-t.x, 0.0)).rgb * 0.15;
    s += u_tex.Sample(u_smp, uv + float2( t.x, 0.0)).rgb * 0.15;
    s += u_tex.Sample(u_smp, uv + float2( 0.0,-t.y)).rgb * 0.15;
    s += u_tex.Sample(u_smp, uv + float2( 0.0, t.y)).rgb * 0.15;
    s += u_tex.Sample(u_smp, uv + float2(-t.x,-t.y)).rgb * 0.10;
    s += u_tex.Sample(u_smp, uv + float2( t.x,-t.y)).rgb * 0.10;
    s += u_tex.Sample(u_smp, uv + float2(-t.x, t.y)).rgb * 0.10;
    s += u_tex.Sample(u_smp, uv + float2( t.x, t.y)).rgb * 0.10;
    return s;
}

float4 main(float4 pos : SV_Position, float2 v_uv : TEXCOORD0) : SV_Target {
    float2 uv = barrel(v_uv, kCurvature);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    float3 col = u_tex.Sample(u_smp, uv).rgb;

    // Bloom
    col = lerp(col, col + sampleBloom(uv, input_size), kBloom);

    // Gaussian-weighted scanlines
    float scanY = uv.y * input_size.y;
    float sl = 0.5 + 0.5 * cos(scanY * 3.14159265 * 2.0);
    sl = sqrt(sl);
    col *= lerp(1.0, sl, kScanline * 0.5);

    // Slot mask (6-pixel pattern with vertical shift)
    float2 outPos = uv * output_size;
    float mx = fmod(outPos.x, 6.0);
    float my = fmod(outPos.y, 2.0);
    float3 mask;
    if (my < 1.0) {
        mask = float3(mx < 2.0 ? 1.0 : 0.7,
                      (mx >= 2.0 && mx < 4.0) ? 1.0 : 0.7,
                      mx >= 4.0 ? 1.0 : 0.7);
    } else {
        mask = float3((mx >= 3.0 && mx < 5.0) ? 0.7 : 1.0,
                      (mx < 1.0 || mx >= 5.0) ? 0.7 : 1.0,
                      (mx >= 1.0 && mx < 3.0) ? 0.7 : 1.0);
    }
    col *= lerp(float3(1.0, 1.0, 1.0), mask, kMask);

    // Vignette
    float2 vig = v_uv * (1.0 - v_uv);
    float vigF = pow(vig.x * vig.y * 15.0, 0.25);
    col *= lerp(1.0, vigF, kVignette);

    col *= 1.4;
    return float4(saturate(col), 1.0);
}
