#include "kcpc/fb_ring.h"

#include <cstring>
#include <new>

namespace kcpc {

namespace {

constexpr std::uint64_t kAlign = 64;

constexpr std::uint64_t round_up(std::uint64_t n) {
  return (n + kAlign - 1) / kAlign * kAlign;
}

std::uint64_t slots_offset() { return round_up(sizeof(FbRingHeader)); }

const std::uint8_t* slot_pixels(const FbRingHeader& h, std::uint64_t slot) {
  return reinterpret_cast<const std::uint8_t*>(&h) + h.slots_offset +
         (slot * h.slot_stride);
}

}  // namespace

std::string fb_ring_name(std::uint64_t pid) {
  return "kcpc-fb-" + std::to_string(pid);
}

std::size_t fb_ring_bytes(std::uint32_t stride, std::uint32_t height) {
  std::uint64_t const slot = std::uint64_t{stride} * height;
  return static_cast<std::size_t>(slots_offset() +
                                  (round_up(slot) * kFbRingSlots));
}

bool FbRingWriter::init(void* base, std::size_t size, std::uint32_t width,
                        std::uint32_t height, std::uint32_t stride,
                        std::uint64_t pid) {
  if (base == nullptr || size < fb_ring_bytes(stride, height)) return false;
  // The region is zero-filled; constructing the header there starts the
  // lifetime of its atomics without touching other processes' view of them.
  auto* h = new (base) FbRingHeader{};
  h->version = kFbRingVersion;
  h->header_bytes = sizeof(FbRingHeader);
  h->format = kFbFormatRgb24;
  h->width = width;
  h->height = height;
  h->stride = stride;
  h->ring_slots = kFbRingSlots;
  h->slot_bytes = std::uint64_t{stride} * height;
  h->slot_stride = round_up(h->slot_bytes);
  h->slots_offset = slots_offset();
  h->producer_pid = pid;
  // Last, with release: a reader that sees the magic sees every field above.
  h->magic.store(kFbRingMagic, std::memory_order_release);
  header_ = h;
  slots_ = static_cast<std::uint8_t*>(base) + h->slots_offset;
  seq_ = 0;
  return true;
}

std::uint64_t FbRingWriter::publish(const std::uint8_t* pixels) {
  FbRingHeader& h = *header_;
  std::uint64_t const n = ++seq_;
  std::uint64_t const slot = n % kFbRingSlots;
  // Odd while the copy runs. The fence orders this store before the pixel
  // stores, so a reader that copied any new pixel sees the stamp move.
  h.slot_seq[slot].store((2 * n) - 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  std::memcpy(slots_ + (slot * h.slot_stride), pixels,
              static_cast<std::size_t>(h.slot_bytes));
  h.slot_seq[slot].store(2 * n, std::memory_order_release);
  h.latest_slot.store(slot, std::memory_order_relaxed);
  h.latest_seq.store(n, std::memory_order_release);
  return n;
}

const FbRingHeader* fb_ring_header(const void* base, std::size_t size) {
  if (base == nullptr || size < sizeof(FbRingHeader)) return nullptr;
  const auto* h = static_cast<const FbRingHeader*>(base);
  if (h->magic.load(std::memory_order_acquire) != kFbRingMagic) return nullptr;
  if (h->version != kFbRingVersion || h->format != kFbFormatRgb24 ||
      h->header_bytes != sizeof(FbRingHeader) || h->ring_slots == 0 ||
      h->ring_slots > kFbRingSlots ||
      h->slot_bytes != std::uint64_t{h->stride} * h->height ||
      h->slot_stride < h->slot_bytes || h->slots_offset < sizeof(FbRingHeader))
    return nullptr;
  // Every slot inside the region, without overflowing on hostile values.
  std::uint64_t const room = size - sizeof(FbRingHeader);
  if (h->slots_offset - sizeof(FbRingHeader) > room) return nullptr;
  std::uint64_t const left = size - h->slots_offset;
  if (h->slot_stride > left / h->ring_slots) return nullptr;
  return h;
}

FbReadStatus fb_ring_begin_read(const FbRingHeader& header, FbSlotRead& read) {
  std::uint64_t const latest =
      header.latest_seq.load(std::memory_order_acquire);
  if (latest == 0) return FbReadStatus::kEmpty;
  // At least as new as `latest`: it was stored before it.
  std::uint64_t const slot =
      header.latest_slot.load(std::memory_order_relaxed) % header.ring_slots;
  std::uint64_t const stamp =
      header.slot_seq[slot].load(std::memory_order_acquire);
  if (stamp == 0 || (stamp & 1) != 0) return FbReadStatus::kTorn;
  read.pixels = slot_pixels(header, slot);
  read.seq = stamp / 2;
  read.slot = static_cast<std::uint32_t>(slot);
  read.stamp = stamp;
  return FbReadStatus::kOk;
}

bool fb_ring_end_read(const FbRingHeader& header, const FbSlotRead& read) {
  // Orders the pixel loads before the stamp load: had the producer started
  // on this slot again before any of them, the stamp is no longer the same.
  std::atomic_thread_fence(std::memory_order_acquire);
  return header.slot_seq[read.slot].load(std::memory_order_relaxed) ==
         read.stamp;
}

FbReadStatus fb_ring_read_latest(const void* base, std::size_t size,
                                 std::uint8_t* dst, std::size_t dst_size,
                                 std::uint64_t& seq, int attempts) {
  const FbRingHeader* h = fb_ring_header(base, size);
  if (h == nullptr || dst_size < h->slot_bytes) return FbReadStatus::kBadHeader;
  FbReadStatus status = FbReadStatus::kTorn;
  for (int i = 0; i < attempts; ++i) {
    FbSlotRead read;
    status = fb_ring_begin_read(*h, read);
    if (status == FbReadStatus::kEmpty) return status;
    if (status != FbReadStatus::kOk) continue;
    std::memcpy(dst, read.pixels, static_cast<std::size_t>(h->slot_bytes));
    if (fb_ring_end_read(*h, read)) {
      seq = read.seq;
      return FbReadStatus::kOk;
    }
    status = FbReadStatus::kTorn;
  }
  return status;
}

}  // namespace kcpc
