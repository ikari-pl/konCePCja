// konCePCja — small string helpers shared by the host shell.

#include "stringutils.h"

#include <algorithm>
#include <cctype>

namespace stringutils {
namespace {

inline char to_lower_ascii(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline char to_upper_ascii(char c) {
  return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

}  // namespace

std::vector<std::string> split(std::string_view s, char delim,
                               bool ignore_empty) {
  std::vector<std::string> elems;
  size_t begin = 0;
  while (begin <= s.size()) {
    size_t end = s.find(delim, begin);
    if (end == std::string_view::npos) end = s.size();
    // Mirror std::getline framing: a trailing delimiter does not produce
    // a final empty field, and an empty input produces no fields at all.
    if (end == s.size() && begin == s.size()) break;
    if (!ignore_empty || end != begin) {
      elems.emplace_back(s.substr(begin, end - begin));
    }
    begin = end + 1;
  }
  return elems;
}

std::string join(const std::vector<std::string>& v, std::string_view delim) {
  std::string result;
  size_t total = v.empty() ? 0 : delim.size() * (v.size() - 1);
  for (const auto& elem : v) total += elem.size();
  result.reserve(total);
  bool first = true;
  for (const auto& elem : v) {
    if (!first) result += delim;
    result += elem;
    first = false;
  }
  return result;
}

std::string trim(std::string_view s, char c) {
  size_t const first = s.find_first_not_of(c);
  if (first == std::string_view::npos) return "";
  size_t const last = s.find_last_not_of(c);
  return std::string(s.substr(first, last - first + 1));
}

std::string lower(std::string_view s) {
  std::string result(s);
  std::transform(result.begin(), result.end(), result.begin(), to_lower_ascii);
  return result;
}

std::string upper(std::string_view s) {
  std::string result(s);
  std::transform(result.begin(), result.end(), result.begin(), to_upper_ascii);
  return result;
}

std::string replace(std::string s, std::string_view search,
                    std::string_view replacement) {
  auto start_pos = s.find(search);
  if (start_pos == std::string::npos) return s;
  return s.replace(start_pos, search.size(), replacement);
}

void splitPath(std::string_view path, std::string& dirname,
               std::string& filename) {
  auto delimiter = path.rfind('/');
  if (delimiter == std::string_view::npos) {
    delimiter = path.rfind('\\');
  }
  if (delimiter != std::string_view::npos) {
    ++delimiter;
    dirname = std::string(path.substr(0, delimiter));
    filename = std::string(path.substr(delimiter));
  } else {
    dirname = "./";
    filename = std::string(path);
  }
}

bool caseInsensitiveCompare(std::string_view str1, std::string_view str2) {
  return std::lexicographical_compare(
      str1.begin(), str1.end(), str2.begin(), str2.end(),
      [](char a, char b) { return to_lower_ascii(a) < to_lower_ascii(b); });
}

}  // namespace stringutils
