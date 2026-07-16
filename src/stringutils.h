#pragma once

// konCePCja — small string helpers shared by the host shell.

#include <string>
#include <string_view>
#include <vector>

namespace stringutils {

// Splits `s` on `delim`. With ignore_empty, empty fields are dropped.
// An empty input yields an empty vector.
std::vector<std::string> split(std::string_view s, char delim,
                               bool ignore_empty = false);

// Joins `v` with `delim` between elements.
std::string join(const std::vector<std::string>& v, std::string_view delim);

// Strips any leading and trailing occurrences of `c`.
std::string trim(std::string_view s, char c);

std::string lower(std::string_view s);
std::string upper(std::string_view s);

// Replaces the FIRST occurrence of `search` only (an empty `search`
// matches at position 0, i.e. prepends `replacement`).
std::string replace(std::string s, std::string_view search,
                    std::string_view replacement);

// Splits `path` into directory (with trailing separator) and filename.
// Prefers '/' over '\\'; a path with no separator yields dirname "./".
void splitPath(std::string_view path, std::string& dirname,
               std::string& filename);

// Case-insensitive "str1 < str2" (strict weak ordering for sorting).
bool caseInsensitiveCompare(std::string_view str1, std::string_view str2);

}  // namespace stringutils
