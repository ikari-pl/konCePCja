#include "gfx_finder.h"
#include "koncepcja.h"
#include <cstdio>
#include <cstring>

// CPC pixel decoding tables
// Mode 0: 2 pixels per byte, 4 bits per pixel (16 colors)
// Bit layout: pixel0 = {b7,b5,b3,b1}, pixel1 = {b6,b4,b2,b0}
// Mode 1: 4 pixels per byte, 2 bits per pixel (4 colors)
// Bit layout: px0={b7,b3}, px1={b6,b2}, px2={b5,b1}, px3={b4,b0}
// Mode 2: 8 pixels per byte, 1 bit per pixel (2 colors)
// Bit layout: px0=b7, px1=b6, px2=b5, px3=b4, px4=b3, px5=b2, px6=b1, px7=b0

int gfx_decode_byte(uint8_t val, int mode, uint8_t* out) {
    switch (mode) {
    case 0: {
        // 2 pixels, 4 bits each (interleaved)
        // CPC hardware decodes: px0 bits {7,5,3,1}, px1 bits {6,4,2,0}
        out[0] = ((val & 0x80) >> 7) | ((val & 0x20) >> 4) |
                 ((val & 0x08) >> 1) | ((val & 0x02) << 2);
        out[1] = ((val & 0x40) >> 6) | ((val & 0x10) >> 3) |
                 ((val & 0x04) >> 0) | ((val & 0x01) << 3);
        return 2;
    }
    case 1: {
        // 4 pixels, 2 bits each
        out[0] = ((val & 0x80) >> 7) | ((val & 0x08) >> 2);
        out[1] = ((val & 0x40) >> 6) | ((val & 0x04) >> 1);
        out[2] = ((val & 0x20) >> 5) | ((val & 0x02) >> 0);
        out[3] = ((val & 0x10) >> 4) | ((val & 0x01) << 1);
        return 4;
    }
    case 2: {
        // 8 pixels, 1 bit each
        for (int i = 0; i < 8; i++) {
            out[i] = (val >> (7 - i)) & 1;
        }
        return 8;
    }
    default:
        return 0;
    }
}

uint8_t gfx_encode_byte(const uint8_t* indices, int mode) {
    uint8_t result = 0;
    switch (mode) {
    case 0: {
        // Reverse of decode: pixel0 bits → {7,5,3,1}, pixel1 bits → {6,4,2,0}
        uint8_t p0 = indices[0] & 0x0F;
        uint8_t p1 = indices[1] & 0x0F;
        result = ((p0 & 1) << 7) | ((p0 & 2) << 4) |
                 ((p0 & 4) << 1) | ((p0 & 8) >> 2) |
                 ((p1 & 1) << 6) | ((p1 & 2) << 3) |
                 ((p1 & 4) >> 0) | ((p1 & 8) >> 3);
        break;
    }
    case 1: {
        uint8_t p0 = indices[0] & 3, p1 = indices[1] & 3;
        uint8_t p2 = indices[2] & 3, p3 = indices[3] & 3;
        result = ((p0 & 1) << 7) | ((p0 & 2) << 2) |
                 ((p1 & 1) << 6) | ((p1 & 2) << 1) |
                 ((p2 & 1) << 5) | ((p2 & 2) << 0) |
                 ((p3 & 1) << 4) | ((p3 & 2) >> 1);
        break;
    }
    case 2: {
        for (int i = 0; i < 8; i++) {
            result |= ((indices[i] & 1) << (7 - i));
        }
        break;
    }
    }
    return result;
}

int gfx_decode(const uint8_t* mem, size_t mem_size,
               const GfxViewParams& params,
               const uint32_t* palette_rgba,
               std::vector<uint32_t>& pixels_out) {
    if (params.mode < 0 || params.mode > 2) return 0;
    if (params.width <= 0 || params.height <= 0) return 0;

    int ppb = (params.mode == 0) ? 2 : (params.mode == 1) ? 4 : 8;
    int pixel_width = params.width * ppb;
    pixels_out.resize(static_cast<size_t>(pixel_width) * params.height);

    for (int row = 0; row < params.height; row++) {
        for (int col = 0; col < params.width; col++) {
            size_t addr = (static_cast<size_t>(params.address) +
                          static_cast<size_t>(row) * params.width + col) & 0xFFFF;
            uint8_t byte_val = (addr < mem_size) ? mem[addr] : 0;

            uint8_t indices[8];
            int count = gfx_decode_byte(byte_val, params.mode, indices);

            for (int p = 0; p < count; p++) {
                int px = col * ppb + p;
                int idx = row * pixel_width + px;
                pixels_out[idx] = palette_rgba[indices[p]];
            }
        }
    }
    return pixel_width;
}

// Write a 32-bit value in little-endian to a byte buffer
static void write_le32(uint8_t* dst, int32_t val) {
    dst[0] = static_cast<uint8_t>(val);
    dst[1] = static_cast<uint8_t>(val >> 8);
    dst[2] = static_cast<uint8_t>(val >> 16);
    dst[3] = static_cast<uint8_t>(val >> 24);
}

static void write_le16(uint8_t* dst, uint16_t val) {
    dst[0] = static_cast<uint8_t>(val);
    dst[1] = static_cast<uint8_t>(val >> 8);
}

bool gfx_export_bmp(const std::string& path,
                    const uint32_t* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) return false;

    // BMP format: BITMAPFILEHEADER (14) + BITMAPINFOHEADER (40) + pixel data
    int row_bytes = width * 4;  // 32-bit BGRA, no padding needed for 4-byte pixels
    int data_size = row_bytes * height;
    int file_size = 14 + 40 + data_size;

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;

    // BITMAPFILEHEADER (portable little-endian writes)
    uint8_t hdr[14] = {'B', 'M'};
    write_le32(hdr + 2, file_size);
    write_le32(hdr + 10, 54);
    if (fwrite(hdr, 1, 14, f) != 14) { fclose(f); return false; }

    // BITMAPINFOHEADER
    uint8_t info[40] = {};
    write_le32(info + 0, 40);
    write_le32(info + 4, width);
    write_le32(info + 8, -height);  // top-down BMP
    write_le16(info + 12, 1);      // planes
    write_le16(info + 14, 32);     // bpp
    write_le32(info + 20, data_size);
    if (fwrite(info, 1, 40, f) != 40) { fclose(f); return false; }

    // Pixel data: convert RGBA to BGRA
    std::vector<uint8_t> row(static_cast<size_t>(row_bytes));
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t rgba = pixels[y * width + x];
            uint8_t r = (rgba >> 0) & 0xFF;
            uint8_t g = (rgba >> 8) & 0xFF;
            uint8_t b = (rgba >> 16) & 0xFF;
            uint8_t a = (rgba >> 24) & 0xFF;
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = a;
        }
        if (fwrite(row.data(), 1, static_cast<size_t>(row_bytes), f) !=
            static_cast<size_t>(row_bytes)) {
            fclose(f);
            return false;
        }
    }

    fclose(f);
    return true;
}

extern t_GateArray GateArray;
extern double colours_rgb[32][3];
extern t_CPC CPC;

void gfx_get_palette_rgba(uint32_t* palette_out, int num_colors) {
    if (num_colors > 16) num_colors = 16;
    double intensity = CPC.scr_intensity / 10.0;
    for (int i = 0; i < num_colors; i++) {
        int hw_color = GateArray.ink_values[i];
        if (hw_color < 0 || hw_color > 31) hw_color = 0;
        uint8_t r = static_cast<uint8_t>(colours_rgb[hw_color][0] * intensity * 255);
        uint8_t g = static_cast<uint8_t>(colours_rgb[hw_color][1] * intensity * 255);
        uint8_t b = static_cast<uint8_t>(colours_rgb[hw_color][2] * intensity * 255);
        palette_out[i] = static_cast<uint32_t>(r) |
                         (static_cast<uint32_t>(g) << 8) |
                         (static_cast<uint32_t>(b) << 16) |
                         (0xFFu << 24);
    }
}

bool gfx_paint(uint8_t* mem, size_t mem_size,
               const GfxViewParams& params,
               int x, int y, uint8_t color_index) {
    if (params.mode < 0 || params.mode > 2) return false;

    int ppb = (params.mode == 0) ? 2 : (params.mode == 1) ? 4 : 8;
    int pixel_width = params.width * ppb;
    if (x < 0 || x >= pixel_width || y < 0 || y >= params.height) return false;

    int byte_col = x / ppb;
    int pixel_in_byte = x % ppb;

    size_t addr = (static_cast<size_t>(params.address) +
                  static_cast<size_t>(y) * params.width + byte_col) & 0xFFFF;
    if (addr >= mem_size) return false;

    // Decode existing byte, modify one pixel, re-encode
    uint8_t indices[8];
    gfx_decode_byte(mem[addr], params.mode, indices);
    indices[pixel_in_byte] = color_index;
    mem[addr] = gfx_encode_byte(indices, params.mode);
    return true;
}
