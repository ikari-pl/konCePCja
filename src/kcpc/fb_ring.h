#pragma once

// The frame ring: the machine's canonical frame published to other processes
// through shared memory (beads-0ir.2). One producer (the core's frame runner)
// and any number of readers that never talk back: the producer never waits
// for a reader, a reader never blocks the producer, and a reader that copied
// a slot while the producer rewrote it finds out and tries again.
//
// The layout is the contract, so it is fixed here byte for byte (FbRingHeader,
// little-endian, every offset static_assert'ed) and a reader in another
// language can follow it:
//
//   offset 0     FbRingHeader (256 bytes)
//   slots_offset ring_slots slots, slot_stride bytes apart, each `height`
//                rows of `stride` bytes, top row first
//
// Publishing frame n (n counts from 1) goes to slot n % ring_slots:
//   1. slot_seq[slot] = 2n - 1 (odd: being written), release fence
//   2. the pixels are copied into the slot
//   3. slot_seq[slot] = 2n (even: frame n, whole), release store
//   4. latest_slot = slot, then latest_seq = n (release)
// A reader loads latest_seq (acquire), then latest_slot, loads that slot's
// seq (acquire; odd or 0 means try again), copies the pixels, issues an
// acquire fence and loads the slot's seq again: if it changed, the producer
// lapped the ring while the copy ran and the copy is torn. It never is
// accepted. With four slots the producer must publish four more frames
// during one copy for that to happen.
//
// Every field read across processes that changes after creation is a
// lock-free 64-bit atomic (static_assert'ed), so the release/acquire pairs
// hold on ARM as well as x86. The fixed fields are written once before
// `magic`, which is stored last with release: a reader that sees the magic
// sees the rest.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kcpc {

inline constexpr std::uint32_t kFbRingMagic = 0x46554246;  // "FBUF" in memory
inline constexpr std::uint32_t kFbRingVersion = 1;
inline constexpr std::uint32_t kFbFormatRgb24 = 1;  // 3 bytes a pixel: R, G, B
inline constexpr std::uint32_t kFbRingSlots = 4;

struct FbRingHeader {
  std::atomic<std::uint32_t> magic;  // kFbRingMagic once the header is whole
  std::uint32_t version;             // kFbRingVersion
  std::uint32_t header_bytes;        // sizeof(FbRingHeader)
  std::uint32_t format;              // kFbFormatRgb24
  std::uint32_t width;               // pixels
  std::uint32_t height;              // rows
  std::uint32_t stride;              // bytes a row
  std::uint32_t ring_slots;          // kFbRingSlots
  std::uint64_t slot_bytes;          // stride * height
  std::uint64_t slot_stride;         // slot_bytes rounded up to 64
  std::uint64_t slots_offset;        // from the start of the region
  std::uint64_t producer_pid;        // the core that publishes
  // Written by the producer, read by every reader (offset 64).
  std::atomic<std::uint64_t> latest_seq;   // last whole frame; 0: none yet
  std::atomic<std::uint64_t> latest_slot;  // the slot latest_seq is in
  std::atomic<std::uint64_t> slot_seq[kFbRingSlots];  // 2n: frame n; odd:
                                                      // being written; 0: empty
  std::uint8_t reserved[256 - 80 - (8 * kFbRingSlots)];
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the ring's sequence numbers are read across processes");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "the ring's magic is read across processes");
static_assert(sizeof(std::atomic<std::uint64_t>) == 8 &&
                  sizeof(std::atomic<std::uint32_t>) == 4,
              "the layout holds plain 32- and 64-bit words");
static_assert(sizeof(FbRingHeader) == 256, "the header is 256 bytes");
static_assert(offsetof(FbRingHeader, slot_bytes) == 32 &&
                  offsetof(FbRingHeader, producer_pid) == 56 &&
                  offsetof(FbRingHeader, latest_seq) == 64 &&
                  offsetof(FbRingHeader, latest_slot) == 72 &&
                  offsetof(FbRingHeader, slot_seq) == 80,
              "the layout is the contract with other processes");

// The base name of the ring a core with this pid publishes (shm::os_name()
// turns it into the OS name: "/kcpc-fb-<pid>" on POSIX, "Local\kcpc-fb-<pid>"
// on Windows).
std::string fb_ring_name(std::uint64_t pid);

// The bytes a ring of rows of `stride` bytes needs, header included.
std::size_t fb_ring_bytes(std::uint32_t stride, std::uint32_t height);

// The producer's side. Not thread-safe: one thread publishes.
class FbRingWriter {
 public:
  // Lays the header out at `base` (64-byte aligned, `size` >=
  // fb_ring_bytes(stride, height), zero-filled) and stores the magic last.
  // False when `size` is too small.
  bool init(void* base, std::size_t size, std::uint32_t width,
            std::uint32_t height, std::uint32_t stride, std::uint64_t pid);

  // Copies one whole frame (height rows of stride bytes) into the next slot
  // and makes it the latest. Returns its sequence number (1, 2, ...).
  std::uint64_t publish(const std::uint8_t* pixels);

  std::uint64_t seq() const { return seq_; }

 private:
  FbRingHeader* header_ = nullptr;
  std::uint8_t* slots_ = nullptr;
  std::uint64_t seq_ = 0;
};

enum class FbReadStatus : std::uint8_t {
  kOk,         // `dst` holds frame `seq`, whole
  kEmpty,      // nothing published yet
  kBadHeader,  // not a ring this reader understands (or not yet initialised)
  kTorn,       // the producer lapped every attempt; try again later
};

// The reader's side, in two halves so a test can put the producer between
// them. fb_ring_begin_read() picks the latest whole slot; the caller copies
// `pixels`, then fb_ring_end_read() says whether the copy is the frame.
struct FbSlotRead {
  const std::uint8_t* pixels = nullptr;
  std::uint64_t seq = 0;
  std::uint32_t slot = 0;
  std::uint64_t stamp = 0;  // slot_seq as first seen
};

// The header at `base` if it is a ring this code reads and fits in `size`,
// else nullptr. Acquire-loads the magic.
const FbRingHeader* fb_ring_header(const void* base, std::size_t size);

FbReadStatus fb_ring_begin_read(const FbRingHeader& header, FbSlotRead& read);
bool fb_ring_end_read(const FbRingHeader& header, const FbSlotRead& read);

// Both halves around one copy into `dst` (slot_bytes long), retried up to
// `attempts` times.
FbReadStatus fb_ring_read_latest(const void* base, std::size_t size,
                                 std::uint8_t* dst, std::size_t dst_size,
                                 std::uint64_t& seq, int attempts = 8);

}  // namespace kcpc
