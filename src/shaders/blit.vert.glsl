// konCePCja — passthrough blit vertex shader (GLSL 450 master source)
//
// Compiled to SPIRV for the Vulkan backend via:
//   glslangValidator -V blit.vert.glsl -o blit.vert.spv
// See scripts/compile_shaders.sh.

#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    // Fullscreen-triangle trick: generate position from gl_VertexIndex.
    // vid=0 → xy=(0,0); vid=1 → xy=(2,0); vid=2 → xy=(0,2)
    vec2 xy = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(xy * 2.0 - 1.0, 0.0, 1.0);
    v_uv = vec2(xy.x, 1.0 - xy.y);  // flip Y: SDL_GPU uses top-left origin
}
