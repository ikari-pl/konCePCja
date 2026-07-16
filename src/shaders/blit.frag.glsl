// konCePCja — passthrough blit fragment shader (GLSL 450 master source)
//
// Compiled to SPIRV for the Vulkan backend via:
//   glslangValidator -V blit.frag.glsl -o blit.frag.spv
// See scripts/compile_shaders.sh.
//
// SDL_GPU binding convention for SPIRV fragment shaders:
//   set 2 = combined image samplers (texture + sampler pair)
// We use binding 0 in that set for our single CPC framebuffer texture.

#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_tex;

void main() {
    out_color = texture(u_tex, v_uv);
}
