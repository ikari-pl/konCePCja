#include "search_engine.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace search_detail {

std::vector<PatternElement> compile_hex_pattern(const std::string& pattern) {
  std::vector<PatternElement> result;
  std::istringstream iss(pattern);
  std::string token;
  while (iss >> token) {
    if (token == "*") {
      result.push_back({PatternElement::ANY_MANY, 0});
    } else if (token == "?" || token == "??") {
      result.push_back({PatternElement::ANY_ONE, 0});
    } else {
      // Parse as hex byte — support multi-byte tokens like "CD38"
      for (size_t i = 0; i + 1 < token.size(); i += 2) {
        std::string pair = token.substr(i, 2);
        if (pair == "??" || pair == "?") {
          result.push_back({PatternElement::ANY_ONE, 0});
        } else {
          unsigned int val = 0;
          try {
            val = std::stoul(pair, nullptr, 16);
          } catch (const std::logic_error&) {
            // Skip invalid hex
            continue;
          }
          result.push_back({PatternElement::LITERAL, static_cast<uint8_t>(val)});
        }
      }
      // Handle odd-length token (single trailing char)
      if (token.size() % 2 == 1 && token.size() > 1) {
        // Ignore trailing nibble
      }
    }
  }
  return result;
}

std::vector<PatternElement> compile_text_pattern(const std::string& pattern,
                                                 bool case_insensitive) {
  std::vector<PatternElement> result;
  for (char c : pattern) {
    if (c == '?') {
      result.push_back({PatternElement::ANY_ONE, 0});
    } else if (c == '*') {
      result.push_back({PatternElement::ANY_MANY, 0});
    } else {
      if (case_insensitive) {
        // Store uppercase version; matching will compare case-insensitively
        result.push_back({PatternElement::LITERAL,
                          static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(c)))});
      } else {
        result.push_back({PatternElement::LITERAL, static_cast<uint8_t>(c)});
      }
    }
  }
  return result;
}

// Recursive pattern matcher with backtracking for ANY_MANY.
// pat_idx = current position in compiled pattern
// mem_pos = current position in memory
static bool match_recursive(const std::vector<PatternElement>& compiled,
                            size_t pat_idx,
                            const uint8_t* mem, size_t mem_size,
                            size_t mem_pos, size_t start,
                            size_t& match_end,
                            bool case_insensitive) {
  while (pat_idx < compiled.size()) {
    const auto& elem = compiled[pat_idx];
    if (elem.kind == PatternElement::LITERAL) {
      if (mem_pos >= mem_size) return false;
      uint8_t mem_val = mem[mem_pos];
      uint8_t pat_val = elem.value;
      if (case_insensitive) {
        mem_val = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(mem_val)));
      }
      if (mem_val != pat_val) return false;
      mem_pos++;
      pat_idx++;
    } else if (elem.kind == PatternElement::ANY_ONE) {
      if (mem_pos >= mem_size) return false;
      mem_pos++;
      pat_idx++;
    } else { // ANY_MANY
      pat_idx++;
      // If * is the last element, match everything remaining
      if (pat_idx >= compiled.size()) {
        // Limit ANY_MANY to at most 256 bytes for practical purposes
        match_end = std::min(mem_size, mem_pos + 256);
        return true;
      }
      // Try matching 0, 1, 2, ... bytes for the wildcard
      size_t max_skip = std::min(mem_size - mem_pos, static_cast<size_t>(256));
      for (size_t skip = 0; skip <= max_skip; skip++) {
        size_t try_end = 0;
        if (match_recursive(compiled, pat_idx, mem, mem_size,
                            mem_pos + skip, start, try_end,
                            case_insensitive)) {
          match_end = try_end;
          return true;
        }
      }
      return false;
    }
  }
  match_end = mem_pos;
  return true;
}

bool match_pattern(const std::vector<PatternElement>& compiled,
                   const uint8_t* mem, size_t mem_size, size_t offset,
                   size_t& match_len, bool case_insensitive) {
  if (compiled.empty()) {
    match_len = 0;
    return true;
  }

  size_t match_end = 0;
  if (match_recursive(compiled, 0, mem, mem_size, offset, offset,
                       match_end, case_insensitive)) {
    match_len = match_end - offset;
    return true;
  }
  return false;
}

int fuzzy_score(const std::string& query, const std::string& text) {
  if (query.empty()) return 1; // Empty query matches everything with minimal score

  // Convert both to lowercase for case-insensitive matching
  std::string lq, lt;
  lq.reserve(query.size());
  lt.reserve(text.size());
  for (char c : query)
    lq.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  for (char c : text)
    lt.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  // Check if all query characters appear in order in text
  size_t qi = 0;
  int score = 0;
  bool prev_matched = false;
  size_t first_match = std::string::npos;

  for (size_t ti = 0; ti < lt.size() && qi < lq.size(); ti++) {
    if (lt[ti] == lq[qi]) {
      if (first_match == std::string::npos) first_match = ti;
      score += 10; // Base score per matched character
      if (prev_matched) score += 5; // Consecutive match bonus
      if (ti == 0 || lt[ti - 1] == ' ' || lt[ti - 1] == '_' || lt[ti - 1] == '-') {
        score += 10; // Word boundary bonus
      }
      prev_matched = true;
      qi++;
    } else {
      prev_matched = false;
    }
  }

  // All query characters must be found
  if (qi < lq.size()) return 0;

  // Bonus for match starting at beginning
  if (first_match == 0) score += 20;

  // Bonus for exact prefix match
  if (lt.substr(0, lq.size()) == lq) score += 30;

  // Bonus for exact match
  if (lt == lq) score += 50;

  return score;
}

} // namespace search_detail

// Build hex context string for a match
static std::string hex_context(const uint8_t* mem, size_t offset, size_t len) {
  std::ostringstream oss;
  for (size_t i = 0; i < len && i < 16; i++) {
    if (i > 0) oss << ' ';
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
        << static_cast<unsigned>(mem[offset + i]);
  }
  return oss.str();
}

std::vector<SearchResult> search_memory(const uint8_t* mem, size_t mem_size,
                                        const std::string& pattern, SearchMode mode,
                                        size_t max_results) {
  std::vector<SearchResult> results;
  if (!mem || mem_size == 0 || pattern.empty()) return results;

  if (mode == SearchMode::HEX) {
    auto compiled = search_detail::compile_hex_pattern(pattern);
    if (compiled.empty()) return results;

    for (size_t addr = 0; addr < mem_size && results.size() < max_results; addr++) {
      size_t match_len = 0;
      if (search_detail::match_pattern(compiled, mem, mem_size, addr, match_len)) {
        SearchResult r;
        r.address = static_cast<uint16_t>(addr);
        r.matched_bytes.assign(mem + addr, mem + addr + match_len);
        r.context = hex_context(mem, addr, match_len);
        results.push_back(std::move(r));
      }
    }
  } else if (mode == SearchMode::TEXT) {
    auto compiled = search_detail::compile_text_pattern(pattern, true);
    if (compiled.empty()) return results;

    for (size_t addr = 0; addr < mem_size && results.size() < max_results; addr++) {
      size_t match_len = 0;
      if (search_detail::match_pattern(compiled, mem, mem_size, addr, match_len, true)) {
        SearchResult r;
        r.address = static_cast<uint16_t>(addr);
        r.matched_bytes.assign(mem + addr, mem + addr + match_len);
        // Text context: show matched text
        std::string ctx;
        for (size_t i = 0; i < match_len && i < 32; i++) {
          char c = static_cast<char>(mem[addr + i]);
          ctx.push_back((c >= 32 && c < 127) ? c : '.');
        }
        r.context = ctx;
        results.push_back(std::move(r));
      }
    }
  } else if (mode == SearchMode::ASM) {
    // ASM mode: pattern matches against disassembled instruction text.
    // This requires the z80 disassembly infrastructure which is only available
    // when linked with the full emulator. The search_memory function for ASM
    // mode is a stub here; the IPC server handles ASM search directly using
    // disassemble_one(). This keeps search_engine.cpp free of z80 dependencies.
    //
    // For unit testing, ASM search uses a simple instruction-text scan:
    // The caller provides pre-disassembled text via the pattern, and we do
    // wildcard matching on it. But for the actual IPC integration, the server
    // calls its own ASM search loop.
    (void)max_results;
  }

  return results;
}
