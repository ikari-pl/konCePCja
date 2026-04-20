// konCePCja — CRT Lottes fragment shader (GLSL 450 master source)
//
// Port of kCrtLottesMSLSource from blit_shaders.h.  Timothy Lottes'
// public-domain CRT shader: Gaussian beam profile, sRGB-linear
// blending, curvature warp, slot mask.  Compiled via:
//   glslangValidator -V crt_lottes.frag.glsl -o crt_lottes.frag.spv
//
// SDL_GPU Vulkan descriptor conventions:
//   set 2 = sampled textures, set 3 = uniform buffers

#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_tex;

layout(set = 3, binding = 0) uniform CrtLottesUniforms {
    vec2 input_size;
    vec2 output_size;
} u;

const float kWarpX     = 0.031;
const float kWarpY     = 0.041;
const float kHardScan  = -8.0;
const float kHardPix   = -3.0;
const float kMaskDark  = 0.5;
const float kMaskLight = 1.5;

vec2 warp(vec2 pos) {
    pos = pos * 2.0 - 1.0;
    pos *= vec2(1.0 + pos.y * pos.y * kWarpX,
                1.0 + pos.x * pos.x * kWarpY);
    return pos * 0.5 + 0.5;
}

float toLinear1(float c) {
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
vec3 toLinear(vec3 c) {
    return vec3(toLinear1(c.r), toLinear1(c.g), toLinear1(c.b));
}
float toSrgb1(float c) {
    return (c < 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
vec3 toSrgb(vec3 c) {
    return vec3(toSrgb1(c.r), toSrgb1(c.g), toSrgb1(c.b));
}

vec3 fetch_pix(vec2 pos, vec2 off, vec2 input_size) {
    pos = (floor(pos * input_size + off) + 0.5) / input_size;
    if (pos.x < 0.0 || pos.x > 1.0 || pos.y < 0.0 || pos.y > 1.0)
        return vec3(0.0);
    return toLinear(texture(u_tex, pos).rgb);
}

vec2 pix_dist(vec2 pos, vec2 input_size) {
    pos = pos * input_size;
    return -(fract(pos) - 0.5);
}

float gaus(float pos, float scale) {
    return exp2(scale * pos * pos);
}

vec3 horz3(vec2 pos, float off, vec2 input_size) {
    vec3 b = fetch_pix(pos, vec2(-1.0, off), input_size);
    vec3 c = fetch_pix(pos, vec2( 0.0, off), input_size);
    vec3 d = fetch_pix(pos, vec2( 1.0, off), input_size);
    float dst = pix_dist(pos, input_size).x;
    float wb = gaus(dst - 1.0, kHardPix);
    float wc = gaus(dst + 0.0, kHardPix);
    float wd = gaus(dst + 1.0, kHardPix);
    return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

vec3 horz5(vec2 pos, float off, vec2 input_size) {
    vec3 a = fetch_pix(pos, vec2(-2.0, off), input_size);
    vec3 b = fetch_pix(pos, vec2(-1.0, off), input_size);
    vec3 c = fetch_pix(pos, vec2( 0.0, off), input_size);
    vec3 d = fetch_pix(pos, vec2( 1.0, off), input_size);
    vec3 e = fetch_pix(pos, vec2( 2.0, off), input_size);
    float dst = pix_dist(pos, input_size).x;
    float wa = gaus(dst - 2.0, kHardPix);
    float wb = gaus(dst - 1.0, kHardPix);
    float wc = gaus(dst + 0.0, kHardPix);
    float wd = gaus(dst + 1.0, kHardPix);
    float we = gaus(dst + 2.0, kHardPix);
    return (a*wa + b*wb + c*wc + d*wd + e*we) / (wa+wb+wc+wd+we);
}

float scan_weight(vec2 pos, float off, vec2 input_size) {
    return gaus(pix_dist(pos, input_size).y + off, kHardScan);
}

vec3 tri_sample(vec2 pos, vec2 input_size) {
    vec3 a = horz3(pos, -1.0, input_size);
    vec3 b = horz5(pos,  0.0, input_size);
    vec3 c = horz3(pos,  1.0, input_size);
    float wa = scan_weight(pos, -1.0, input_size);
    float wb = scan_weight(pos,  0.0, input_size);
    float wc = scan_weight(pos,  1.0, input_size);
    return a * wa + b * wb + c * wc;
}

vec3 shadowMask(vec2 pos) {
    pos.x += pos.y * 3.0;
    vec3 m = vec3(kMaskDark);
    pos.x = fract(pos.x / 6.0);
    if (pos.x < 0.333)      m.r = kMaskLight;
    else if (pos.x < 0.666) m.g = kMaskLight;
    else                    m.b = kMaskLight;
    return m;
}

void main() {
    vec2 pos = warp(v_uv);
    if (pos.x < 0.0 || pos.x > 1.0 || pos.y < 0.0 || pos.y > 1.0) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 col = tri_sample(pos, u.input_size);
    col *= shadowMask(gl_FragCoord.xy);
    out_color = vec4(toSrgb(clamp(col, 0.0, 1.0)), 1.0);
}
