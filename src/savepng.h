#pragma once

// PNG screenshot writer: encode an SDL_Surface into a PNG file via libpng.

#include <SDL3/SDL_surface.h>

#include <string>

// Write `surface` to `file` as an 8-bit RGBA PNG.
//
// Any SDL pixel format is accepted; the pixels are normalised to RGBA32
// before encoding, so paletted / 16-bit / BGR surfaces all produce the same
// kind of file. The surface itself is not modified.
//
// Returns 0 on success, -1 on failure — the reason is then retrievable via
// SDL_GetError() (this covers libpng errors and short/failed writes, e.g. a
// full disk, which are detected on both write and close).
[[nodiscard]] int SDL_SavePNG(SDL_Surface* src, const std::string& file);
