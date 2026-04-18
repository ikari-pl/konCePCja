/* konCePCja — SDL3 GPU API scaffolding (Phase 2 of P1.2b)
 *
 * Owns the GPU device, swapchain claim, samplers, and CPC framebuffer
 * upload resources.  Coexists alongside the GL path until Phase 4 wires
 * plugins to the GPU render loop.
 */

#ifndef VIDEO_GPU_H
#define VIDEO_GPU_H

#include <SDL3/SDL_gpu.h>
#include <cstdint>

struct SDL_Window;

struct GpuState {
    SDL_GPUDevice*           device          = nullptr;
    SDL_Window*              window          = nullptr;   // claimed for GPU
    SDL_GPUTextureFormat     swapchain_fmt   = SDL_GPU_TEXTUREFORMAT_INVALID;

    // Samplers (created once at init, destroyed at shutdown)
    SDL_GPUSampler*          linear_sampler  = nullptr;   // 4:3 aspect stretch
    SDL_GPUSampler*          nearest_sampler = nullptr;   // square-pixel mode

    // CPC framebuffer upload path
    SDL_GPUTexture*          cpc_texture     = nullptr;   // RGBA8 render surface
    SDL_GPUTransferBuffer*   cpc_upload      = nullptr;   // staging buffer
    uint32_t                 cpc_tex_w       = 0;
    uint32_t                 cpc_tex_h       = 0;

    // Pipelines (nullptr until Phase 3 provides shader blobs)
    SDL_GPUGraphicsPipeline* blit_pipeline   = nullptr;

    // Per-frame command buffer (stashed by flip_a, submitted by flip_b).
    // Not used until Phase 4 wires plugins to the GPU path.
    SDL_GPUCommandBuffer*    pending_cmd     = nullptr;

    bool                     initialized     = false;
};

extern GpuState g_gpu;

// Initialize GPU device, claim window, create samplers + CPC texture/upload
// buffer.  Called from video_init() after the window exists.
// Returns true on success.  On failure (no GPU backend), returns false and
// the caller continues with the GL path.
bool video_gpu_init(SDL_Window* window, uint32_t fb_w, uint32_t fb_h);

// Destroy all GPU resources in reverse order.  Safe to call if not initialized.
void video_gpu_shutdown();

#endif // VIDEO_GPU_H
