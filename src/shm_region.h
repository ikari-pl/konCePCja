#pragma once

// A thin shim over named shared memory: POSIX shm_open + mmap, Win32
// CreateFileMapping + MapViewOfFile.
//
// This is the one place with `#ifdef _WIN32` around shared memory (the frame
// ring, beads-0ir.2), as net_socket is for sockets. It carries no policy: no
// layout, no threads, no logging.
//
// Lifetimes differ by platform, and the class hides the difference:
// - POSIX: a name outlives every mapping until it is unlinked. The Region that
//   created it unlinks it when it closes; a process that dies without closing
//   leaves the name behind, which create() replaces.
// - Windows: the mapping lives while any handle to it is open, so it goes away
//   with the last process that has it mapped, however that process ended.
//   There is nothing to unlink and nothing stale to replace. Names live in the
//   session's Local\ namespace: Global\ needs SeCreateGlobalPrivilege.

#include <cstddef>
#include <string>

namespace shm {

// The OS name for `base` (letters, digits, '-' only): "/<base>" on POSIX,
// "Local\<base>" on Windows. macOS refuses POSIX names longer than 31
// characters (PSHMNAMLEN), slash included, so keep `base` under 30.
std::string os_name(const std::string& base);

class Region {
 public:
  Region() = default;
  ~Region();
  Region(Region&& other) noexcept;
  Region& operator=(Region&& other) noexcept;
  Region(const Region&) = delete;
  Region& operator=(const Region&) = delete;

  // A new zero-filled read-write region of `size` bytes under os_name(base).
  // On POSIX a region of that name left by a process that died is unlinked
  // and replaced: the name is this process's own (a pid in it), so nothing
  // alive can be using it. False when the region cannot be made; `error` then
  // says why.
  bool create(const std::string& base, std::size_t size, std::string& error);

  // Maps the region another process (or this one) created, read-only. The
  // size is whatever the creator made it (on POSIX rounded up to a page).
  // False when no region has that name.
  bool open_read(const std::string& base, std::string& error);

  // Unmaps, and unlinks the name if this Region created it (POSIX). Safe to
  // call twice.
  void close();

  void* data() const { return base_; }
  std::size_t size() const { return size_; }
  bool is_open() const { return base_ != nullptr; }

 private:
  void* base_ = nullptr;
  std::size_t size_ = 0;
  std::string name_;  // os_name(), unlinked on close() when owner_
  bool owner_ = false;
#ifdef _WIN32
  void* mapping_ = nullptr;  // the file-mapping HANDLE
#endif
};

}  // namespace shm
