// Tests for the SDL3 GPU scaffolding (Phase 2 of P1.2b).
//
// These tests exercise the GpuState lifecycle: device creation, window claim,
// sampler/texture/transfer-buffer creation, and orderly shutdown.
// On headless CI runners without a GPU backend, tests SKIP rather than fail.

#include <gtest/gtest.h>

#include <SDL3/SDL.h>
#include "video_gpu.h"
#include <cstring>

namespace {

class VideoGpuTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        SDL_Init(SDL_INIT_VIDEO);
    }
    static void TearDownTestSuite() {
        SDL_Quit();
    }

    void TearDown() override {
        // Ensure cleanup even if a test fails partway through.
        video_gpu_shutdown();
    }

    // Helper: create a minimal hidden window for GPU testing.
    // Returns nullptr if the video subsystem can't create windows (headless).
    SDL_Window* make_test_window() {
        SDL_Window* w = SDL_CreateWindow("gpu-test", 64, 64, SDL_WINDOW_HIDDEN);
        return w;
    }

    // Helper macro: attempt init, skip the test if no GPU backend available.
    // Must be called directly in the test body (not a helper function) because
    // GTEST_SKIP() cannot be used in non-void-returning functions.
    #define TRY_GPU_INIT(w, fb_w, fb_h)                                       \
        do {                                                                   \
            if (!video_gpu_init((w), (fb_w), (fb_h))) {                        \
                video_gpu_shutdown();                                           \
                SDL_DestroyWindow((w));                                         \
                GTEST_SKIP() << "No GPU backend available: " << SDL_GetError();\
            }                                                                  \
        } while (0)
};

TEST_F(VideoGpuTest, InitAndShutdown) {
    SDL_Window* w = make_test_window();
    if (!w) GTEST_SKIP() << "Cannot create window (headless)";

    if (!video_gpu_init(w, 768, 540)) {
        SDL_DestroyWindow(w);
        GTEST_SKIP() << "No GPU backend available: " << SDL_GetError();
    }

    EXPECT_TRUE(g_gpu.initialized);
    EXPECT_NE(g_gpu.device, nullptr);
    EXPECT_NE(g_gpu.linear_sampler, nullptr);
    EXPECT_NE(g_gpu.nearest_sampler, nullptr);
    EXPECT_NE(g_gpu.cpc_texture, nullptr);
    EXPECT_NE(g_gpu.cpc_upload, nullptr);
    EXPECT_EQ(g_gpu.cpc_tex_w, 768u);
    EXPECT_EQ(g_gpu.cpc_tex_h, 540u);

    video_gpu_shutdown();

    EXPECT_FALSE(g_gpu.initialized);
    EXPECT_EQ(g_gpu.device, nullptr);
    EXPECT_EQ(g_gpu.linear_sampler, nullptr);
    EXPECT_EQ(g_gpu.nearest_sampler, nullptr);
    EXPECT_EQ(g_gpu.cpc_texture, nullptr);
    EXPECT_EQ(g_gpu.cpc_upload, nullptr);

    SDL_DestroyWindow(w);
}

TEST_F(VideoGpuTest, SwapchainFormatIsValid) {
    SDL_Window* w = make_test_window();
    if (!w) GTEST_SKIP() << "Cannot create window (headless)";
    TRY_GPU_INIT(w, 768, 540);

    EXPECT_NE(g_gpu.swapchain_fmt, SDL_GPU_TEXTUREFORMAT_INVALID)
        << "Swapchain format should be a real format after init";

    video_gpu_shutdown();
    SDL_DestroyWindow(w);
}

TEST_F(VideoGpuTest, SamplerCreation) {
    SDL_Window* w = make_test_window();
    if (!w) GTEST_SKIP() << "Cannot create window (headless)";
    TRY_GPU_INIT(w, 768, 540);

    EXPECT_NE(g_gpu.linear_sampler, nullptr);
    EXPECT_NE(g_gpu.nearest_sampler, nullptr);

    // They should be distinct objects.
    EXPECT_NE(g_gpu.linear_sampler, g_gpu.nearest_sampler);

    video_gpu_shutdown();
    SDL_DestroyWindow(w);
}

TEST_F(VideoGpuTest, TransferBufferMapCycle) {
    SDL_Window* w = make_test_window();
    if (!w) GTEST_SKIP() << "Cannot create window (headless)";
    TRY_GPU_INIT(w, 768, 540);

    // Map with cycle=true (the per-frame pattern), write a pattern, unmap.
    void* ptr = SDL_MapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload,
                                         /*cycle=*/true);
    ASSERT_NE(ptr, nullptr) << "Map failed: " << SDL_GetError();

    std::memset(ptr, 0xCD, g_gpu.cpc_tex_w * g_gpu.cpc_tex_h * 4);
    SDL_UnmapGPUTransferBuffer(g_gpu.device, g_gpu.cpc_upload);

    video_gpu_shutdown();
    SDL_DestroyWindow(w);
}

TEST_F(VideoGpuTest, DoubleInitIsIdempotent) {
    SDL_Window* w = make_test_window();
    if (!w) GTEST_SKIP() << "Cannot create window (headless)";
    TRY_GPU_INIT(w, 768, 540);

    // Second init with the same window should return true without recreating.
    SDL_GPUDevice* first_device = g_gpu.device;
    EXPECT_TRUE(video_gpu_init(w, 768, 540));
    EXPECT_EQ(g_gpu.device, first_device) << "Double init must not recreate device";

    video_gpu_shutdown();
    SDL_DestroyWindow(w);
}

TEST_F(VideoGpuTest, ShutdownWithoutInitIsSafe) {
    // Must not crash even when nothing was initialized.
    EXPECT_NO_FATAL_FAILURE(video_gpu_shutdown());
    EXPECT_FALSE(g_gpu.initialized);
}

// Only Metal currently has a shader blob shipped (MSL source).  Vulkan
// and D3D12 blob arrays stay empty until a follow-up populates them, so
// the pipeline is expected to be null on those backends.
static bool backend_has_blit_blob() {
    if (!g_gpu.device) return false;
    const char* drv = SDL_GetGPUDeviceDriver(g_gpu.device);
    return drv && std::strcmp(drv, "metal") == 0;
}

TEST_F(VideoGpuTest, BlitShadersCreated) {
    SDL_Window* w = make_test_window();
    if (!w) GTEST_SKIP() << "Cannot create window (headless)";
    TRY_GPU_INIT(w, 768, 540);

    if (!backend_has_blit_blob()) {
        GTEST_SKIP() << "Backend has no blit shader blob yet";
    }
    EXPECT_NE(g_gpu.blit_vertex_shader,   nullptr);
    EXPECT_NE(g_gpu.blit_fragment_shader, nullptr);

    video_gpu_shutdown();
    SDL_DestroyWindow(w);
}

TEST_F(VideoGpuTest, BlitPipelineCreated) {
    SDL_Window* w = make_test_window();
    if (!w) GTEST_SKIP() << "Cannot create window (headless)";
    TRY_GPU_INIT(w, 768, 540);

    if (!backend_has_blit_blob()) {
        GTEST_SKIP() << "Backend has no blit shader blob yet";
    }
    EXPECT_NE(g_gpu.blit_pipeline, nullptr);

    video_gpu_shutdown();
    SDL_DestroyWindow(w);
}

}  // namespace
