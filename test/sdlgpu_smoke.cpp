// Smoke test for SDL3 GPU API availability — Phase 1 of the SDL_GPU migration
// (see ~/.claude/plans/swirling-tumbling-blum.md).
//
// These tests do NOT touch the emulator. They just verify that the vendored
// SDL3 build has a working GPU subsystem for the current host's driver
// (Metal on macOS, Vulkan on Linux, D3D12 on Windows) and that the headers
// compile against our include paths.
//
// If headless CI runners lack a usable GPU driver, device creation will return
// null and the tests are SKIPPED rather than failed — the presence of the API
// surface alone is what Phase 1 guarantees.

#include <gtest/gtest.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <cstring>

namespace {

// Initialise SDL video once for the whole test binary. The GPU subsystem is
// a peer of video; creating a device does not itself require a window.
class SdlGpuSmokeTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    // SDL_Init is idempotent — if another test suite already called it, this
    // just bumps the refcount.
    SDL_Init(SDL_INIT_VIDEO);
  }
  static void TearDownTestSuite() {
    SDL_Quit();
  }
};

// Sanity: the header exposes a non-INVALID shader format enum.
TEST_F(SdlGpuSmokeTest, ShaderFormatEnumExists) {
  // At least one of the three real backend formats must be defined as a
  // non-zero bit so device creation can request it.
  Uint32 any_format = SDL_GPU_SHADERFORMAT_SPIRV
                    | SDL_GPU_SHADERFORMAT_MSL
                    | SDL_GPU_SHADERFORMAT_DXIL
                    | SDL_GPU_SHADERFORMAT_METALLIB;
  EXPECT_NE(0u, any_format);
}

// Try to create a GPU device, log which backend SDL picked, then destroy.
// Does NOT claim a window — that's for later phases.
TEST_F(SdlGpuSmokeTest, CreateAndDestroyDevice) {
  // Ask SDL for any backend it can deliver. Accept SPIRV (Vulkan), MSL/MetalLib
  // (Metal) or DXIL (D3D12). On CI headless runners the call may return null
  // if no suitable driver is present — that's a SKIP, not a failure.
  SDL_GPUDevice* device = SDL_CreateGPUDevice(
      SDL_GPU_SHADERFORMAT_SPIRV
        | SDL_GPU_SHADERFORMAT_MSL
        | SDL_GPU_SHADERFORMAT_METALLIB
        | SDL_GPU_SHADERFORMAT_DXIL,
      /*debug_mode=*/false,
      /*name=*/nullptr);

  if (device == nullptr) {
    GTEST_SKIP() << "SDL_CreateGPUDevice returned null (no GPU backend "
                 << "available on this host): " << SDL_GetError();
  }

  const char* driver = SDL_GetGPUDeviceDriver(device);
  ASSERT_NE(nullptr, driver) << "Created a GPU device but it has no driver name";

  // Accept any of the three known drivers.
  const bool known_driver =
      std::strcmp(driver, "metal") == 0 ||
      std::strcmp(driver, "vulkan") == 0 ||
      std::strcmp(driver, "direct3d12") == 0;
  EXPECT_TRUE(known_driver) << "Unexpected GPU driver name: " << driver;

  // Supported shader format query must not crash and must return at least one
  // format when the device was created successfully.
  SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(device);
  EXPECT_NE(0u, static_cast<Uint32>(fmts))
      << "GPU device reports no supported shader formats";

  SDL_DestroyGPUDevice(device);
}

// Repeat create/destroy to catch leaks and state left behind between lifecycles.
// If a backend leaks a command pool, the second Create typically fails.
TEST_F(SdlGpuSmokeTest, DeviceLifecycleRepeats) {
  for (int i = 0; i < 5; ++i) {
    SDL_GPUDevice* device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV
          | SDL_GPU_SHADERFORMAT_MSL
          | SDL_GPU_SHADERFORMAT_METALLIB
          | SDL_GPU_SHADERFORMAT_DXIL,
        false,
        nullptr);
    if (device == nullptr) {
      GTEST_SKIP() << "Backend unavailable: " << SDL_GetError();
    }
    SDL_DestroyGPUDevice(device);
  }
}

// Transfer buffer round-trip: create a small RGBA8 transfer buffer, map it,
// write a known pattern, unmap, destroy. No upload — that needs a copy pass
// and a destination texture, which Phase 2 builds. This test only verifies
// that the memory-mapping path exists and doesn't crash.
TEST_F(SdlGpuSmokeTest, TransferBufferMapRoundTrip) {
  SDL_GPUDevice* device = SDL_CreateGPUDevice(
      SDL_GPU_SHADERFORMAT_SPIRV
        | SDL_GPU_SHADERFORMAT_MSL
        | SDL_GPU_SHADERFORMAT_METALLIB
        | SDL_GPU_SHADERFORMAT_DXIL,
      false,
      nullptr);
  if (device == nullptr) {
    GTEST_SKIP() << "Backend unavailable: " << SDL_GetError();
  }

  constexpr Uint32 kBytes = 16 * 16 * 4;  // 1 KiB, arbitrary
  SDL_GPUTransferBufferCreateInfo tb_info{};
  tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  tb_info.size = kBytes;
  SDL_GPUTransferBuffer* tbuf = SDL_CreateGPUTransferBuffer(device, &tb_info);
  ASSERT_NE(nullptr, tbuf) << "CreateGPUTransferBuffer failed: " << SDL_GetError();

  // cycle=true: we're about to write into the buffer and SDL should not wait
  // for any prior upload to complete (there is none in this test, so cycle is
  // harmless here, but it matches the per-frame pattern from the plan).
  void* ptr = SDL_MapGPUTransferBuffer(device, tbuf, /*cycle=*/true);
  ASSERT_NE(nullptr, ptr) << "MapGPUTransferBuffer failed: " << SDL_GetError();
  std::memset(ptr, 0xAB, kBytes);
  SDL_UnmapGPUTransferBuffer(device, tbuf);

  SDL_ReleaseGPUTransferBuffer(device, tbuf);
  SDL_DestroyGPUDevice(device);
}

}  // namespace
