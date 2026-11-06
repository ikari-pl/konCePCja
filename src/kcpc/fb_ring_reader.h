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

// Why open() failed, for a caller deciding whether to try again.
enum class FbOpenFailure : std::uint8_t {
  kNone,       // open() succeeded (or was never called)
  kNoName,     // an empty name: the core publishes no ring
  kNoRegion,   // no region of that name: the core is gone, or never had one
  kTooSmall,   // the region cannot hold a ring header: never a ring
  kNotYet,     // the magic is not stored yet: the producer is laying the
               // header out, try again
  kVersion,    // a ring of another version: this reader cannot read it
  kBadHeader,  // a header whose fields disagree with each other or with
               // the region's size: not a ring this reader trusts
};

class FbRingReader {
 public:
  // Maps the ring of this name: FB_INFO's `shm_name`, the OS name
  // ("/kcpc-fb-<pid>" on POSIX, "Local\kcpc-fb-<pid>" on Windows), or the
  // base name without the prefix (shm::base_name()). False, with `error`
  // saying why and failure() saying which way: there is no such region,
  // the producer has not laid the header out yet (try again), or it holds
  // no ring this code reads.
  bool open(const std::string& shm_name, std::string& error);
  void close();
  bool is_open() const { return header_ != nullptr; }
  // Why the last open() failed; kNone after one that succeeded.
  FbOpenFailure failure() const { return failure_; }

  // From the ring's header; 0 while closed.
  std::uint32_t width() const;
  std::uint32_t height() const;
  std::uint32_t stride() const;
  // The bytes of one frame, as the header said when open() validated it.
  std::size_t frame_bytes() const { return frame_bytes_; }
  std::uint64_t producer_pid() const;
  // The latest whole frame's sequence number; 0 before the first.
  std::uint64_t latest_seq() const;

  // Copies the latest whole frame into `dst` (resized to frame_bytes()) and
  // its sequence number into `seq`. kTorn when the producer lapped every
  // attempt (call again); `dst` then holds nothing to show. kBadHeader when
  // the header no longer reads as the ring open() validated (nothing is
  // allocated on a header's say-so).
  FbReadStatus read_latest(std::vector<std::uint8_t>& dst, std::uint64_t& seq,
                           int attempts = 8);

 private:
  shm::Region region_;
  const FbRingHeader* header_ = nullptr;
  std::size_t frame_bytes_ = 0;
  FbOpenFailure failure_ = FbOpenFailure::kNone;
};

}  // namespace kcpc
