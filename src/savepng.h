#pragma once

// PNG screenshot writer over libpng.

#include <cstdint>
#include <string>

// Write a packed 8-bit RGBA image (R, G, B, A bytes per pixel, `pitch` bytes
// per row) to `file` as an 8-bit RGBA PNG.
//
// Returns 0 on success, -1 on failure with the reason in `error` (this covers
// libpng errors and short/failed writes, e.g. a full disk, which are detected
// on both write and close). Needs no SDL: the SDL-free build writes its
// screenshots through this directly.
[[nodiscard]] int save_png_rgba32(const uint8_t* pixels, int width, int height,
                                  int pitch, const std::string& file,
                                  std::string& error);

#ifdef KONCPC_SDL
#include <SDL3/SDL_surface.h>

// Write `surface` to `file` as an 8-bit RGBA PNG.
//
// Any SDL pixel format is accepted; the pixels are normalised to RGBA32
// before encoding, so paletted / 16-bit / BGR surfaces all produce the same
// kind of file. The surface itself is not modified.
//
// Returns 0 on success, -1 on failure — the reason is then retrievable via
// SDL_GetError().
[[nodiscard]] int SDL_SavePNG(SDL_Surface* src, const std::string& file);
#endif
