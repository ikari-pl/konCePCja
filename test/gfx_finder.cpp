#include <gtest/gtest.h>
#include "gfx_finder.h"
#include <cstring>
#include <vector>
#include <filesystem>

namespace {

// --- gfx_decode_byte tests ---

TEST(GfxDecodeByte, Mode0DecodesTwo4BitPixels) {
    uint8_t out[2];
    EXPECT_EQ(2, gfx_decode_byte(0x00, 0, out));
    EXPECT_EQ(0, out[0]);
    EXPECT_EQ(0, out[1]);
}

TEST(GfxDecodeByte, Mode0AllOnesGives15) {
    uint8_t out[2];
    EXPECT_EQ(2, gfx_decode_byte(0xFF, 0, out));
    EXPECT_EQ(15, out[0]);
    EXPECT_EQ(15, out[1]);
}

TEST(GfxDecodeByte, Mode0Pixel0OnlyBit7) {
    // Byte 0x80: bit7=1, rest=0
    // pixel0 gets bit7 as LSB of color → color index 1
    // pixel1 gets nothing → 0
    uint8_t out[2];
    gfx_decode_byte(0x80, 0, out);
    EXPECT_EQ(1, out[0]);
    EXPECT_EQ(0, out[1]);
}

TEST(GfxDecodeByte, Mode0Pixel1OnlyBit6) {
    // Byte 0x40: bit6=1
    // pixel0 = 0 (bit7=0, bit5=0, bit3=0, bit1=0)
    // pixel1 gets bit6 as LSB → color index 1
    uint8_t out[2];
    gfx_decode_byte(0x40, 0, out);
    EXPECT_EQ(0, out[0]);
    EXPECT_EQ(1, out[1]);
}

TEST(GfxDecodeByte, Mode1DecodesFourPixels) {
    uint8_t out[4];
    EXPECT_EQ(4, gfx_decode_byte(0x00, 1, out));
    for (int i = 0; i < 4; i++) EXPECT_EQ(0, out[i]);
}

TEST(GfxDecodeByte, Mode1AllOnesGives3) {
    uint8_t out[4];
    EXPECT_EQ(4, gfx_decode_byte(0xFF, 1, out));
    for (int i = 0; i < 4; i++) EXPECT_EQ(3, out[i]);
}

TEST(GfxDecodeByte, Mode2DecodesEightPixels) {
    uint8_t out[8];
    EXPECT_EQ(8, gfx_decode_byte(0x00, 2, out));
    for (int i = 0; i < 8; i++) EXPECT_EQ(0, out[i]);
}

TEST(GfxDecodeByte, Mode2AllOnesGives1) {
    uint8_t out[8];
    EXPECT_EQ(8, gfx_decode_byte(0xFF, 2, out));
    for (int i = 0; i < 8; i++) EXPECT_EQ(1, out[i]);
}

TEST(GfxDecodeByte, Mode2AlternatingBits) {
    uint8_t out[8];
    gfx_decode_byte(0xAA, 2, out);  // 10101010
    EXPECT_EQ(1, out[0]);
    EXPECT_EQ(0, out[1]);
    EXPECT_EQ(1, out[2]);
    EXPECT_EQ(0, out[3]);
    EXPECT_EQ(1, out[4]);
    EXPECT_EQ(0, out[5]);
    EXPECT_EQ(1, out[6]);
    EXPECT_EQ(0, out[7]);
}

TEST(GfxDecodeByte, InvalidModeReturnsZero) {
    uint8_t out[8];
    EXPECT_EQ(0, gfx_decode_byte(0xFF, 3, out));
    EXPECT_EQ(0, gfx_decode_byte(0xFF, -1, out));
}

// --- gfx_encode_byte roundtrip tests ---

TEST(GfxEncodeByte, Mode0RoundTrip) {
    for (int byte_val = 0; byte_val < 256; byte_val++) {
        uint8_t decoded[2];
        gfx_decode_byte(static_cast<uint8_t>(byte_val), 0, decoded);
        uint8_t encoded = gfx_encode_byte(decoded, 0);
        EXPECT_EQ(byte_val, encoded) << "Roundtrip failed for byte " << byte_val;
    }
}

TEST(GfxEncodeByte, Mode1RoundTrip) {
    for (int byte_val = 0; byte_val < 256; byte_val++) {
        uint8_t decoded[4];
        gfx_decode_byte(static_cast<uint8_t>(byte_val), 1, decoded);
        uint8_t encoded = gfx_encode_byte(decoded, 1);
        EXPECT_EQ(byte_val, encoded) << "Roundtrip failed for byte " << byte_val;
    }
}

TEST(GfxEncodeByte, Mode2RoundTrip) {
    for (int byte_val = 0; byte_val < 256; byte_val++) {
        uint8_t decoded[8];
        gfx_decode_byte(static_cast<uint8_t>(byte_val), 2, decoded);
        uint8_t encoded = gfx_encode_byte(decoded, 2);
        EXPECT_EQ(byte_val, encoded) << "Roundtrip failed for byte " << byte_val;
    }
}

// --- gfx_decode tests ---

TEST(GfxDecode, Mode0ProducesCorrectDimensions) {
    uint8_t mem[64] = {};
    uint32_t palette[16] = {};
    std::vector<uint32_t> pixels;

    GfxViewParams params = {0, 4, 4, 0};  // 4 bytes wide, 4 rows, mode 0
    int pw = gfx_decode(mem, sizeof(mem), params, palette, pixels);
    EXPECT_EQ(8, pw);  // 4 bytes * 2 pixels/byte = 8 pixels wide
    EXPECT_EQ(32u, pixels.size());  // 8 * 4 = 32
}

TEST(GfxDecode, Mode1ProducesCorrectDimensions) {
    uint8_t mem[64] = {};
    uint32_t palette[16] = {};
    std::vector<uint32_t> pixels;

    GfxViewParams params = {0, 4, 4, 1};
    int pw = gfx_decode(mem, sizeof(mem), params, palette, pixels);
    EXPECT_EQ(16, pw);  // 4 * 4 = 16 pixels wide
    EXPECT_EQ(64u, pixels.size());
}

TEST(GfxDecode, Mode2ProducesCorrectDimensions) {
    uint8_t mem[64] = {};
    uint32_t palette[16] = {};
    std::vector<uint32_t> pixels;

    GfxViewParams params = {0, 4, 4, 2};
    int pw = gfx_decode(mem, sizeof(mem), params, palette, pixels);
    EXPECT_EQ(32, pw);  // 4 * 8 = 32 pixels wide
    EXPECT_EQ(128u, pixels.size());
}

TEST(GfxDecode, InvalidModeReturnsZero) {
    uint8_t mem[16] = {};
    uint32_t palette[16] = {};
    std::vector<uint32_t> pixels;

    GfxViewParams params = {0, 2, 2, 3};
    EXPECT_EQ(0, gfx_decode(mem, sizeof(mem), params, palette, pixels));
}

TEST(GfxDecode, ZeroDimensionsReturnZero) {
    uint8_t mem[16] = {};
    uint32_t palette[16] = {};
    std::vector<uint32_t> pixels;

    GfxViewParams params = {0, 0, 4, 0};
    EXPECT_EQ(0, gfx_decode(mem, sizeof(mem), params, palette, pixels));
}

TEST(GfxDecode, UsesCorrectPaletteColors) {
    uint8_t mem[1] = {0xFF};  // Mode 0: both pixels = color 15
    uint32_t palette[16] = {};
    palette[15] = 0xDEADBEEF;
    std::vector<uint32_t> pixels;

    GfxViewParams params = {0, 1, 1, 0};
    int pw = gfx_decode(mem, sizeof(mem), params, palette, pixels);
    EXPECT_EQ(2, pw);
    EXPECT_EQ(0xDEADBEEF, pixels[0]);
    EXPECT_EQ(0xDEADBEEF, pixels[1]);
}

// --- gfx_paint tests ---

TEST(GfxPaint, PaintsPixelAndVerifies) {
    uint8_t mem[16] = {};
    GfxViewParams params = {0, 4, 4, 0};

    // Paint pixel (0,0) with color 5
    EXPECT_TRUE(gfx_paint(mem, sizeof(mem), params, 0, 0, 5));

    // Verify by decoding
    uint8_t decoded[2];
    gfx_decode_byte(mem[0], 0, decoded);
    EXPECT_EQ(5, decoded[0]);
    EXPECT_EQ(0, decoded[1]);  // other pixel unchanged
}

TEST(GfxPaint, OutOfBoundsReturnsFalse) {
    uint8_t mem[16] = {};
    GfxViewParams params = {0, 4, 4, 0};

    EXPECT_FALSE(gfx_paint(mem, sizeof(mem), params, 100, 0, 1));
    EXPECT_FALSE(gfx_paint(mem, sizeof(mem), params, 0, 100, 1));
    EXPECT_FALSE(gfx_paint(mem, sizeof(mem), params, -1, 0, 1));
}

TEST(GfxPaint, InvalidModeReturnsFalse) {
    uint8_t mem[16] = {};
    GfxViewParams params = {0, 4, 4, 3};
    EXPECT_FALSE(gfx_paint(mem, sizeof(mem), params, 0, 0, 1));
}

// --- gfx_export_bmp tests ---

TEST(GfxExportBmp, CreatesValidFile) {
    auto tmp = std::filesystem::temp_directory_path() / "test_gfx.bmp";
    uint32_t pixels[4] = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF};

    EXPECT_TRUE(gfx_export_bmp(tmp.string(), pixels, 2, 2));

    // Verify file exists and has correct size
    // 14 (file hdr) + 40 (info hdr) + 2*2*4 (pixels) = 70
    auto fsize = std::filesystem::file_size(tmp);
    EXPECT_EQ(70u, fsize);

    // Verify BMP magic
    FILE* f = fopen(tmp.string().c_str(), "rb");
    ASSERT_NE(nullptr, f);
    uint8_t magic[2];
    fread(magic, 1, 2, f);
    EXPECT_EQ('B', magic[0]);
    EXPECT_EQ('M', magic[1]);
    fclose(f);

    std::filesystem::remove(tmp);
}

TEST(GfxExportBmp, NullPixelsReturnsFalse) {
    EXPECT_FALSE(gfx_export_bmp("/dev/null", nullptr, 2, 2));
}

TEST(GfxExportBmp, ZeroDimensionsReturnsFalse) {
    uint32_t pixels[1] = {0};
    EXPECT_FALSE(gfx_export_bmp("/dev/null", pixels, 0, 1));
    EXPECT_FALSE(gfx_export_bmp("/dev/null", pixels, 1, 0));
}

}  // namespace
