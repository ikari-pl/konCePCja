#pragma once

// A window manager may echo a command-line file back to the application as a
// drag-and-drop event once the window exists — macOS does this for every
// launch argument, after the files have already been mounted. Such an echo is
// not a user action and must not be handled like one: re-loading each echoed
// disk into drive A clobbers a launch that filled both drives.
//
// This is the filter for exactly that, and nothing more. It matches whole
// paths, consumes each registration once, and stops matching shortly after
// launch. Everything it does not match is a real drop, which the application
// must act on AND report — a drop that changes nothing and says nothing is
// indistinguishable from a broken emulator.
//
// Kept free of SDL and the filesystem so it can be tested directly: the caller
// supplies already-canonical paths and the clock.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

class LaunchEchoFilter {
 public:
  // Register the launch files, canonical paths, as echo candidates. Only
  // drops arriving within `window_ms` of `now_ms` can be treated as echoes.
  void arm(const std::vector<std::string>& canonical_files,
           std::uint64_t now_ms, std::uint64_t window_ms) {
    pending_.clear();
    pending_.insert(canonical_files.begin(), canonical_files.end());
    deadline_ms_ = now_ms + window_ms;
  }

  // True if this drop is an echo of a launch file — at most once per file,
  // and never after the window closes. Any other drop is the user's.
  bool consume(const std::string& canonical_path, std::uint64_t now_ms) {
    if (pending_.empty()) return false;
    if (now_ms > deadline_ms_) {
      pending_.clear();  // window closed: never swallow a drop again
      return false;
    }
    return pending_.erase(canonical_path) > 0;
  }

  bool armed() const { return !pending_.empty(); }

 private:
  std::set<std::string> pending_;
  std::uint64_t deadline_ms_ = 0;
};
