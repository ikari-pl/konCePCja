#include "host_surface.h"

#include <cstring>

#include "savepng.h"

#ifdef KONCPC_SDL

#include <SDL3/SDL.h>

HostSurface* host_surface_create_rgba32(int width, int height) {
  SDL_Surface* s = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
  if (s == nullptr) return nullptr;
  SDL_FillSurfaceRect(s, nullptr, host_surface_map_rgb(s, 0, 0, 0));
  return s;
}

void host_surface_destroy(HostSurface* surface) {
  if (surface != nullptr) SDL_DestroySurface(surface);
}

int host_surface_bits_per_pixel(const HostSurface* surface) {
  const SDL_PixelFormatDetails* fmt =
      SDL_GetPixelFormatDetails(surface->format);
  return fmt ? fmt->bits_per_pixel : 0;
}

uint32_t host_surface_map_rgb(const HostSurface* surface, uint8_t r, uint8_t g,
                              uint8_t b) {
  const SDL_PixelFormatDetails* fmt =
      SDL_GetPixelFormatDetails(surface->format);
  // SDL_GetSurfacePalette takes a non-const surface but only reads it.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  SDL_Palette const* pal =
      SDL_GetSurfacePalette(const_cast<SDL_Surface*>(surface));
  return SDL_MapRGB(fmt, pal, r, g, b);
}

bool host_surface_save_png(HostSurface* surface, const std::string& file,
                           std::string& error) {
  if (SDL_SavePNG(surface, file) != 0) {
    error = SDL_GetError();
    return false;
  }
  return true;
}

#else

#include <new>

HostSurface* host_surface_create_rgba32(int width, int height) {
  if (width <= 0 || height <= 0) return nullptr;
  auto* s = new (std::nothrow) HostSurface;
  if (s == nullptr) return nullptr;
  s->w = width;
  s->h = height;
  s->pitch = width * 4;
  const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
  auto* px = new (std::nothrow) uint32_t[count];
  if (px == nullptr) {
    delete s;
    return nullptr;
  }
  const uint32_t black = host_surface_map_rgb(s, 0, 0, 0);
  for (size_t i = 0; i < count; ++i) px[i] = black;
  s->pixels = px;
  return s;
}

void host_surface_destroy(HostSurface* surface) {
  if (surface == nullptr) return;
  delete[] static_cast<uint32_t*>(surface->pixels);
  delete surface;
}

int host_surface_bits_per_pixel(const HostSurface* /*surface*/) { return 32; }

uint32_t host_surface_map_rgb(const HostSurface* /*surface*/, uint8_t r,
                              uint8_t g, uint8_t b) {
  return host_rgba32_pixel(r, g, b);
}

bool host_surface_save_png(HostSurface* surface, const std::string& file,
                           std::string& error) {
  if (surface == nullptr) {
    error = "no surface";
    return false;
  }
  return save_png_rgba32(static_cast<const uint8_t*>(surface->pixels),
                         surface->w, surface->h, surface->pitch, file,
                         error) == 0;
}

#endif

uint32_t host_rgba32_pixel(uint8_t r, uint8_t g, uint8_t b) {
  // RGBA32 is a byte order, so build the value from bytes.
  const uint8_t bytes[4] = {r, g, b, 0xFF};
  uint32_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

void host_blit_rgb24_to_rgba32_nearest(const uint8_t* src, int src_w,
                                       int src_pitch, int src_y, int src_h,
                                       uint8_t* dst, int dst_w, int dst_h,
                                       int dst_pitch) {
  if (src == nullptr || dst == nullptr || src_w <= 0 || src_h <= 0 ||
      dst_w <= 0 || dst_h <= 0)
    return;
  for (int y = 0; y < dst_h; ++y) {
    const int sy = src_y + ((y * src_h) / dst_h);
    const uint8_t* srow = src + (static_cast<size_t>(sy) * src_pitch);
    uint8_t* drow = dst + (static_cast<size_t>(y) * dst_pitch);
    for (int x = 0; x < dst_w; ++x) {
      const uint8_t* sp = srow + (static_cast<size_t>((x * src_w) / dst_w) * 3);
      uint8_t* dp = drow + (static_cast<size_t>(x) * 4);
      dp[0] = sp[0];
      dp[1] = sp[1];
      dp[2] = sp[2];
      dp[3] = 0xFF;
    }
  }
}
