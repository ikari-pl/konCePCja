/* konCePCja — SDL3 GPU API scaffolding (Phase 2 of P1.2b)
 *
 * Creates and owns the GPU device, swapchain claim, samplers, and CPC
 * framebuffer texture + transfer buffer.  Coexists alongside the GL
 * rendering path until Phase 4 wires plugins to gpu_flip_a / gpu_flip_b.
 */

#include "video_gpu.h"

#include <SDL3/SDL.h>

#include <cstring>

#include "log.h"
#include "shaders/blit_shaders.h"

GpuState g_gpu;

namespace {

// Create the blit vertex + fragment shaders for the active backend.
// On Metal: pass MSL source directly (SDL compiles on device).
// On Vulkan/D3D12: pass pre-compiled SPIRV/DXBC blobs — but these are
// currently empty placeholders, so shader creation is skipped and the
// blit pipeline stays nullptr on those backends.
// Returns true if both shaders were created; false if the backend has
// no blob yet (non-fatal — plugins will fall back to the GL path until
// blobs are populated in a follow-up).
bool create_blit_shaders(const char* driver) {
  SDL_GPUShaderCreateInfo vsi{};
  vsi.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vsi.num_samplers = 0;
  vsi.num_uniform_buffers = 0;
  vsi.num_storage_buffers = 0;
  vsi.num_storage_textures = 0;

  SDL_GPUShaderCreateInfo fsi{};
  fsi.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fsi.num_samplers = 1;  // single CPC framebuffer texture
  fsi.num_uniform_buffers = 0;
  fsi.num_storage_buffers = 0;
  fsi.num_storage_textures = 0;

  if (driver && std::strcmp(driver, "metal") == 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_MSL;
    vsi.code = reinterpret_cast<const Uint8*>(kBlitMSLSource);
    vsi.code_size = std::strlen(kBlitMSLSource);
    vsi.entrypoint = "vert_main";

    fsi.format = SDL_GPU_SHADERFORMAT_MSL;
    fsi.code = reinterpret_cast<const Uint8*>(kBlitMSLSource);
    fsi.code_size = std::strlen(kBlitMSLSource);
    fsi.entrypoint = "frag_main";
  } else if (driver && std::strcmp(driver, "vulkan") == 0 &&
             kBlitVertexSPIRVSize > 0 && kBlitFragmentSPIRVSize > 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vsi.code = kBlitVertexSPIRV;
    vsi.code_size = kBlitVertexSPIRVSize;
    vsi.entrypoint = "main";

    fsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fsi.code = kBlitFragmentSPIRV;
    fsi.code_size = kBlitFragmentSPIRVSize;
    fsi.entrypoint = "main";
  } else if (driver && std::strcmp(driver, "direct3d12") == 0 &&
             kBlitVertexDXBCSize > 0 && kBlitFragmentDXBCSize > 0) {
    vsi.format = SDL_GPU_SHADERFORMAT_DXBC;
    vsi.code = kBlitVertexDXBC;
    vsi.code_size = kBlitVertexDXBCSize;
    vsi.entrypoint = "main";

    fsi.format = SDL_GPU_SHADERFORMAT_DXBC;
    fsi.code = kBlitFragmentDXBC;
    fsi.code_size = kBlitFragmentDXBCSize;
    fsi.entrypoint = "main";
  } else {
    LOG_INFO("No blit shader blob available for driver '"
             << (driver ? driver : "(null)")
             << "' — blit pipeline will be skipped");
    return false;
  }

  g_gpu.blit_vertex_shader = SDL_CreateGPUShader(g_gpu.device, &vsi);
  g_gpu.blit_fragment_shader = SDL_CreateGPUShader(g_gpu.device, &fsi);
  if (!g_gpu.blit_vertex_shader || !g_gpu.blit_fragment_shader) {
    LOG_ERROR("SDL_CreateGPUShader failed: " << SDL_GetError());
    // Release whichever shader did succeed so g_gpu stays in a
    // consistent "no blit shaders" state (both null).
    if (g_gpu.blit_vertex_shader) {
      SDL_ReleaseGPUShader(g_gpu.device, g_gpu.blit_vertex_shader);
      g_gpu.blit_vertex_shader = nullptr;
    }
    if (g_gpu.blit_fragment_shader) {
      SDL_ReleaseGPUShader(g_gpu.device, g_gpu.blit_fragment_shader);
      g_gpu.blit_fragment_shader = nullptr;
    }
    return false;
  }
  return true;
}

// Create the passthrough blit graphics pipeline.  Requires shaders to
// already exist in g_gpu.  Renders a fullscreen triangle with no vertex
// buffer; one color target matching the swapchain format.
bool create_blit_pipeline() {
  if (!g_gpu.blit_vertex_shader || !g_gpu.blit_fragment_shader) return false;

  SDL_GPUColorTargetDescription color_target{};
  color_target.format = g_gpu.swapchain_fmt;
  // No blending — passthrough writes the CPC framebuffer's pixels directly.

  SDL_GPUGraphicsPipelineTargetInfo target_info{};
  target_info.num_color_targets = 1;
  target_info.color_target_descriptions = &color_target;
  target_info.has_depth_stencil_target = false;

  SDL_GPUGraphicsPipelineCreateInfo info{};
  info.vertex_shader = g_gpu.blit_vertex_shader;
  info.fragment_shader = g_gpu.blit_fragment_shader;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.target_info = target_info;
  // vertex_input_state left zero — no VB, fullscreen triangle from vertex ID

  g_gpu.blit_pipeline = SDL_CreateGPUGraphicsPipeline(g_gpu.device, &info);
  if (!g_gpu.blit_pipeline) {
    LOG_ERROR("SDL_CreateGPUGraphicsPipeline failed: " << SDL_GetError());
    return false;
  }
  return true;
}

}  // namespace

bool video_gpu_init(SDL_Window* window, uint32_t fb_w, uint32_t fb_h) {
  if (g_gpu.initialized) return true;  // idempotent
  if (!window) return false;

  // ── 1. Create GPU device ──────────────────────────────────────────
  // Accept any backend (Metal on macOS, Vulkan on Linux, D3D12 on Windows).
  g_gpu.device = SDL_CreateGPUDevice(
      SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL |
          SDL_GPU_SHADERFORMAT_METALLIB | SDL_GPU_SHADERFORMAT_DXIL,
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
    info.min_filter = SDL_GPU_FILTER_LINEAR;
    info.mag_filter = SDL_GPU_FILTER_LINEAR;
    info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    g_gpu.linear_sampler = SDL_CreateGPUSampler(g_gpu.device, &info);
  }
  {
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = SDL_GPU_FILTER_NEAREST;
    info.mag_filter = SDL_GPU_FILTER_NEAREST;
    info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    g_gpu.nearest_sampler = SDL_CreateGPUSampler(g_gpu.device, &info);
  }

  // ── 5. Create CPC framebuffer texture ─────────────────────────────
  // RGBA8, dimensions match the CPU-side render surface (768×540 at scale=2).
  // Usage: sampled in the blit pass, written by texture upload copy pass.
  // Note: SDL3 GPU has no COPY_DST flag — UploadToGPUTexture works on any
  // texture regardless of usage flags (see SDL_render_gpu.c for precedent).
  {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = fb_w;
    info.height = fb_h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.usage =
        SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    g_gpu.cpc_texture = SDL_CreateGPUTexture(g_gpu.device, &info);
    g_gpu.cpc_tex_w = fb_w;
    g_gpu.cpc_tex_h = fb_h;
  }

  // ── 6. Create transfer buffer (staging for CPU→GPU upload) ────────
  {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = fb_w * fb_h * 4;  // RGBA8 = 4 bytes/pixel
    g_gpu.cpc_upload = SDL_CreateGPUTransferBuffer(g_gpu.device, &info);
  }

  // ── Verify all mandatory resources were created ───────────────────
  if (!g_gpu.linear_sampler || !g_gpu.nearest_sampler || !g_gpu.cpc_texture ||
      !g_gpu.cpc_upload) {
    LOG_ERROR("GPU resource creation failed — rolling back");
    video_gpu_shutdown();
    return false;
  }

  // ── 7. Blit shaders + pipeline ────────────────────────────────────
  // Non-fatal if the backend has no blob yet — plugins will fall back
  // to the GL path on those platforms.  On Metal (the dev target), a
  // failure here is a shader source bug and should be treated as one.
  if (create_blit_shaders(driver)) {
    if (!create_blit_pipeline()) {
      LOG_ERROR("Blit pipeline creation failed — continuing without it");
    }
  }

  g_gpu.initialized = true;
  LOG_INFO("GPU scaffolding initialized (" << fb_w << "×" << fb_h << ", "
                                           << (fb_w * fb_h * 4)
                                           << " bytes staging)");
  return true;
}

void video_gpu_shutdown() {
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
    if (g_gpu.blit_fragment_shader) {
      SDL_ReleaseGPUShader(g_gpu.device, g_gpu.blit_fragment_shader);
    }
    if (g_gpu.blit_vertex_shader) {
      SDL_ReleaseGPUShader(g_gpu.device, g_gpu.blit_vertex_shader);
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

  g_gpu = GpuState{};  // zero all fields, initialized = false
  LOG_INFO("GPU scaffolding shut down");
}

uintptr_t video_gpu_make_rgba_texture(const unsigned char* rgba, int w, int h) {
  if (!g_gpu.device || !rgba || w <= 0 || h <= 0) return 0;

  const uint32_t uw = static_cast<uint32_t>(w);
  const uint32_t uh = static_cast<uint32_t>(h);
  const uint32_t bytes = uw * uh * 4;

  // Match cpc_texture: RGBA8 UNORM, SAMPLER usage (ImGui_ImplSDLGPU3 samples
  // it). COLOR_TARGET isn't needed for an upload-only thumbnail.
  SDL_GPUTextureCreateInfo tinfo{};
  tinfo.type = SDL_GPU_TEXTURETYPE_2D;
  tinfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  tinfo.width = uw;
  tinfo.height = uh;
  tinfo.layer_count_or_depth = 1;
  tinfo.num_levels = 1;
  tinfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  SDL_GPUTexture* tex = SDL_CreateGPUTexture(g_gpu.device, &tinfo);
  if (!tex) {
    LOG_ERROR("video_gpu_make_rgba_texture: SDL_CreateGPUTexture failed: "
              << SDL_GetError());
    return 0;
  }

  SDL_GPUTransferBufferCreateInfo binfo{};
  binfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  binfo.size = bytes;
  SDL_GPUTransferBuffer* xfer =
      SDL_CreateGPUTransferBuffer(g_gpu.device, &binfo);
  if (!xfer) {
    LOG_ERROR("video_gpu_make_rgba_texture: transfer buffer failed: "
              << SDL_GetError());
    SDL_ReleaseGPUTexture(g_gpu.device, tex);
    return 0;
  }

  void* dst = SDL_MapGPUTransferBuffer(g_gpu.device, xfer, /*cycle=*/false);
  if (!dst) {
    LOG_ERROR("video_gpu_make_rgba_texture: map failed: " << SDL_GetError());
    SDL_ReleaseGPUTransferBuffer(g_gpu.device, xfer);
    SDL_ReleaseGPUTexture(g_gpu.device, tex);
    return 0;
  }
  std::memcpy(dst, rgba, bytes);
  SDL_UnmapGPUTransferBuffer(g_gpu.device, xfer);

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_gpu.device);
  if (!cmd) {
    LOG_ERROR("video_gpu_make_rgba_texture: acquire cmd buffer failed: "
              << SDL_GetError());
    SDL_ReleaseGPUTransferBuffer(g_gpu.device, xfer);
    SDL_ReleaseGPUTexture(g_gpu.device, tex);
    return 0;
  }

  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo src_info{};
  src_info.transfer_buffer = xfer;
  src_info.offset = 0;
  src_info.pixels_per_row = uw;
  src_info.rows_per_layer = uh;
  SDL_GPUTextureRegion dst_region{};
  dst_region.texture = tex;
  dst_region.w = uw;
  dst_region.h = uh;
  dst_region.d = 1;
  SDL_UploadToGPUTexture(copy, &src_info, &dst_region, /*cycle=*/false);
  SDL_EndGPUCopyPass(copy);
  SDL_SubmitGPUCommandBuffer(cmd);

  // The transfer buffer is no longer needed; keep the texture.
  SDL_ReleaseGPUTransferBuffer(g_gpu.device, xfer);
  return reinterpret_cast<uintptr_t>(tex);
}

void video_gpu_free_rgba_texture(uintptr_t tex) {
  if (!tex || !g_gpu.device) return;
  SDL_ReleaseGPUTexture(g_gpu.device, reinterpret_cast<SDL_GPUTexture*>(tex));
}

void video_gpu_set_main_present_mode(bool vsync) {
  if (!g_gpu.device || !g_gpu.window) return;
  // VSYNC is SDL's default after claiming the window, so vsync=1 is a no-op
  // (identical to today).  Only switch the MAIN window when vsync is disabled.
  if (vsync) return;
  // Prefer MAILBOX (low-latency, tear-free) and fall back to IMMEDIATE; both
  // avoid the vsync-locked Metal acquire stall over remote desktop.  Only the
  // main window is touched — viewport swapchains stay VSYNC (see video.cpp).
  SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
  if (SDL_WindowSupportsGPUPresentMode(g_gpu.device, g_gpu.window,
                                       SDL_GPU_PRESENTMODE_MAILBOX)) {
    mode = SDL_GPU_PRESENTMODE_MAILBOX;
  } else if (SDL_WindowSupportsGPUPresentMode(g_gpu.device, g_gpu.window,
                                              SDL_GPU_PRESENTMODE_IMMEDIATE)) {
    mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
  }
  if (mode == SDL_GPU_PRESENTMODE_VSYNC) {
    LOG_INFO(
        "video.vsync=0 requested but no non-VSYNC present mode is supported — "
        "staying on VSYNC");
    return;
  }
  if (!SDL_SetGPUSwapchainParameters(g_gpu.device, g_gpu.window,
                                     SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
    LOG_ERROR("SDL_SetGPUSwapchainParameters failed: "
              << SDL_GetError() << " — staying on VSYNC");
    return;
  }
  LOG_INFO("Main-window present mode: "
           << (mode == SDL_GPU_PRESENTMODE_MAILBOX ? "MAILBOX" : "IMMEDIATE")
           << " (video.vsync=0)");
}
