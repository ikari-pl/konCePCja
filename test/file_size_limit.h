/* Scoped per-process file-size limit, to make a regular file fail its
 * writes on demand — the disk-full case, portable to macOS (which has no
 * /dev/full). While active, a write that would grow any regular file past
 * `max_bytes` fails with EFBIG instead of raising SIGXFSZ. Keep the scope
 * tight: the limit is process-wide.
 *
 * POSIX only; `supported()` is false on Windows, where tests should skip.
 */

#pragma once

#ifndef _WIN32
#include <sys/resource.h>

#include <csignal>
#endif

class ScopedFileSizeLimit {
 public:
  static constexpr bool supported() {
#ifdef _WIN32
    return false;
#else
    return true;
#endif
  }

#ifdef _WIN32
  explicit ScopedFileSizeLimit(unsigned long /*max_bytes*/) {}
#else
  explicit ScopedFileSizeLimit(rlim_t max_bytes) {
    old_handler_ = std::signal(SIGXFSZ, SIG_IGN);
    active_ = getrlimit(RLIMIT_FSIZE, &old_) == 0;
    if (active_) {
      struct rlimit lim = old_;
      lim.rlim_cur = max_bytes;
      active_ = setrlimit(RLIMIT_FSIZE, &lim) == 0;
    }
  }
  ~ScopedFileSizeLimit() {
    if (active_) setrlimit(RLIMIT_FSIZE, &old_);
    std::signal(SIGXFSZ, old_handler_);
  }
#endif
  ScopedFileSizeLimit(const ScopedFileSizeLimit&) = delete;
  ScopedFileSizeLimit& operator=(const ScopedFileSizeLimit&) = delete;

  bool active() const { return active_; }

 private:
  bool active_ = false;
#ifndef _WIN32
  struct rlimit old_{};
  void (*old_handler_)(int) = SIG_DFL;
#endif
};
