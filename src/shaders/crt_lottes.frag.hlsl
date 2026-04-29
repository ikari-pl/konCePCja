// konCePCja — CRT Lottes fragment shader (HLSL 5.0 master source)
//
// Port of crt_lottes.frag.glsl for the D3D12 backend.  Timothy Lottes'
// public-domain CRT shader: Gaussian beam profile, sRGB-linear
// blending, curvature warp, slot mask.  Compiled via:
//   dxc -T ps_5_0 -E main crt_lottes.frag.hlsl -Fo crt_lottes.frag.dxbc
// or
//   fxc /T ps_5_0 /E main crt_lottes.frag.hlsl /Fo crt_lottes.frag.dxbc
//
// SDL_GPU DXBC bindings: t/s register space 2, b register space 3.

Texture2D    u_tex : register(t0, space2);
SamplerState u_smp : register(s0, space2);

cbuffer CrtLottesUniforms : register(b0, space3) {
    float2 input_size;
    float2 output_size;
};

static const float kWarpX     = 0.031;
static const float kWarpY     = 0.041;
static const float kHardScan  = -8.0;
static const float kHardPix   = -3.0;
static const float kMaskDark  = 0.5;
static const float kMaskLight = 1.5;

float2 warp(float2 p) {
    p = p * 2.0 - 1.0;
    p *= float2(1.0 + p.y * p.y * kWarpX,
                1.0 + p.x * p.x * kWarpY);
    return p * 0.5 + 0.5;
}

float toLinear1(float c) {
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 toLinear(float3 c) {
    return float3(toLinear1(c.r), toLinear1(c.g), toLinear1(c.b));
}
float toSrgb1(float c) {
    return (c < 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float3 toSrgb(float3 c) {
    return float3(toSrgb1(c.r), toSrgb1(c.g), toSrgb1(c.b));
}

float3 fetch_pix(float2 p, float2 off, float2 in_size) {
    p = (floor(p * in_size + off) + 0.5) / in_size;
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0)
        return float3(0.0, 0.0, 0.0);
    return toLinear(u_tex.Sample(u_smp, p).rgb);
}

float2 pix_dist(float2 p, float2 in_size) {
    p = p * in_size;
    return -(frac(p) - 0.5);
}

float gaus(float p, float scale) {
    return exp2(scale * p * p);
}

float3 horz3(float2 p, float off, float2 in_size) {
    float3 b = fetch_pix(p, float2(-1.0, off), in_size);
    float3 c = fetch_pix(p, float2( 0.0, off), in_size);
    float3 d = fetch_pix(p, float2( 1.0, off), in_size);
    float dst = pix_dist(p, in_size).x;
    float wb = gaus(dst - 1.0, kHardPix);
    float wc = gaus(dst + 0.0, kHardPix);
    float wd = gaus(dst + 1.0, kHardPix);
    return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

float3 horz5(float2 p, float off, float2 in_size) {
    float3 a = fetch_pix(p, float2(-2.0, off), in_size);
    float3 b = fetch_pix(p, float2(-1.0, off), in_size);
    float3 c = fetch_pix(p, float2( 0.0, off), in_size);
    float3 d = fetch_pix(p, float2( 1.0, off), in_size);
    float3 e = fetch_pix(p, float2( 2.0, off), in_size);
    float dst = pix_dist(p, in_size).x;
    float wa = gaus(dst - 2.0, kHardPix);
    float wb = gaus(dst - 1.0, kHardPix);
    float wc = gaus(dst + 0.0, kHardPix);
    float wd = gaus(dst + 1.0, kHardPix);
    float we = gaus(dst + 2.0, kHardPix);
    return (a*wa + b*wb + c*wc + d*wd + e*we) / (wa+wb+wc+wd+we);
}

float scan_weight(float2 p, float off, float2 in_size) {
    return gaus(pix_dist(p, in_size).y + off, kHardScan);
}

float3 tri_sample(float2 p, float2 in_size) {
    float3 a = horz3(p, -1.0, in_size);
    float3 b = horz5(p,  0.0, in_size);
    float3 c = horz3(p,  1.0, in_size);
    float wa = scan_weight(p, -1.0, in_size);
    float wb = scan_weight(p,  0.0, in_size);
    float wc = scan_weight(p,  1.0, in_size);
    return a * wa + b * wb + c * wc;
}

float3 shadowMask(float2 p) {
    p.x += p.y * 3.0;
    float3 m = float3(kMaskDark, kMaskDark, kMaskDark);
    p.x = frac(p.x / 6.0);
    if (p.x < 0.333)      m.r = kMaskLight;
    else if (p.x < 0.666) m.g = kMaskLight;
    else                  m.b = kMaskLight;
    return m;
}

float4 main(float4 pos : SV_Position, float2 v_uv : TEXCOORD0) : SV_Target {
    float2 p = warp(v_uv);
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    float3 col = tri_sample(p, input_size);
    col *= shadowMask(pos.xy);
    return float4(toSrgb(saturate(col)), 1.0);
}
