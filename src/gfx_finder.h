#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct GfxViewParams {
    uint16_t address;    // start address in CPC memory
    int width;           // width in bytes (not pixels)
    int height;          // height in pixel rows
    int mode;            // CPC screen mode: 0, 1, or 2
};

// Decode a single CPC byte into color indices for the given mode.
// out[] must hold at least: 2 (mode 0), 4 (mode 1), 8 (mode 2).
// Returns the number of pixels decoded, or 0 on invalid mode.
int gfx_decode_byte(uint8_t val, int mode, uint8_t* out);

// Encode pixel color indices back into a CPC byte for the given mode.
// indices must have the correct count: 2/4/8 depending on mode.
uint8_t gfx_encode_byte(const uint8_t* indices, int mode);

// Decode CPC pixels from memory into an RGBA buffer.
// Returns pixel width (mode 0: width*2, mode 1: width*4, mode 2: width*8).
// Returns 0 on error.
int gfx_decode(const uint8_t* mem, size_t mem_size,
               const GfxViewParams& params,
               const uint32_t* palette_rgba,  // 16 or 27 RGBA colors
               std::vector<uint32_t>& pixels_out);

// Export decoded graphics as 32-bit BMP.
bool gfx_export_bmp(const std::string& path,
                    const uint32_t* pixels, int width, int height);

// Get the current CPC palette as RGBA values (reads GateArray.ink_values + colours_rgb).
void gfx_get_palette_rgba(uint32_t* palette_out, int num_colors);

// Paint a single pixel at (x,y) in the memory buffer.
bool gfx_paint(uint8_t* mem, size_t mem_size,
               const GfxViewParams& params,
               int x, int y, uint8_t color_index);
