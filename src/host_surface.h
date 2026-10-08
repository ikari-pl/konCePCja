#pragma once

// The host's CPC frame surface, with or without SDL (beads-29zx).
//
// back_surface is the frame the bridge blits the machine's RGB24 framebuffer
// into, and what screenshots, `hash vram`, the GIF/AVI recorders and the M4
// preview read. In the GUI build it is an SDL_Surface of whatever format the
// video plugin chose, and HostSurface is SDL_Surface: nothing changes there.
// In the SDL-free build (KONCPC_MODERN_UI=0) the only surface is the headless
// one, so HostSurface is a plain RGBA32 buffer with the same w/h/pitch/pixels
// members the readers use. SDL_PIXELFORMAT_RGBA32 is also what the headless
// plugin asks SDL for, so both builds hold the same bytes for the same frame.

#include <cstdint>
#include <string>

#ifdef KONCPC_SDL
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
using HostSurface = SDL_Surface;
using HostColor = SDL_Color;
#else
struct HostSurface {
  int w = 0;
  int h = 0;
  int pitch = 0;           // bytes per row
  void* pixels = nullptr;  // R, G, B, A bytes per pixel
};
struct HostColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
};
#endif

// An opaque-black RGBA32 surface, or null on failure.
HostSurface* host_surface_create_rgba32(int width, int height);
void host_surface_destroy(HostSurface* surface);

int host_surface_bits_per_pixel(const HostSurface* surface);

// The pixel value of an opaque (r, g, b) in `surface`'s format.
uint32_t host_surface_map_rgb(const HostSurface* surface, uint8_t r, uint8_t g,
                              uint8_t b);

// The value of an opaque (r, g, b) pixel in RGBA32 byte order, as a native
// uint32_t: what SDL_MapRGB returns for SDL_PIXELFORMAT_RGBA32 on either
// endianness. The SDL-free surface's host_surface_map_rgb.
uint32_t host_rgba32_pixel(uint8_t r, uint8_t g, uint8_t b);

// Write `surface` to `file` as an 8-bit RGBA PNG. Returns false and fills
// `error` on failure.
bool host_surface_save_png(HostSurface* surface, const std::string& file,
                           std::string& error);

// Nearest-neighbour copy of rows [src_y, src_y + src_h) of a packed RGB24
// frame onto the whole of an RGBA32 destination, alpha 255. Destination pixel
// (x, y) takes source pixel (x * src_w / dst_w, src_y + y * src_h / dst_h),
// the mapping subcycle_bridge_scanline_gap_row() also uses. For the headless
// geometry (768x272 cropped to 270 rows, onto 768x270) that is a straight
// copy, the same bytes SDL_BlitSurfaceScaled produces in the GUI build.
void host_blit_rgb24_to_rgba32_nearest(const uint8_t* src, int src_w,
                                       int src_pitch, int src_y, int src_h,
                                       uint8_t* dst, int dst_w, int dst_h,
                                       int dst_pitch);
