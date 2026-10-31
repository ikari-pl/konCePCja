#include "shm_region.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include <utility>

namespace shm {

std::string os_name(const std::string& base) {
#ifdef _WIN32
  return "Local\\" + base;
#else
  return "/" + base;
#endif
}

Region::~Region() { close(); }

Region::Region(Region&& other) noexcept { *this = std::move(other); }

Region& Region::operator=(Region&& other) noexcept {
  if (this != &other) {
    close();
    base_ = std::exchange(other.base_, nullptr);
    size_ = std::exchange(other.size_, 0);
    name_ = std::move(other.name_);
    owner_ = std::exchange(other.owner_, false);
#ifdef _WIN32
    mapping_ = std::exchange(other.mapping_, nullptr);
#endif
  }
  return *this;
}

#ifdef _WIN32

namespace {
std::string last_error(const char* what) {
  return std::string(what) + " failed (error " +
         std::to_string(GetLastError()) + ")";
}
}  // namespace

bool Region::create(const std::string& base, std::size_t size,
                    std::string& error) {
  close();
  std::string const name = os_name(base);
  auto const wide = static_cast<unsigned long long>(size);
  HANDLE const mapping =
      CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                         static_cast<DWORD>(wide >> 32),
                         static_cast<DWORD>(wide & 0xFFFFFFFFu), name.c_str());
  if (mapping == nullptr) {
    error = last_error("CreateFileMapping");
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // Only a live process can hold a Windows mapping open, and the name
    // carries this process's pid: somebody else chose it on purpose.
    CloseHandle(mapping);
    error = name + " is already in use";
    return false;
  }
  void* const view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (view == nullptr) {
    error = last_error("MapViewOfFile");
    CloseHandle(mapping);
    return false;
  }
  // A page-file-backed mapping starts zero-filled.
  mapping_ = mapping;
  base_ = view;
  size_ = size;
  name_ = name;
  owner_ = true;
  return true;
}

bool Region::open_read(const std::string& base, std::string& error) {
  close();
  std::string const name = os_name(base);
  HANDLE const mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
  if (mapping == nullptr) {
    error = last_error("OpenFileMapping");
    return false;
  }
  void* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (view == nullptr) {
    error = last_error("MapViewOfFile");
    CloseHandle(mapping);
    return false;
  }
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(view, &info, sizeof(info)) == 0) {
    error = last_error("VirtualQuery");
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return false;
  }
  mapping_ = mapping;
  base_ = view;
  size_ = info.RegionSize;
  name_ = name;
  owner_ = false;
  return true;
}

void Region::close() {
  if (base_ != nullptr) UnmapViewOfFile(base_);
  if (mapping_ != nullptr) CloseHandle(mapping_);
  base_ = nullptr;
  mapping_ = nullptr;
  size_ = 0;
  owner_ = false;
  name_.clear();
}

#else  // POSIX

namespace {
std::string errno_text(const char* what) {
  return std::string(what) + " failed: " + std::strerror(errno);
}
}  // namespace

bool Region::create(const std::string& base, std::size_t size,
                    std::string& error) {
  close();
  std::string const name = os_name(base);
  int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0 && errno == EEXIST) {
    // Left by a process that died without unlinking it (a kill -9, a crash,
    // a second Ctrl+C). The pid in the name was that process's and is this
    // one's now, so nothing alive is using it: replace it.
    shm_unlink(name.c_str());
    fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  }
  if (fd < 0) {
    error = errno_text("shm_open");
    return false;
  }
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
    error = errno_text("ftruncate");
    ::close(fd);
    shm_unlink(name.c_str());
    return false;
  }
  void* const view =
      mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);  // the mapping keeps the object alive
  if (view == MAP_FAILED) {
    error = errno_text("mmap");
    shm_unlink(name.c_str());
    return false;
  }
  // ftruncate of a new object zero-fills it.
  base_ = view;
  size_ = size;
  name_ = name;
  owner_ = true;
  return true;
}

bool Region::open_read(const std::string& base, std::string& error) {
  close();
  std::string const name = os_name(base);
  int const fd = shm_open(name.c_str(), O_RDONLY, 0);
  if (fd < 0) {
    error = errno_text("shm_open");
    return false;
  }
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    error = errno_text("fstat");
    ::close(fd);
    return false;
  }
  auto const size = static_cast<std::size_t>(st.st_size);
  void* const view = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);
  if (view == MAP_FAILED) {
    error = errno_text("mmap");
    return false;
  }
  base_ = view;
  size_ = size;
  name_ = name;
  owner_ = false;
  return true;
}

void Region::close() {
  if (base_ != nullptr) munmap(base_, size_);
  if (owner_) shm_unlink(name_.c_str());
  base_ = nullptr;
  size_ = 0;
  owner_ = false;
  name_.clear();
}

#endif

}  // namespace shm
