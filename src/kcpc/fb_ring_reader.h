#pragma once

// A reader of a core's frame ring (docs/protocol/fb-ring-v1.md), for a client
// in another process (beads-3sp.1). It maps the ring FB_INFO names read-only
// through shm_region and copies the latest whole frame with
// fb_ring_read_latest(): the layout, the seqlock and the torn-copy check are
// fb_ring.h's, not repeated here.
//
// Not thread-safe: one thread reads. It never writes to the ring, and the
// producer never waits for it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kcpc/fb_ring.h"
#include "shm_region.h"

namespace kcpc {

class FbRingReader {
 public:
  // Maps the ring of this OS name: FB_INFO's `shm_name` ("/kcpc-fb-<pid>" on
  // POSIX, "Local\kcpc-fb-<pid>" on Windows). False, with `error` saying why,
  // when there is no such region or it holds no ring this code reads (the
  // producer may not have laid the header out yet: try again).
  bool open(const std::string& shm_name, std::string& error);
  void close();
  bool is_open() const { return header_ != nullptr; }

  // From the ring's header; 0 while closed.
  std::uint32_t width() const;
  std::uint32_t height() const;
  std::uint32_t stride() const;
  std::size_t frame_bytes() const;
  std::uint64_t producer_pid() const;
  // The latest whole frame's sequence number; 0 before the first.
  std::uint64_t latest_seq() const;

  // Copies the latest whole frame into `dst` (resized to frame_bytes()) and
  // its sequence number into `seq`. kTorn when the producer lapped every
  // attempt (call again); `dst` then holds nothing to show.
  FbReadStatus read_latest(std::vector<std::uint8_t>& dst, std::uint64_t& seq,
                           int attempts = 8);

 private:
  shm::Region region_;
  const FbRingHeader* header_ = nullptr;
};

}  // namespace kcpc
