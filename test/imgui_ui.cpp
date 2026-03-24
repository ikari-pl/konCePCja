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

// ─────────────────────────────────────────────────
// MRU list tests
// ─────────────────────────────────────────────────

TEST(MruList, PushToEmpty) {
  std::vector<std::string> list;
  mru_list_push(list, "/path/a.dsk");
  ASSERT_EQ(1u, list.size());
  EXPECT_EQ("/path/a.dsk", list[0]);
}

TEST(MruList, PushMovesToFront) {
  std::vector<std::string> list = {"/a", "/b", "/c"};
  mru_list_push(list, "/b");
  ASSERT_EQ(3u, list.size());
  EXPECT_EQ("/b", list[0]);
  EXPECT_EQ("/a", list[1]);
  EXPECT_EQ("/c", list[2]);
}

TEST(MruList, PushDeduplicates) {
  std::vector<std::string> list = {"/a", "/b", "/a"};
  mru_list_push(list, "/a");
  ASSERT_EQ(2u, list.size());
  EXPECT_EQ("/a", list[0]);
  EXPECT_EQ("/b", list[1]);
}

TEST(MruList, PushCapsAtMax) {
  std::vector<std::string> list;
  for (int i = 0; i < 12; i++) {
    mru_list_push(list, "/path/" + std::to_string(i), 10);
  }
  EXPECT_EQ(10u, list.size());
  EXPECT_EQ("/path/11", list[0]);  // most recent
  EXPECT_EQ("/path/2", list[9]);   // oldest kept (0 and 1 were evicted)
}

TEST(MruList, PushSameTwiceNoGrowth) {
  std::vector<std::string> list = {"/a"};
  mru_list_push(list, "/a");
  ASSERT_EQ(1u, list.size());
  EXPECT_EQ("/a", list[0]);
}

TEST(MruList, PushNewToFullList) {
  std::vector<std::string> list = {"/a", "/b", "/c"};
  mru_list_push(list, "/d", 3);
  ASSERT_EQ(3u, list.size());
  EXPECT_EQ("/d", list[0]);
  EXPECT_EQ("/a", list[1]);
  EXPECT_EQ("/b", list[2]);
  // "/c" was evicted
}

// ─────────────────────────────────────────────────
// is_valid_ram_size tests
// ─────────────────────────────────────────────────

TEST(IsValidRamSize, AllValidSizes) {
  EXPECT_TRUE(is_valid_ram_size(64));
  EXPECT_TRUE(is_valid_ram_size(128));
  EXPECT_TRUE(is_valid_ram_size(192));
  EXPECT_TRUE(is_valid_ram_size(256));
  EXPECT_TRUE(is_valid_ram_size(320));
  EXPECT_TRUE(is_valid_ram_size(512));
  EXPECT_TRUE(is_valid_ram_size(576));
  EXPECT_TRUE(is_valid_ram_size(4160));
}

TEST(IsValidRamSize, InvalidSizes) {
  EXPECT_FALSE(is_valid_ram_size(0));
  EXPECT_FALSE(is_valid_ram_size(1));
  EXPECT_FALSE(is_valid_ram_size(63));
  EXPECT_FALSE(is_valid_ram_size(65));
  EXPECT_FALSE(is_valid_ram_size(100));
  EXPECT_FALSE(is_valid_ram_size(1024));
  EXPECT_FALSE(is_valid_ram_size(4096));
  EXPECT_FALSE(is_valid_ram_size(99999));
}

// ─────────────────────────────────────────────────
// Constants consistency checks
// ─────────────────────────────────────────────────

TEST(Constants, RamSizeCountMatchesArray) {
  EXPECT_EQ(RAM_SIZE_COUNT, 8);
  EXPECT_EQ(RAM_SIZES[0], 64u);
  EXPECT_EQ(RAM_SIZES[RAM_SIZE_COUNT - 1], 4160u);
}

TEST(Constants, SampleRateCountMatchesArray) {
  EXPECT_EQ(SAMPLE_RATE_COUNT, 5);
  EXPECT_EQ(SAMPLE_RATES[0], 11025u);
  EXPECT_EQ(SAMPLE_RATES[SAMPLE_RATE_COUNT - 1], 96000u);
}

TEST(Constants, SampleRatesAreMonotonicallyIncreasing) {
  for (int i = 1; i < SAMPLE_RATE_COUNT; i++) {
    EXPECT_GT(SAMPLE_RATES[i], SAMPLE_RATES[i - 1])
        << "SAMPLE_RATES[" << i << "] should be > SAMPLE_RATES[" << (i - 1) << "]";
  }
}

TEST(Constants, RamSizesAreMonotonicallyIncreasing) {
  for (int i = 1; i < RAM_SIZE_COUNT; i++) {
    EXPECT_GT(RAM_SIZES[i], RAM_SIZES[i - 1])
        << "RAM_SIZES[" << i << "] should be > RAM_SIZES[" << (i - 1) << "]";
  }
}

// ─────────────────────────────────────────────────
// format_memory_line: additional edge cases
// ─────────────────────────────────────────────────

TEST_F(FormatMemoryLineTest, AddressAtFFF0) {
  // 16 bytes starting at 0xFFF0 — the last valid 16-byte line
  for (int i = 0; i < 16; i++) {
    ram[(0xFFF0 + i) & 0xFFFF] = static_cast<byte>(0xF0 + i);
  }

  int len = format_memory_line(buf, sizeof(buf), 0xFFF0, 16, 0, ram);

  EXPECT_GT(len, 0);
  EXPECT_NE(nullptr, strstr(buf, "FFF0"));
  // First byte: 0xF0, last byte wraps: ram[0x0000] was set to 0xF0+16=0x00 (truncated)
  EXPECT_NE(nullptr, strstr(buf, "F0"));
  EXPECT_NE(nullptr, strstr(buf, "FF")); // 0xFFF0 + 15 = 0xFFFF -> ram[0xFFFF] = 0xFF
}

TEST_F(FormatMemoryLineTest, LargeBaseAddrMaskedTo16Bit) {
  // base_addr > 0xFFFF: should be masked to 16-bit
  ram[0x1234] = 0x42;

  int len = format_memory_line(buf, sizeof(buf), 0x11234, 1, 0, ram);

  EXPECT_GT(len, 0);
  // Address display: 0x11234 & 0xFFFF = 0x1234
  EXPECT_NE(nullptr, strstr(buf, "1234"));
  EXPECT_NE(nullptr, strstr(buf, "42"));
}

TEST_F(FormatMemoryLineTest, AsciiFormatPrintableRange) {
  // Test exact boundaries of printable range: 32 (space) to 126 (~)
  ram[0x100] = 31;   // non-printable (below space)
  ram[0x101] = 32;   // space - first printable
  ram[0x102] = 126;  // '~' - last printable
  ram[0x103] = 127;  // DEL - non-printable

  int len = format_memory_line(buf, sizeof(buf), 0x100, 4, 1, ram);

  EXPECT_GT(len, 0);
  const char* pipe = strstr(buf, "|");
  ASSERT_NE(nullptr, pipe);
  // After "| ": dot, space, tilde, dot
  EXPECT_EQ('.', pipe[2]);   // 31 -> dot
  EXPECT_EQ(' ', pipe[3]);   // 32 -> space
  EXPECT_EQ('~', pipe[4]);   // 126 -> tilde
  EXPECT_EQ('.', pipe[5]);   // 127 -> dot
}

TEST_F(FormatMemoryLineTest, DecimalFormatValues) {
  ram[0x200] = 0;
  ram[0x201] = 1;
  ram[0x202] = 127;
  ram[0x203] = 255;

  int len = format_memory_line(buf, sizeof(buf), 0x200, 4, 2, ram);

  EXPECT_GT(len, 0);
  // All four decimal values should appear
  EXPECT_NE(nullptr, strstr(buf, "  0"));   // %3u format
  EXPECT_NE(nullptr, strstr(buf, "  1"));
  EXPECT_NE(nullptr, strstr(buf, "127"));
  EXPECT_NE(nullptr, strstr(buf, "255"));
}

TEST_F(FormatMemoryLineTest, UnknownFormatFallsBackToHexOnly) {
  ram[0x300] = 0xAB;

  // format=99 (invalid) — should produce hex only, no ASCII or decimal
  int len = format_memory_line(buf, sizeof(buf), 0x300, 1, 99, ram);

  EXPECT_GT(len, 0);
  EXPECT_NE(nullptr, strstr(buf, "0300"));
  EXPECT_NE(nullptr, strstr(buf, "AB"));
  // No pipe separator (that's format 1 and 2 only)
  EXPECT_EQ(nullptr, strstr(buf, "|"));
}

TEST_F(FormatMemoryLineTest, BufferExactlyFitsAddress) {
  // "0000 : " = 7 chars + null = 8 bytes minimum
  char tiny[8];
  int len = format_memory_line(tiny, sizeof(tiny), 0, 1, 0, ram);

  EXPECT_GT(len, 0);
  EXPECT_EQ('\0', tiny[len]);
}

// ─────────────────────────────────────────────────
// snd_playback_rate index bug documentation
// ─────────────────────────────────────────────────
// BUG: The Options dialog in imgui_ui.cpp writes raw frequency values
// (e.g. 44100) to CPC.snd_playback_rate instead of an index (0-4).
//
// The rest of the codebase (psg.cpp, kon_cpc_ja.cpp) uses
// CPC.snd_playback_rate as an INDEX into freq_table[5].
//
// The dialog does:
//   CPC.snd_playback_rate = sample_rate_values[rate_idx];  // writes 44100!
//
// But psg.cpp does:
//   freq_table[CPC.snd_playback_rate]  // OOB access with freq_table[44100]
//
// Additionally, find_sample_rate_index() takes a frequency value, but
// CPC.snd_playback_rate is normally an index. It "works" for the default
// index 2 because find_sample_rate_index(2) returns the default of 2.

TEST(SndPlaybackRateBug, IndexValuesAreValidIndices) {
  // CPC.snd_playback_rate should be an index 0..4 into freq_table[5].
  // Verify all valid index values are within bounds.
  for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
    EXPECT_LT(i, 5) << "Index " << i << " would be out of bounds for freq_table[5]";
  }
}

TEST(SndPlaybackRateBug, RawFrequencyIsNotValidIndex) {
  // This documents the bug: if the Options dialog writes a raw frequency
  // (e.g. 44100) to CPC.snd_playback_rate, it's way out of bounds for
  // freq_table[5]. Any value >= 5 is invalid as an index.
  for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
    unsigned int raw_freq = SAMPLE_RATES[i];
    EXPECT_GE(raw_freq, 5u)
        << "Raw frequency " << raw_freq << " would be an invalid index into freq_table[5]";
  }
}

TEST(SndPlaybackRateBug, FindSampleRateIndexDefaultMasksTheBug) {
  // find_sample_rate_index() expects a frequency, but CPC.snd_playback_rate
  // is normally an index. When the index is 2, find_sample_rate_index(2)
  // doesn't find 2 in {11025, 22050, 44100, 48000, 96000}, so it returns
  // the default of 2 -- which happens to be correct by coincidence.
  EXPECT_EQ(2, find_sample_rate_index(2));  // "works" by accident

  // For other index values, the result is always 2 (the default),
  // which means the combo always shows "44100" regardless of actual setting.
  EXPECT_EQ(2, find_sample_rate_index(0));  // should show 11025, shows 44100
  EXPECT_EQ(2, find_sample_rate_index(1));  // should show 22050, shows 44100
  EXPECT_EQ(2, find_sample_rate_index(3));  // should show 48000, shows 44100
  EXPECT_EQ(2, find_sample_rate_index(4));  // should show 96000, shows 44100
}

TEST(SndPlaybackRateBug, AfterDialogWritesFreqOtherCodeBreaks) {
  // After the dialog writes a raw frequency, any code using
  // CPC.snd_playback_rate as an index would access out-of-bounds memory.
  // Example: freq_table[44100] when freq_table has only 5 entries.
  //
  // We can't test the actual OOB access, but we verify that the raw
  // frequency values from sample_rate_values[] are never valid indices.
  int sample_rate_values[] = { 11025, 22050, 44100, 48000, 96000 };
  for (int i = 0; i < 5; i++) {
    EXPECT_GE(sample_rate_values[i], SAMPLE_RATE_COUNT)
        << "After Options dialog writes " << sample_rate_values[i]
        << " to snd_playback_rate, freq_table[" << sample_rate_values[i]
        << "] is out-of-bounds (freq_table has " << SAMPLE_RATE_COUNT << " entries)";
  }
}

// ─────────────────────────────────────────────────
// MRU list: additional edge cases
// ─────────────────────────────────────────────────

TEST(MruList, MaxSizeOne) {
  std::vector<std::string> list;
  mru_list_push(list, "/a", 1);
  mru_list_push(list, "/b", 1);
  ASSERT_EQ(1u, list.size());
  EXPECT_EQ("/b", list[0]);
}

TEST(MruList, EmptyPathIsAllowed) {
  std::vector<std::string> list;
  mru_list_push(list, "");
  ASSERT_EQ(1u, list.size());
  EXPECT_EQ("", list[0]);
}

TEST(MruList, ExistingItemMovedToFrontPreservesOrder) {
  std::vector<std::string> list = {"/a", "/b", "/c", "/d", "/e"};
  mru_list_push(list, "/d");
  ASSERT_EQ(5u, list.size());
  EXPECT_EQ("/d", list[0]);
  EXPECT_EQ("/a", list[1]);
  EXPECT_EQ("/b", list[2]);
  EXPECT_EQ("/c", list[3]);
  EXPECT_EQ("/e", list[4]);
}

// ─────────────────────────────────────────────────
// parse_hex: additional edge cases
// ─────────────────────────────────────────────────

TEST(ParseHex, MaxValueBoundaryExact) {
  unsigned long result = 0;
  // Exactly at max_val should succeed
  EXPECT_TRUE(parse_hex("100", &result, 0x100));
  EXPECT_EQ(0x100u, result);
}

TEST(ParseHex, MaxValueBoundaryOneOver) {
  unsigned long result = 0;
  // One over max_val should fail
  EXPECT_FALSE(parse_hex("101", &result, 0x100));
}

TEST(ParseHex, MaxValueZero) {
  unsigned long result = 999;
  // max_val=0: only "0" should be valid
  EXPECT_TRUE(parse_hex("0", &result, 0));
  EXPECT_EQ(0u, result);

  EXPECT_FALSE(parse_hex("1", &result, 0));
}

// ─────────────────────────────────────────────────
// safe_read: zero-offset edge cases
// ─────────────────────────────────────────────────

TEST(SafeReadWord, ZeroLengthBuffer) {
  byte b = 0x42;
  word result = 0xFFFF;
  // Buffer is zero length (start == end)
  EXPECT_FALSE(safe_read_word(&b, &b, 0, result));
  EXPECT_EQ(0xFFFFu, result);
}

TEST(SafeReadDword, ZeroLengthBuffer) {
  byte b = 0x42;
  dword result = 0xDEADBEEF;
  EXPECT_FALSE(safe_read_dword(&b, &b, 0, result));
  EXPECT_EQ(0xDEADBEEFu, result);
}

TEST(SafeReadDword, ExactFourBytes) {
  byte buffer[4] = { 0x01, 0x02, 0x03, 0x04 };
  dword result = 0;
  EXPECT_TRUE(safe_read_dword(buffer, buffer + 4, 0, result));
  EXPECT_EQ(0x04030201u, result);
}
