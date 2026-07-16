// konCePCja — host filesystem helpers.

#include "fileutils.h"

#ifdef _MSC_VER
#include "compat/msvc_compat.h"
#endif
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <filesystem>

#include "log.h"

long file_size(int fd) {
  struct stat s = {};
  if (fstat(fd, &s) != 0) return 0;
  return static_cast<long>(s.st_size);
}

bool file_copy(std::FILE* in, std::FILE* out) {
  std::array<char, 16384> buffer;
  size_t read;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), in)) > 0) {
    if (std::fwrite(buffer.data(), 1, read, out) != read) break;
  }
  return !(std::ferror(in) || std::ferror(out));
}

bool is_directory(const std::string& filepath) {
  std::error_code ec;
  return std::filesystem::is_directory(filepath, ec);
}

std::vector<std::string> listDirectory(const std::string& directory) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry :
       std::filesystem::directory_iterator(directory, ec)) {
    std::string name = entry.path().filename().string();
    if (name != "." && name != "..") names.push_back(std::move(name));
  }
  if (ec) {
    LOG_VERBOSE("listDirectory(" << directory << ") failed: " << ec.message());
    return names;
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> listDirectoryExt(const std::string& directory,
                                          const std::string& ext) {
  std::vector<std::string> matching;
  for (auto& name : listDirectory(directory)) {
    auto dot = name.find_last_of('.');
    if (name.compare(dot + 1, std::string::npos, ext) == 0) {
      matching.push_back(std::move(name));
    }
  }
  return matching;
}

std::string getDateString() {
  char stamp[32];
  std::time_t t = std::time(nullptr);
  if (std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S",
                    std::localtime(&t))) {
    return stamp;
  }
  return "unknown_date";
}
