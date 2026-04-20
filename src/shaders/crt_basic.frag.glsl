// konCePCja — CRT Basic fragment shader (GLSL 450 master source)
//
// Port of kCrtBasicMSLSource from blit_shaders.h.  Applies barrel
// distortion + per-scanline modulation + RGB phosphor mask.  Compiled
// to SPIRV for the Vulkan backend via:
//   glslangValidator -V crt_basic.frag.glsl -o crt_basic.frag.spv
// See scripts/compile_shaders.sh.
//
// SDL_GPU Vulkan descriptor conventions for fragment shaders:
//   set 2 = sampled textures / combined image samplers
//   set 3 = uniform buffers

#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_tex;

layout(set = 3, binding = 0) uniform CrtBasicUniforms {
    vec2 input_size;   // CPC framebuffer resolution (width, height)
    vec2 output_size;  // swapchain resolution (affects RGB mask cell size)
} u;

vec2 barrel(vec2 uv, float k) {
    vec2 cc = uv - 0.5;
    float d = dot(cc, cc);
    return uv + cc * d * k;
}

void main() {
    vec2 uv = barrel(v_uv, 0.22);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 col = texture(u_tex, uv).rgb;

    // Scanlines tied to source resolution
    float scanline = sin(uv.y * u.input_size.y * 3.14159265) * 0.5 + 0.5;
    col *= mix(1.0, scanline, 0.35);

    // RGB phosphor mask at output pixel density
    vec2 outPos = uv * u.output_size;
    float m = mod(outPos.x, 3.0);
    vec3 mask = vec3(
        m < 1.0 ? 1.0 : 0.75,
        (m >= 1.0 && m < 2.0) ? 1.0 : 0.75,
        m >= 2.0 ? 1.0 : 0.75);
    col *= mask;
    col *= 1.3;
    out_color = vec4(clamp(col, 0.0, 1.0), 1.0);
}
