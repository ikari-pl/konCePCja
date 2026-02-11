#include <gtest/gtest.h>
#include "search_engine.h"
#include <cstring>
#include <vector>

namespace {

// Helper: build a memory buffer from initializer list
std::vector<uint8_t> mem(std::initializer_list<uint8_t> bytes) {
  return std::vector<uint8_t>(bytes);
}

// --- Hex search tests ---

TEST(SearchEngineHex, ExactMatchFindsKnownPattern) {
  auto m = mem({0x00, 0xCD, 0x38, 0x00, 0x00});
  auto results = search_memory(m.data(), m.size(), "CD 38", SearchMode::HEX);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].address, 1);
  EXPECT_EQ(results[0].matched_bytes.size(), 2u);
  EXPECT_EQ(results[0].matched_bytes[0], 0xCD);
  EXPECT_EQ(results[0].matched_bytes[1], 0x38);
}

TEST(SearchEngineHex, WildcardQuestionMarkMatchesAnyByte) {
  auto m = mem({0xCD, 0x10, 0x38, 0xCD, 0x20, 0x38, 0xCD, 0x30, 0x39});
  auto results = search_memory(m.data(), m.size(), "CD ?? 38", SearchMode::HEX);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].address, 0);
  EXPECT_EQ(results[1].address, 3);
}

TEST(SearchEngineHex, WildcardStarMatchesVariableLength) {
  auto m = mem({0x21, 0xAA, 0xBB, 0xCC, 0x00, 0x21, 0xDD, 0x00});
  auto results = search_memory(m.data(), m.size(), "21 * 00", SearchMode::HEX);
  ASSERT_GE(results.size(), 1u);
  EXPECT_EQ(results[0].address, 0);
}

TEST(SearchEngineHex, NoMatchReturnsEmpty) {
  auto m = mem({0x00, 0x01, 0x02, 0x03});
  auto results = search_memory(m.data(), m.size(), "FF EE", SearchMode::HEX);
  EXPECT_TRUE(results.empty());
}

TEST(SearchEngineHex, MultipleMatchesFound) {
  auto m = mem({0xAA, 0xBB, 0xAA, 0xBB, 0xAA, 0xBB});
  auto results = search_memory(m.data(), m.size(), "AA BB", SearchMode::HEX);
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].address, 0);
  EXPECT_EQ(results[1].address, 2);
  EXPECT_EQ(results[2].address, 4);
}

// --- Text search tests ---

TEST(SearchEngineText, CaseInsensitiveMatch) {
  std::string text = "Hello World";
  std::vector<uint8_t> m(text.begin(), text.end());
  auto results = search_memory(m.data(), m.size(), "hello", SearchMode::TEXT);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].address, 0);
}

TEST(SearchEngineText, QuestionMarkMatchesSingleChar) {
  std::string text = "HELLO WORLD";
  std::vector<uint8_t> m(text.begin(), text.end());
  auto results = search_memory(m.data(), m.size(), "HEL?O", SearchMode::TEXT);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].address, 0);
}

// --- ASM search tests (stub - ASM mode handled by IPC server) ---

TEST(SearchEngineAsm, AsmModeReturnsEmptyFromGenericSearch) {
  // ASM mode requires z80 disassembly; the generic search_memory returns empty
  auto m = mem({0x3E, 0x00, 0xC9});
  auto results = search_memory(m.data(), m.size(), "ld a,#00", SearchMode::ASM);
  EXPECT_TRUE(results.empty()); // ASM is handled by IPC server directly
}

// --- Edge cases ---

TEST(SearchEngineEdge, EmptyPatternReturnsEmpty) {
  auto m = mem({0x00, 0x01});
  auto results = search_memory(m.data(), m.size(), "", SearchMode::HEX);
  EXPECT_TRUE(results.empty());
}

TEST(SearchEngineEdge, PatternLongerThanMemory) {
  auto m = mem({0xAA});
  auto results = search_memory(m.data(), m.size(), "AA BB CC DD EE", SearchMode::HEX);
  EXPECT_TRUE(results.empty());
}

TEST(SearchEngineEdge, ResultLimitRespected) {
  // Fill with 0xAA pattern
  std::vector<uint8_t> m(1000, 0xAA);
  auto results = search_memory(m.data(), m.size(), "AA", SearchMode::HEX, 10);
  EXPECT_EQ(results.size(), 10u);
}

// --- Fuzzy score tests ---

TEST(FuzzyScore, ExactPrefixScoresHighest) {
  int score_exact = search_detail::fuzzy_score("pause", "Pause");
  int score_sub = search_detail::fuzzy_score("pause", "Toggle Pause Mode");
  EXPECT_GT(score_exact, 0);
  EXPECT_GT(score_sub, 0);
  EXPECT_GT(score_exact, score_sub);
}

TEST(FuzzyScore, SubstringMatchScoresLower) {
  int score = search_detail::fuzzy_score("dev", "DevTools");
  EXPECT_GT(score, 0);
}

TEST(FuzzyScore, NoMatchReturnsZero) {
  int score = search_detail::fuzzy_score("xyz", "Pause");
  EXPECT_EQ(score, 0);
}

} // namespace
