#pragma once

// konCePCja — host filesystem helpers.

#include <cstdio>
#include <string>
#include <vector>

// Size in bytes of the open file behind descriptor `fd`, or 0 on error.
long file_size(int fd);

// Streams the remaining content of `in` into `out`.
// Returns true when every byte was transferred without an I/O error.
bool file_copy(std::FILE* in, std::FILE* out);

// True when `filepath` names an existing directory.
bool is_directory(const std::string& filepath);

// Sorted names of the entries in `directory` ("." and ".." excluded).
// Returns an empty vector when the directory cannot be read.
std::vector<std::string> listDirectory(const std::string& directory);

// Like listDirectory, filtered to entries whose extension equals `ext`
// (without the dot).
std::vector<std::string> listDirectoryExt(const std::string& directory,
                                          const std::string& ext);

// Current local date and time as "YYYYMMDD_HHmmss" (used to stamp
// screenshot and snapshot filenames).
std::string getDateString();
