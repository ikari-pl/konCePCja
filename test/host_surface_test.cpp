// The SDL-free host layer (beads-29zx) checked against SDL itself.
//
// The UI-free build (`koncepcja -H`, KONCPC_MODERN_UI=0) links no SDL: its
// frame surface, the bridge's blit into it, its pixel values and its PNG
// writer are the code in src/host_surface.cpp and src/savepng.cpp. That build
// has no test_runner, so these tests run the same pure functions here, in the
// GUI build, next to the SDL calls they replace. Byte-identical results mean a
// `hash vram` or a screenshot of the same frame agrees between `koncepcja -H`
// built with and without SDL.

#include "host_surface.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <png.h>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "koncepcja.h"
#include "savepng.h"
#include "subcycle/machine.h"

namespace {

struct SurfaceDeleter {
  void operator()(SDL_Surface* s) const { SDL_DestroySurface(s); }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

std::vector<uint8_t> random_rgb24(int w, int h, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 3);
  for (auto& b : fb) b = static_cast<uint8_t>(rng());
  return fb;
}

// What the GUI build's bridge does (subcycle_bridge.cpp blit_fb, SDL path):
// RGB24 -> dst-format convert, then a NEAREST stretch of rows
// [src_y, src_y + src_h) onto the whole destination.
std::vector<uint8_t> sdl_blit(std::vector<uint8_t>& fb, int fb_w, int fb_h,
                              int src_y, int src_h, int dst_w, int dst_h) {
  SurfacePtr fbsurf(SDL_CreateSurfaceFrom(fb_w, fb_h, SDL_PIXELFORMAT_RGB24,
                                          fb.data(), fb_w * 3));
  SurfacePtr fbconv(SDL_CreateSurface(fb_w, fb_h, SDL_PIXELFORMAT_RGBA32));
  SurfacePtr dst(SDL_CreateSurface(dst_w, dst_h, SDL_PIXELFORMAT_RGBA32));
  EXPECT_TRUE(fbsurf && fbconv && dst);
  if (!fbsurf || !fbconv || !dst) return {};
  SDL_SetSurfaceBlendMode(fbconv.get(), SDL_BLENDMODE_NONE);
  SDL_BlitSurface(fbsurf.get(), nullptr, fbconv.get(), nullptr);
  SDL_Rect src{0, src_y, fb_w, src_h};
  SDL_BlitSurfaceScaled(fbconv.get(), &src, dst.get(), nullptr,
                        SDL_SCALEMODE_NEAREST);
  std::vector<uint8_t> out;
  const auto* px = static_cast<const uint8_t*>(dst->pixels);
  for (int y = 0; y < dst_h; ++y) {
    out.insert(out.end(), px + (static_cast<size_t>(y) * dst->pitch),
               px + (static_cast<size_t>(y) * dst->pitch) + (dst_w * 4));
  }
  return out;
}

std::vector<uint8_t> host_blit(const std::vector<uint8_t>& fb, int fb_w,
                               int src_y, int src_h, int dst_w, int dst_h) {
  std::vector<uint8_t> out(static_cast<size_t>(dst_w) * dst_h * 4, 0);
  host_blit_rgb24_to_rgba32_nearest(fb.data(), fb_w, fb_w * 3, src_y, src_h,
                                    out.data(), dst_w, dst_h, dst_w * 4);
  return out;
}

}  // namespace

TEST(HostSurface, BlitMatchesSdlForTheHeadlessGeometry) {
  // The machine's 768x272 frame onto the headless 768x270 surface: blit_fb
  // crops one row off the top and bottom and copies 1:1.
  const int fb_w = subcycle::kFbWidth;
  const int fb_h = subcycle::kFbHeight;
  auto fb = random_rgb24(fb_w, fb_h, 29);
  const int src_h = CPC_VISIBLE_SCR_HEIGHT;
  const int src_y = (fb_h - src_h) / 2;

  const auto want = sdl_blit(fb, fb_w, fb_h, src_y, src_h, CPC_RENDER_WIDTH,
                             CPC_VISIBLE_SCR_HEIGHT);
  const auto got = host_blit(fb, fb_w, src_y, src_h, CPC_RENDER_WIDTH,
                             CPC_VISIBLE_SCR_HEIGHT);
  ASSERT_EQ(want.size(), got.size());
  EXPECT_TRUE(want == got);
}

TEST(HostSurface, BlitMatchesSdlAtAnIntegerUpscale) {
  // The line-doubled 768x540 surface the windowed plugins use: every source
  // row lands on two destination rows, in SDL and here alike.
  const int fb_w = subcycle::kFbWidth;
  const int fb_h = subcycle::kFbHeight;
  auto fb = random_rgb24(fb_w, fb_h, 540);
  const int src_h = 270;
  const int src_y = (fb_h - src_h) / 2;

  const auto want = sdl_blit(fb, fb_w, fb_h, src_y, src_h, 768, 540);
  const auto got = host_blit(fb, fb_w, src_y, src_h, 768, 540);
  ASSERT_EQ(want.size(), got.size());
  EXPECT_TRUE(want == got);
}

TEST(HostSurface, Rgba32PixelMatchesSdlMapRgb) {
  const SDL_PixelFormatDetails* fmt =
      SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA32);
  ASSERT_NE(nullptr, fmt);
  const uint8_t samples[][3] = {
      {0, 0, 0},   {255, 255, 255},    {255, 0, 0},       {0, 255, 0},
      {0, 0, 255}, {0x12, 0x34, 0x56}, {0x80, 0x7F, 0x01}};
  for (const auto& c : samples) {
    EXPECT_EQ(SDL_MapRGB(fmt, nullptr, c[0], c[1], c[2]),
              host_rgba32_pixel(c[0], c[1], c[2]))
        << "rgb " << int{c[0]} << "," << int{c[1]} << "," << int{c[2]};
  }
}

TEST(HostSurface, HeadlessSurfaceStartsOpaqueBlack) {
  HostSurface* s =
      host_surface_create_rgba32(CPC_RENDER_WIDTH, CPC_VISIBLE_SCR_HEIGHT);
  ASSERT_NE(nullptr, s);
  EXPECT_EQ(32, host_surface_bits_per_pixel(s));
  const auto* px = static_cast<const uint8_t*>(s->pixels);
  for (int y = 0; y < s->h; y += 67) {
    const uint8_t* p = px + (static_cast<size_t>(y) * s->pitch);
    EXPECT_EQ(0, p[0]);
    EXPECT_EQ(0, p[1]);
    EXPECT_EQ(0, p[2]);
    EXPECT_EQ(255, p[3]);
  }
  host_surface_destroy(s);
}

TEST(SavePng, Rgba32RoundTripsThroughLibpng) {
  const int w = 5;
  const int h = 3;
  const int pitch = (w * 4) + 8;  // padded rows, as surfaces have
  std::vector<uint8_t> pixels(static_cast<size_t>(pitch) * h, 0xEE);
  std::mt19937 rng(7);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w * 4; ++x)
      pixels[(static_cast<size_t>(y) * pitch) + x] =
          static_cast<uint8_t>(rng());

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "koncpc_savepng_roundtrip.png";
  std::string error;
  ASSERT_EQ(0,
            save_png_rgba32(pixels.data(), w, h, pitch, path.string(), error))
      << error;

  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  ASSERT_NE(0, png_image_begin_read_from_file(&image, path.string().c_str()));
  image.format = PNG_FORMAT_RGBA;
  ASSERT_EQ(static_cast<png_uint_32>(w), image.width);
  ASSERT_EQ(static_cast<png_uint_32>(h), image.height);
  std::vector<uint8_t> decoded(PNG_IMAGE_SIZE(image));
  ASSERT_NE(0,
            png_image_finish_read(&image, nullptr, decoded.data(), 0, nullptr));
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w * 4; ++x)
      EXPECT_EQ(pixels[(static_cast<size_t>(y) * pitch) + x],
                decoded[(static_cast<size_t>(y) * w * 4) + x])
          << "byte " << x << " of row " << y;
  std::filesystem::remove(path);
}

TEST(SavePng, ReportsAnUnwritablePath) {
  const uint8_t pixel[4] = {1, 2, 3, 255};
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "koncpc-no-such-dir" / "shot.png";
  std::string error;
  EXPECT_EQ(-1, save_png_rgba32(pixel, 1, 1, 4, path.string(), error));
  EXPECT_FALSE(error.empty());
}
