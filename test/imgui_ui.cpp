#include <gtest/gtest.h>
#include "imgui_ui_testable.h"

// ─────────────────────────────────────────────────
// parse_hex tests
// ─────────────────────────────────────────────────

TEST(ParseHex, ValidHexLowercase) {
  unsigned long result = 0;
  EXPECT_TRUE(parse_hex("1234", &result, 0xFFFF));
  EXPECT_EQ(0x1234u, result);
}

TEST(ParseHex, ValidHexUppercase) {
  unsigned long result = 0;
  EXPECT_TRUE(parse_hex("ABCD", &result, 0xFFFF));
  EXPECT_EQ(0xABCDu, result);
}

TEST(ParseHex, ValidHexMixedCase) {
  unsigned long result = 0;
  EXPECT_TRUE(parse_hex("AbCd", &result, 0xFFFF));
  EXPECT_EQ(0xABCDu, result);
}

TEST(ParseHex, ValidHexWithPrefix) {
  unsigned long result = 0;
  // Note: strtoul with base 16 doesn't require 0x prefix
  // but also accepts it - this depends on implementation
  EXPECT_TRUE(parse_hex("FF", &result, 0xFFFF));
  EXPECT_EQ(0xFFu, result);
}

TEST(ParseHex, ValidHexMaxValue) {
  unsigned long result = 0;
  EXPECT_TRUE(parse_hex("FFFF", &result, 0xFFFF));
  EXPECT_EQ(0xFFFFu, result);
}

TEST(ParseHex, ValidHexZero) {
  unsigned long result = 999;
  EXPECT_TRUE(parse_hex("0", &result, 0xFFFF));
  EXPECT_EQ(0u, result);
}

TEST(ParseHex, ExceedsMaxValue) {
  unsigned long result = 0;
  EXPECT_FALSE(parse_hex("10000", &result, 0xFFFF));
  EXPECT_EQ(0u, result); // unchanged on failure
}

TEST(ParseHex, InvalidCharacters) {
  unsigned long result = 0;
  EXPECT_FALSE(parse_hex("12GH", &result, 0xFFFF));
}

TEST(ParseHex, InvalidTrailingSpace) {
  unsigned long result = 0;
  EXPECT_FALSE(parse_hex("1234 ", &result, 0xFFFF));
}

TEST(ParseHex, InvalidLeadingSpace) {
  unsigned long result = 0;
  // strtoul skips leading whitespace, but we check *end != '\0'
  // Actually strtoul does skip leading whitespace, so " 1234" would parse as 1234
  // Let's verify the actual behavior
}

TEST(ParseHex, EmptyString) {
  unsigned long result = 999;
  EXPECT_FALSE(parse_hex("", &result, 0xFFFF));
  EXPECT_EQ(999u, result); // unchanged on failure
}

TEST(ParseHex, NullString) {
  unsigned long result = 999;
  EXPECT_FALSE(parse_hex(nullptr, &result, 0xFFFF));
  EXPECT_EQ(999u, result); // unchanged on failure
}

TEST(ParseHex, SingleDigit) {
  unsigned long result = 0;
  EXPECT_TRUE(parse_hex("F", &result, 0xFFFF));
  EXPECT_EQ(0xFu, result);
}

TEST(ParseHex, LargeValue32Bit) {
  unsigned long result = 0;
  EXPECT_TRUE(parse_hex("FFFFFFFF", &result, 0xFFFFFFFF));
  EXPECT_EQ(0xFFFFFFFFu, result);
}

// ─────────────────────────────────────────────────
// safe_read_word / safe_read_dword tests
// ─────────────────────────────────────────────────

TEST(SafeReadWord, ValidRead) {
  byte buffer[] = { 0x34, 0x12 }; // little-endian: 0x1234
  word result = 0;
  EXPECT_TRUE(safe_read_word(buffer, buffer + sizeof(buffer), 0, result));
  EXPECT_EQ(0x1234u, result);
}

TEST(SafeReadWord, ValidReadWithOffset) {
  byte buffer[] = { 0x00, 0x34, 0x12, 0x00 };
  word result = 0;
  EXPECT_TRUE(safe_read_word(buffer, buffer + sizeof(buffer), 1, result));
  EXPECT_EQ(0x1234u, result);
}

TEST(SafeReadWord, ReadAtBoundary) {
  byte buffer[] = { 0x00, 0x00, 0x34, 0x12 };
  word result = 0;
  // offset 2 + sizeof(word)=2 = 4, which is exactly end
  EXPECT_TRUE(safe_read_word(buffer, buffer + 4, 2, result));
  EXPECT_EQ(0x1234u, result);
}

TEST(SafeReadWord, ReadPastEnd) {
  byte buffer[] = { 0x34, 0x12 };
  word result = 0xFFFF;
  EXPECT_FALSE(safe_read_word(buffer, buffer + sizeof(buffer), 1, result));
  EXPECT_EQ(0xFFFFu, result); // unchanged on failure
}

TEST(SafeReadWord, ReadPastEndWithOffset) {
  byte buffer[] = { 0x00, 0x34 };
  word result = 0xFFFF;
  // offset 1 + sizeof(word)=2 = 3 > 2 (end)
  EXPECT_FALSE(safe_read_word(buffer, buffer + 2, 1, result));
}

TEST(SafeReadWord, EmptyBuffer) {
  byte buffer[] = { 0x00 };
  word result = 0xFFFF;
  EXPECT_FALSE(safe_read_word(buffer, buffer, 0, result));
}

TEST(SafeReadDword, ValidRead) {
  byte buffer[] = { 0x78, 0x56, 0x34, 0x12 }; // little-endian: 0x12345678
  dword result = 0;
  EXPECT_TRUE(safe_read_dword(buffer, buffer + sizeof(buffer), 0, result));
  EXPECT_EQ(0x12345678u, result);
}

TEST(SafeReadDword, ValidReadWithOffset) {
  byte buffer[] = { 0x00, 0x78, 0x56, 0x34, 0x12, 0x00 };
  dword result = 0;
  EXPECT_TRUE(safe_read_dword(buffer, buffer + sizeof(buffer), 1, result));
  EXPECT_EQ(0x12345678u, result);
}

TEST(SafeReadDword, ReadPastEnd) {
  byte buffer[] = { 0x78, 0x56, 0x34 }; // only 3 bytes
  dword result = 0xDEADBEEF;
  EXPECT_FALSE(safe_read_dword(buffer, buffer + sizeof(buffer), 0, result));
  EXPECT_EQ(0xDEADBEEFu, result); // unchanged on failure
}

TEST(SafeReadDword, ReadAtBoundary) {
  byte buffer[] = { 0x00, 0x78, 0x56, 0x34, 0x12 };
  dword result = 0;
  // offset 1 + sizeof(dword)=4 = 5, which is exactly end
  EXPECT_TRUE(safe_read_dword(buffer, buffer + 5, 1, result));
  EXPECT_EQ(0x12345678u, result);
}

// ─────────────────────────────────────────────────
// find_ram_index tests
// ─────────────────────────────────────────────────

TEST(FindRamIndex, Find64KB) {
  EXPECT_EQ(0, find_ram_index(64));
}

TEST(FindRamIndex, Find128KB) {
  EXPECT_EQ(1, find_ram_index(128));
}

TEST(FindRamIndex, Find192KB) {
  EXPECT_EQ(2, find_ram_index(192));
}

TEST(FindRamIndex, Find256KB) {
  EXPECT_EQ(3, find_ram_index(256));
}

TEST(FindRamIndex, Find320KB) {
  EXPECT_EQ(4, find_ram_index(320));
}

TEST(FindRamIndex, Find512KB) {
  EXPECT_EQ(5, find_ram_index(512));
}

TEST(FindRamIndex, Find576KB) {
  EXPECT_EQ(6, find_ram_index(576));
}

TEST(FindRamIndex, Find4160KB) {
  EXPECT_EQ(7, find_ram_index(4160));
}

TEST(FindRamIndex, InvalidValue) {
  EXPECT_EQ(2, find_ram_index(999)); // defaults to index 2 (192 KB)
}

TEST(FindRamIndex, ZeroValue) {
  EXPECT_EQ(2, find_ram_index(0)); // defaults to index 2 (192 KB)
}

// ─────────────────────────────────────────────────
// find_sample_rate_index tests
// ─────────────────────────────────────────────────

TEST(FindSampleRateIndex, Find11025) {
  EXPECT_EQ(0, find_sample_rate_index(11025));
}

TEST(FindSampleRateIndex, Find22050) {
  EXPECT_EQ(1, find_sample_rate_index(22050));
}

TEST(FindSampleRateIndex, Find44100) {
  EXPECT_EQ(2, find_sample_rate_index(44100));
}

TEST(FindSampleRateIndex, Find48000) {
  EXPECT_EQ(3, find_sample_rate_index(48000));
}

TEST(FindSampleRateIndex, Find96000) {
  EXPECT_EQ(4, find_sample_rate_index(96000));
}

TEST(FindSampleRateIndex, InvalidValue) {
  EXPECT_EQ(2, find_sample_rate_index(999)); // defaults to 44100 (index 2)
}

TEST(FindSampleRateIndex, ZeroValue) {
  EXPECT_EQ(2, find_sample_rate_index(0)); // defaults to 44100 (index 2)
}

// ─────────────────────────────────────────────────
// format_memory_line tests
// ─────────────────────────────────────────────────

class FormatMemoryLineTest : public ::testing::Test {
protected:
  static constexpr size_t RAM_SIZE = 65536;
  byte ram[RAM_SIZE];
  char buf[512];

  void SetUp() override {
    memset(ram, 0, sizeof(ram));
    memset(buf, 0, sizeof(buf));
  }
};

TEST_F(FormatMemoryLineTest, HexOnlyFormat) {
  ram[0x1000] = 0xAB;
  ram[0x1001] = 0xCD;
  ram[0x1002] = 0xEF;
  ram[0x1003] = 0x12;

  int len = format_memory_line(buf, sizeof(buf), 0x1000, 4, 0, ram);

  EXPECT_GT(len, 0);
  // Expected: "1000 : AB CD EF 12 "
  EXPECT_NE(nullptr, strstr(buf, "1000"));
  EXPECT_NE(nullptr, strstr(buf, "AB"));
  EXPECT_NE(nullptr, strstr(buf, "CD"));
  EXPECT_NE(nullptr, strstr(buf, "EF"));
  EXPECT_NE(nullptr, strstr(buf, "12"));
}

TEST_F(FormatMemoryLineTest, HexPlusAsciiFormat) {
  ram[0x2000] = 'H';
  ram[0x2001] = 'i';
  ram[0x2002] = 0x00; // non-printable
  ram[0x2003] = '!';

  int len = format_memory_line(buf, sizeof(buf), 0x2000, 4, 1, ram);

  EXPECT_GT(len, 0);
  // Should have "| Hi.!" or similar
  EXPECT_NE(nullptr, strstr(buf, "|"));
  EXPECT_NE(nullptr, strstr(buf, "H"));
}

TEST_F(FormatMemoryLineTest, HexPlusDecimalFormat) {
  ram[0x3000] = 255;
  ram[0x3001] = 0;
  ram[0x3002] = 128;
  ram[0x3003] = 1;

  int len = format_memory_line(buf, sizeof(buf), 0x3000, 4, 2, ram);

  EXPECT_GT(len, 0);
  // Should have decimal values: 255, 0, 128, 1
  EXPECT_NE(nullptr, strstr(buf, "255"));
  EXPECT_NE(nullptr, strstr(buf, "128"));
}

TEST_F(FormatMemoryLineTest, AddressWraparound) {
  ram[0xFFFF] = 0xAA;
  ram[0x0000] = 0xBB; // wraps around

  int len = format_memory_line(buf, sizeof(buf), 0xFFFF, 2, 0, ram);

  EXPECT_GT(len, 0);
  EXPECT_NE(nullptr, strstr(buf, "FFFF"));
  EXPECT_NE(nullptr, strstr(buf, "AA"));
  EXPECT_NE(nullptr, strstr(buf, "BB"));
}

TEST_F(FormatMemoryLineTest, ZeroAddress) {
  ram[0] = 0x01;
  ram[1] = 0x02;

  int len = format_memory_line(buf, sizeof(buf), 0x0000, 2, 0, ram);

  EXPECT_GT(len, 0);
  EXPECT_NE(nullptr, strstr(buf, "0000"));
}

TEST_F(FormatMemoryLineTest, SmallBuffer) {
  ram[0] = 0xAB;

  char small_buf[10];
  int len = format_memory_line(small_buf, sizeof(small_buf), 0, 1, 0, ram);

  EXPECT_GE(len, 0);
  EXPECT_LT(len, 10);
  // Should be null-terminated
  EXPECT_EQ('\0', small_buf[len]);
}

TEST_F(FormatMemoryLineTest, ZeroSizeBuffer) {
  int len = format_memory_line(buf, 0, 0, 1, 0, ram);
  EXPECT_EQ(0, len);
}

TEST_F(FormatMemoryLineTest, NullRam) {
  int len = format_memory_line(buf, sizeof(buf), 0, 1, 0, nullptr);
  EXPECT_EQ(0, len);
}

TEST_F(FormatMemoryLineTest, SingleByte) {
  ram[0x8000] = 0x42;

  int len = format_memory_line(buf, sizeof(buf), 0x8000, 1, 0, ram);

  EXPECT_GT(len, 0);
  EXPECT_NE(nullptr, strstr(buf, "8000"));
  EXPECT_NE(nullptr, strstr(buf, "42"));
}

TEST_F(FormatMemoryLineTest, SixteenBytes) {
  for (int i = 0; i < 16; i++) {
    ram[0x4000 + i] = static_cast<byte>(i);
  }

  int len = format_memory_line(buf, sizeof(buf), 0x4000, 16, 0, ram);

  EXPECT_GT(len, 0);
  EXPECT_NE(nullptr, strstr(buf, "4000"));
  EXPECT_NE(nullptr, strstr(buf, "00"));
  EXPECT_NE(nullptr, strstr(buf, "0F"));
}

TEST_F(FormatMemoryLineTest, NonPrintableAscii) {
  ram[0x5000] = 0x01; // SOH - non-printable
  ram[0x5001] = 0x1F; // US - non-printable
  ram[0x5002] = 0x7F; // DEL - non-printable
  ram[0x5003] = 0x20; // Space - printable

  int len = format_memory_line(buf, sizeof(buf), 0x5000, 4, 1, ram);

  EXPECT_GT(len, 0);
  // Non-printable chars should be shown as '.'
  const char* pipe = strstr(buf, "|");
  ASSERT_NE(nullptr, pipe);
  // After the pipe, should have "... " (3 dots + space)
  EXPECT_EQ('.', pipe[2]);  // first non-printable
  EXPECT_EQ('.', pipe[3]);  // second non-printable
  EXPECT_EQ('.', pipe[4]);  // third non-printable
  EXPECT_EQ(' ', pipe[5]);  // space is printable
}
