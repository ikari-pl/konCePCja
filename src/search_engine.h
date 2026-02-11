#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class SearchMode { HEX, TEXT, ASM };

struct SearchResult {
  uint16_t address;
  std::vector<uint8_t> matched_bytes;
  std::string context; // disassembled instruction for ASM mode, hex dump otherwise
};

// Search memory for a pattern with wildcard support.
std::vector<SearchResult> search_memory(const uint8_t* mem, size_t mem_size,
                                        const std::string& pattern, SearchMode mode,
                                        size_t max_results = 256);

// Internal helpers exposed for testing
namespace search_detail {

struct PatternElement {
  enum Kind { LITERAL, ANY_ONE, ANY_MANY };
  Kind kind;
  uint8_t value; // only meaningful when kind == LITERAL
};

std::vector<PatternElement> compile_hex_pattern(const std::string& pattern);
std::vector<PatternElement> compile_text_pattern(const std::string& pattern,
                                                 bool case_insensitive = true);
bool match_pattern(const std::vector<PatternElement>& compiled,
                   const uint8_t* mem, size_t mem_size, size_t offset,
                   size_t& match_len, bool case_insensitive = false);
int fuzzy_score(const std::string& query, const std::string& text);

} // namespace search_detail
