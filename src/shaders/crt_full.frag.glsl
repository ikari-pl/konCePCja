// konCePCja — CRT Full fragment shader (GLSL 450 master source)
//
// Port of kCrtFullMSLSource from blit_shaders.h.  CRT Basic plus
// bloom + vignette + slot mask + curvature knobs.  Compiled via:
//   glslangValidator -V crt_full.frag.glsl -o crt_full.frag.spv
//
// SDL_GPU Vulkan descriptor conventions:
//   set 2 = sampled textures, set 3 = uniform buffers

#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_tex;

layout(set = 3, binding = 0) uniform CrtFullUniforms {
    vec2 input_size;
    vec2 output_size;
} u;

const float kCurvature = 0.22;
const float kScanline  = 0.7;
const float kMask      = 0.7;
const float kBloom     = 0.15;
const float kVignette  = 0.4;

vec2 barrel(vec2 uv, float k) {
    vec2 cc = uv - 0.5;
    float d = dot(cc, cc);
    return uv + cc * d * k;
}

vec3 sampleBloom(vec2 uv, vec2 input_size) {
    vec2 t = 1.0 / input_size;
    vec3 s = vec3(0.0);
    s += texture(u_tex, uv + vec2(-t.x, 0.0)).rgb * 0.15;
    s += texture(u_tex, uv + vec2( t.x, 0.0)).rgb * 0.15;
    s += texture(u_tex, uv + vec2( 0.0,-t.y)).rgb * 0.15;
    s += texture(u_tex, uv + vec2( 0.0, t.y)).rgb * 0.15;
    s += texture(u_tex, uv + vec2(-t.x,-t.y)).rgb * 0.10;
    s += texture(u_tex, uv + vec2( t.x,-t.y)).rgb * 0.10;
    s += texture(u_tex, uv + vec2(-t.x, t.y)).rgb * 0.10;
    s += texture(u_tex, uv + vec2( t.x, t.y)).rgb * 0.10;
    return s;
}

void main() {
    vec2 uv = barrel(v_uv, kCurvature);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 col = texture(u_tex, uv).rgb;

    // Bloom
    col = mix(col, col + sampleBloom(uv, u.input_size), kBloom);

    // Gaussian-weighted scanlines
    float scanY = uv.y * u.input_size.y;
    float sl = 0.5 + 0.5 * cos(scanY * 3.14159265 * 2.0);
    sl = sqrt(sl);
    col *= mix(1.0, sl, kScanline * 0.5);

    // Slot mask (6-pixel pattern with vertical shift)
    vec2 outPos = uv * u.output_size;
    float mx = mod(outPos.x, 6.0);
    float my = mod(outPos.y, 2.0);
    vec3 mask;
    if (my < 1.0) {
        mask = vec3(mx < 2.0 ? 1.0 : 0.7,
                    (mx >= 2.0 && mx < 4.0) ? 1.0 : 0.7,
                    mx >= 4.0 ? 1.0 : 0.7);
    } else {
        mask = vec3((mx >= 3.0 && mx < 5.0) ? 0.7 : 1.0,
                    (mx < 1.0 || mx >= 5.0) ? 0.7 : 1.0,
                    (mx >= 1.0 && mx < 3.0) ? 0.7 : 1.0);
    }
    col *= mix(vec3(1.0), mask, kMask);

    // Vignette
    vec2 vig = v_uv * (1.0 - v_uv);
    float vigF = pow(vig.x * vig.y * 15.0, 0.25);
    col *= mix(1.0, vigF, kVignette);

    col *= 1.4;
    out_color = vec4(clamp(col, 0.0, 1.0), 1.0);
}
