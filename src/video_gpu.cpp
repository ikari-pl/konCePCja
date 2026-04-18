/* konCePCja — SDL3 GPU API scaffolding (Phase 2 of P1.2b)
 *
 * Creates and owns the GPU device, swapchain claim, samplers, and CPC
 * framebuffer texture + transfer buffer.  Coexists alongside the GL
 * rendering path until Phase 4 wires plugins to gpu_flip_a / gpu_flip_b.
 */

#include "video_gpu.h"
#include "log.h"

#include <SDL3/SDL.h>
#include <cstring>

GpuState g_gpu;

bool video_gpu_init(SDL_Window* window, uint32_t fb_w, uint32_t fb_h)
{
    if (g_gpu.initialized) return true;   // idempotent
    if (!window) return false;

    // ── 1. Create GPU device ──────────────────────────────────────────
    // Accept any backend (Metal on macOS, Vulkan on Linux, D3D12 on Windows).
    g_gpu.device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV
          | SDL_GPU_SHADERFORMAT_MSL
          | SDL_GPU_SHADERFORMAT_METALLIB
          | SDL_GPU_SHADERFORMAT_DXIL,
        /*debug_mode=*/false,
        /*name=*/nullptr);

    if (!g_gpu.device) {
        LOG_INFO("SDL_CreateGPUDevice failed: " << SDL_GetError());
        return false;
    }

    const char* driver = SDL_GetGPUDeviceDriver(g_gpu.device);
    LOG_INFO("GPU device created — driver: " << (driver ? driver : "(null)"));

    // ── 2. Claim window for GPU ───────────────────────────────────────
    // Must happen before any swapchain acquire (landmine #13).
    if (!SDL_ClaimWindowForGPUDevice(g_gpu.device, window)) {
        LOG_ERROR("SDL_ClaimWindowForGPUDevice failed: " << SDL_GetError());
        SDL_DestroyGPUDevice(g_gpu.device);
        g_gpu.device = nullptr;
        return false;
    }
    g_gpu.window = window;

    // ── 3. Query swapchain format ─────────────────────────────────────
    // Needed later for pipeline creation (landmine #5).
    g_gpu.swapchain_fmt = SDL_GetGPUSwapchainTextureFormat(g_gpu.device, window);
    LOG_INFO("GPU swapchain format: " << static_cast<int>(g_gpu.swapchain_fmt));

    // ── 4. Create samplers ────────────────────────────────────────────
    {
        SDL_GPUSamplerCreateInfo info{};
        info.min_filter       = SDL_GPU_FILTER_LINEAR;
        info.mag_filter       = SDL_GPU_FILTER_LINEAR;
        info.address_mode_u   = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_v   = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_w   = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        g_gpu.linear_sampler  = SDL_CreateGPUSampler(g_gpu.device, &info);
    }
    {
        SDL_GPUSamplerCreateInfo info{};
        info.min_filter       = SDL_GPU_FILTER_NEAREST;
        info.mag_filter       = SDL_GPU_FILTER_NEAREST;
        info.address_mode_u   = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_v   = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_w   = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        g_gpu.nearest_sampler = SDL_CreateGPUSampler(g_gpu.device, &info);
    }

    // ── 5. Create CPC framebuffer texture ─────────────────────────────
    // RGBA8, dimensions match the CPU-side render surface (768×540 at scale=2).
    // Usage: sampled in the blit pass, written by texture upload copy pass.
    // Note: SDL3 GPU has no COPY_DST flag — UploadToGPUTexture works on any
    // texture regardless of usage flags (see SDL_render_gpu.c for precedent).
    {
        SDL_GPUTextureCreateInfo info{};
        info.type                  = SDL_GPU_TEXTURETYPE_2D;
        info.format                = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width                 = fb_w;
        info.height                = fb_h;
        info.layer_count_or_depth  = 1;
        info.num_levels            = 1;
        info.usage                 = SDL_GPU_TEXTUREUSAGE_SAMPLER
                                   | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        g_gpu.cpc_texture = SDL_CreateGPUTexture(g_gpu.device, &info);
        g_gpu.cpc_tex_w = fb_w;
        g_gpu.cpc_tex_h = fb_h;
    }

    // ── 6. Create transfer buffer (staging for CPU→GPU upload) ────────
    {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size  = fb_w * fb_h * 4;   // RGBA8 = 4 bytes/pixel
        g_gpu.cpc_upload = SDL_CreateGPUTransferBuffer(g_gpu.device, &info);
    }

    // ── Verify all mandatory resources were created ───────────────────
    if (!g_gpu.linear_sampler || !g_gpu.nearest_sampler ||
        !g_gpu.cpc_texture   || !g_gpu.cpc_upload) {
        LOG_ERROR("GPU resource creation failed — rolling back");
        video_gpu_shutdown();
        return false;
    }

    g_gpu.initialized = true;
    LOG_INFO("GPU scaffolding initialized ("
             << fb_w << "×" << fb_h << ", "
             << (fb_w * fb_h * 4) << " bytes staging)");
    return true;
}

void video_gpu_shutdown()
{
    // No `initialized` guard — this function doubles as cleanup after a
    // partially-failed init.  All branches null-check before releasing.
    if (g_gpu.device) {
        if (g_gpu.cpc_upload) {
            SDL_ReleaseGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload);
        }
        if (g_gpu.cpc_texture) {
            SDL_ReleaseGPUTexture(g_gpu.device, g_gpu.cpc_texture);
        }
        if (g_gpu.blit_pipeline) {
            SDL_ReleaseGPUGraphicsPipeline(g_gpu.device, g_gpu.blit_pipeline);
        }
        if (g_gpu.nearest_sampler) {
            SDL_ReleaseGPUSampler(g_gpu.device, g_gpu.nearest_sampler);
        }
        if (g_gpu.linear_sampler) {
            SDL_ReleaseGPUSampler(g_gpu.device, g_gpu.linear_sampler);
        }

        // Wait for all in-flight GPU work before destroying the device
        // (landmine #20).
        SDL_WaitForGPUIdle(g_gpu.device);

        if (g_gpu.window) {
            SDL_ReleaseWindowFromGPUDevice(g_gpu.device, g_gpu.window);
        }

        SDL_DestroyGPUDevice(g_gpu.device);
    }

    g_gpu = GpuState{};   // zero all fields, initialized = false
    LOG_INFO("GPU scaffolding shut down");
}
