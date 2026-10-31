// konCePCja — tests for the shared-memory frame ring (beads-0ir.2).
//
// The ring is read by other processes that cannot ask the producer for
// anything, so what is pinned here is what a reader can rely on: the latest
// frame is whole, the ring wraps, a copy the producer laps is refused rather
// than handed out torn, and a region with the core's name is created, read
// from a second mapping and gone after close, even over a stale one.

#include "kcpc/fb_ring.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "kcpc/fb_publish.h"
#include "kcpc/kcpc_host.h"  // kcpc_host_fb_info
#include "shm_region.h"
#include "startup_manifest.h"  // startup_manifest_self_pid
#include "subcycle/machine.h"  // kFbWidth, kFbHeight

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using kcpc::FbReadStatus;
using kcpc::FbRingWriter;
using kcpc::FbSlotRead;

constexpr std::uint32_t kW = 16;
constexpr std::uint32_t kH = 8;
constexpr std::uint32_t kStride = kW * 3;
constexpr std::size_t kFrame = std::size_t{kStride} * kH;

// A 64-byte aligned, zero-filled buffer the size of a ring.
struct RingMemory {
  explicit RingMemory(std::size_t n) : words((n + 7) / 8 + 8, 0), size(n) {
    auto const raw = reinterpret_cast<std::uintptr_t>(words.data());
    base = reinterpret_cast<void*>((raw + 63) & ~std::uintptr_t{63});
  }
  std::vector<std::uint64_t> words;
  void* base = nullptr;
  std::size_t size = 0;
};

// Every byte of frame n is n & 0xFF, and its first 8 bytes are n: a frame
// made of two publishes cannot pass check_frame().
std::vector<std::uint8_t> make_frame(std::uint64_t n) {
  std::vector<std::uint8_t> f(kFrame, static_cast<std::uint8_t>(n & 0xFF));
  std::memcpy(f.data(), &n, sizeof(n));
  return f;
}

bool check_frame(const std::uint8_t* p, std::uint64_t n) {
  std::uint64_t head = 0;
  std::memcpy(&head, p, sizeof(head));
  if (head != n) return false;
  for (std::size_t i = sizeof(head); i < kFrame; ++i)
    if (p[i] != static_cast<std::uint8_t>(n & 0xFF)) return false;
  return true;
}

TEST(FbRingTest, NameIsShortEnoughForMacOs) {
  // PSHMNAMLEN is 31 on macOS, the leading slash included.
  std::string const name = kcpc::fb_ring_name(4294967295ULL);
  EXPECT_EQ(name, "kcpc-fb-4294967295");
  EXPECT_LE(shm::os_name(name).size(), 31u);
}

TEST(FbRingTest, HeaderDescribesTheFrame) {
  RingMemory mem(kcpc::fb_ring_bytes(kStride, kH));
  FbRingWriter w;
  ASSERT_TRUE(w.init(mem.base, mem.size, kW, kH, kStride, 1234));
  const kcpc::FbRingHeader* h = kcpc::fb_ring_header(mem.base, mem.size);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->magic.load(), kcpc::kFbRingMagic);
  EXPECT_EQ(std::memcmp(mem.base, "FBUF", 4), 0);
  EXPECT_EQ(h->version, kcpc::kFbRingVersion);
  EXPECT_EQ(h->header_bytes, sizeof(kcpc::FbRingHeader));
  EXPECT_EQ(h->format, kcpc::kFbRingFormatRgb24);
  EXPECT_EQ(h->width, kW);
  EXPECT_EQ(h->height, kH);
  EXPECT_EQ(h->stride, kStride);
  EXPECT_EQ(h->ring_slots, kcpc::kFbRingSlots);
  EXPECT_EQ(h->slot_bytes, kFrame);
  EXPECT_EQ(h->slot_stride % 64, 0u);
  EXPECT_GE(h->slot_stride, kFrame);
  EXPECT_EQ(h->slots_offset % 64, 0u);
  EXPECT_LE(h->slots_offset + h->slot_stride * h->ring_slots, mem.size);
  EXPECT_EQ(h->producer_pid, 1234u);
  EXPECT_EQ(h->latest_seq.load(), 0u);
}

TEST(FbRingTest, RefusesTooSmallAndForeignMemory) {
  RingMemory mem(kcpc::fb_ring_bytes(kStride, kH));
  FbRingWriter w;
  EXPECT_FALSE(w.init(mem.base, mem.size - 1, kW, kH, kStride, 1));
  // Zeroed memory has no magic: not a ring (a reader that maps the region
  // before the producer finished its header lands here).
  EXPECT_EQ(kcpc::fb_ring_header(mem.base, mem.size), nullptr);
  ASSERT_TRUE(w.init(mem.base, mem.size, kW, kH, kStride, 1));
  EXPECT_EQ(kcpc::fb_ring_header(mem.base, sizeof(kcpc::FbRingHeader)),
            nullptr);  // the slots do not fit
  std::vector<std::uint8_t> dst(kFrame);
  std::uint64_t seq = 99;
  EXPECT_EQ(kcpc::fb_ring_read_latest(mem.base, mem.size, dst.data(),
                                      dst.size(), seq),
            FbReadStatus::kEmpty);
  EXPECT_EQ(kcpc::fb_ring_read_latest(mem.base, mem.size, dst.data(),
                                      dst.size() - 1, seq),
            FbReadStatus::kBadHeader);
}

TEST(FbRingTest, LatestFrameIsReadInOrderAndTheRingWraps) {
  RingMemory mem(kcpc::fb_ring_bytes(kStride, kH));
  FbRingWriter w;
  ASSERT_TRUE(w.init(mem.base, mem.size, kW, kH, kStride, 1));
  const kcpc::FbRingHeader* h = kcpc::fb_ring_header(mem.base, mem.size);
  ASSERT_NE(h, nullptr);
  std::vector<std::uint8_t> dst(kFrame);
  for (std::uint64_t n = 1; n <= 3 * kcpc::kFbRingSlots + 1; ++n) {
    EXPECT_EQ(w.publish(make_frame(n).data()), n);
    EXPECT_EQ(h->latest_seq.load(), n);
    EXPECT_EQ(h->latest_slot.load(), n % kcpc::kFbRingSlots);
    EXPECT_EQ(h->slot_seq[n % kcpc::kFbRingSlots].load(), 2 * n);
    std::uint64_t seq = 0;
    ASSERT_EQ(kcpc::fb_ring_read_latest(mem.base, mem.size, dst.data(),
                                        dst.size(), seq),
              FbReadStatus::kOk);
    EXPECT_EQ(seq, n);
    EXPECT_TRUE(check_frame(dst.data(), n)) << "frame " << n;
  }
  // Every slot holds one of the last four frames, whole.
  for (std::uint32_t s = 0; s < kcpc::kFbRingSlots; ++s) {
    std::uint64_t const stamp = h->slot_seq[s].load();
    ASSERT_EQ(stamp % 2, 0u);
    const std::uint8_t* p = static_cast<const std::uint8_t*>(mem.base) +
                            h->slots_offset + s * h->slot_stride;
    EXPECT_TRUE(check_frame(p, stamp / 2)) << "slot " << s;
  }
}

TEST(FbRingTest, ACopyTheProducerLapsIsRefused) {
  RingMemory mem(kcpc::fb_ring_bytes(kStride, kH));
  FbRingWriter w;
  ASSERT_TRUE(w.init(mem.base, mem.size, kW, kH, kStride, 1));
  const kcpc::FbRingHeader* h = kcpc::fb_ring_header(mem.base, mem.size);
  ASSERT_NE(h, nullptr);
  w.publish(make_frame(1).data());

  FbSlotRead read;
  ASSERT_EQ(kcpc::fb_ring_begin_read(*h, read), FbReadStatus::kOk);
  EXPECT_EQ(read.seq, 1u);
  // Three more frames land in the other slots: the copy is still frame 1.
  for (std::uint64_t n = 2; n <= kcpc::kFbRingSlots; ++n)
    w.publish(make_frame(n).data());
  EXPECT_TRUE(kcpc::fb_ring_end_read(*h, read));
  // The fifth frame rewrites the slot being copied: refused.
  w.publish(make_frame(kcpc::kFbRingSlots + 1).data());
  EXPECT_FALSE(kcpc::fb_ring_end_read(*h, read));
}

TEST(FbRingTest, ASlotBeingWrittenIsNotHandedOut) {
  RingMemory mem(kcpc::fb_ring_bytes(kStride, kH));
  FbRingWriter w;
  ASSERT_TRUE(w.init(mem.base, mem.size, kW, kH, kStride, 1));
  auto* h = static_cast<kcpc::FbRingHeader*>(mem.base);
  w.publish(make_frame(1).data());
  // What a reader sees while the producer is half way through that slot
  // again (it lapped the ring): an odd stamp.
  h->slot_seq[1].store(2 * 5 - 1);
  FbSlotRead read;
  EXPECT_EQ(kcpc::fb_ring_begin_read(*h, read), FbReadStatus::kTorn);
  std::vector<std::uint8_t> dst(kFrame);
  std::uint64_t seq = 0;
  EXPECT_EQ(kcpc::fb_ring_read_latest(mem.base, mem.size, dst.data(),
                                      dst.size(), seq, 3),
            FbReadStatus::kTorn);
}

// A reader thread copying while the producer publishes without pause: every
// copy it accepts is one whole frame, never older than one it accepted
// before. Copies alternate between one memcpy (the producer rarely laps it)
// and byte by byte with yields (the producer laps most of them), so both
// outcomes happen and the check is exercised rather than vacuous. Bounded by
// time, not by turns: on a busy two-core CI runner the producer may not run
// at all for the first few thousand turns.
TEST(FbRingTest, AConcurrentReaderNeverAcceptsATornFrame) {
  RingMemory mem(kcpc::fb_ring_bytes(kStride, kH));
  FbRingWriter w;
  ASSERT_TRUE(w.init(mem.base, mem.size, kW, kH, kStride, 1));
  std::vector<std::vector<std::uint8_t>> frames;
  for (std::uint64_t n = 0; n < 256; ++n) frames.push_back(make_frame(n));
  std::atomic<bool> stop{false};
  std::thread producer([&] {
    std::vector<std::uint8_t> f(kFrame);
    while (!stop.load(std::memory_order_relaxed)) {
      std::uint64_t const n = w.seq() + 1;
      f = frames[n & 0xFF];
      std::memcpy(f.data(), &n, sizeof(n));
      w.publish(f.data());
      // Lets the reader run on a machine with fewer cores than threads.
      std::this_thread::yield();
    }
  });
  const kcpc::FbRingHeader* h = kcpc::fb_ring_header(mem.base, mem.size);
  ASSERT_NE(h, nullptr);
  std::vector<std::uint8_t> dst(kFrame);
  std::uint64_t last = 0;
  int accepted = 0;
  int refused = 0;
  int bad = 0;
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  for (std::uint64_t turn = 0; (accepted < 500 || refused < 5) &&
                               std::chrono::steady_clock::now() < deadline;
       ++turn) {
    FbSlotRead read;
    if (kcpc::fb_ring_begin_read(*h, read) != FbReadStatus::kOk) {
      std::this_thread::yield();
      continue;
    }
    if (turn % 2 == 0) {
      std::memcpy(dst.data(), read.pixels, kFrame);
    } else {
      for (std::size_t b = 0; b < kFrame; ++b) {
        dst[b] = read.pixels[b];
        if ((b & 15) == 0) std::this_thread::yield();
      }
    }
    if (!kcpc::fb_ring_end_read(*h, read)) {
      ++refused;
      continue;
    }
    ++accepted;
    if (!check_frame(dst.data(), read.seq) || read.seq < last) ++bad;
    last = read.seq;
  }
  stop = true;
  producer.join();
  EXPECT_EQ(bad, 0);
  EXPECT_GT(accepted, 0);
  EXPECT_GT(refused, 0) << "the producer never lapped a copy";
}

TEST(ShmRegionTest, ARegionIsSharedBetweenMappingsAndGoneAfterClose) {
  std::string const base =
      "kcpc-region-" + std::to_string(startup_manifest_self_pid());
  std::string error;
  shm::Region owner;
  ASSERT_TRUE(owner.create(base, 4096, error)) << error;
  ASSERT_GE(owner.size(), 4096u);
  auto* bytes = static_cast<std::uint8_t*>(owner.data());
  EXPECT_EQ(bytes[0], 0);
  EXPECT_EQ(bytes[4095], 0);
  bytes[17] = 0xA5;

  shm::Region reader;
  ASSERT_TRUE(reader.open_read(base, error)) << error;
  ASSERT_GE(reader.size(), 4096u);
  EXPECT_EQ(static_cast<const std::uint8_t*>(reader.data())[17], 0xA5);
  bytes[18] = 0x5A;  // a later write is seen through the other mapping too
  EXPECT_EQ(static_cast<const std::uint8_t*>(reader.data())[18], 0x5A);
  reader.close();

  owner.close();
  EXPECT_FALSE(owner.is_open());
  owner.close();  // twice is fine
  shm::Region again;
  EXPECT_FALSE(again.open_read(base, error));
}

// The core's side as KCPC sees it (beads-0ir.3): while the ring runs,
// FB_INFO names it and reports the sequence number of its latest frame, the
// one EVT_FRAME carries; without it FB_INFO says there is no transport.
TEST(FbPublishTest, FbInfoNamesTheRingAndItsLatestFrame) {
  kcpc::FbInfo info;
  ASSERT_EQ(kcpc_host_fb_info(info), kcpc::Status::kOk);
  EXPECT_EQ(info.transport, kcpc::kFbTransportNone) << "not started yet";
  EXPECT_TRUE(info.shm_name.empty());
  EXPECT_EQ(fb_publish_seq(), 0U);

  ASSERT_TRUE(fb_publish_start());
  std::string const base = kcpc::fb_ring_name(
      static_cast<std::uint64_t>(startup_manifest_self_pid()));
  ASSERT_EQ(kcpc_host_fb_info(info), kcpc::Status::kOk);
  EXPECT_EQ(info.transport, kcpc::kFbTransportShm);
  EXPECT_EQ(info.shm_name, shm::os_name(base));
  EXPECT_EQ(info.fb_seq, 0U) << "nothing published yet";

  std::vector<std::uint8_t> const frame(
      std::size_t{subcycle::kFbWidth} * 3 * subcycle::kFbHeight, 0x5A);
  fb_publish_frame(frame.data());
  fb_publish_frame(frame.data());
  EXPECT_EQ(fb_publish_seq(), 2U);
  ASSERT_EQ(kcpc_host_fb_info(info), kcpc::Status::kOk);
  EXPECT_EQ(info.fb_seq, 2U);

  // The name FB_INFO gives is the one a reader opens.
  std::string error;
  shm::Region reader;
  ASSERT_TRUE(reader.open_read(base, error)) << error;
  const kcpc::FbRingHeader* header =
      kcpc::fb_ring_header(reader.data(), reader.size());
  ASSERT_NE(header, nullptr);
  EXPECT_EQ(header->latest_seq.load(), 2U);
  reader.close();

  fb_publish_stop();
  ASSERT_EQ(kcpc_host_fb_info(info), kcpc::Status::kOk);
  EXPECT_EQ(info.transport, kcpc::kFbTransportNone);
  EXPECT_TRUE(info.shm_name.empty());
  EXPECT_EQ(fb_publish_seq(), 0U);
}

#ifndef _WIN32
// A core that died without unlinking (kill -9) leaves its name behind; the
// next process that gets the same pid must still start.
TEST(ShmRegionTest, AStaleRegionOfTheSameNameIsReplaced) {
  std::string const base =
      "kcpc-stale-" + std::to_string(startup_manifest_self_pid());
  std::string const name = shm::os_name(base);
  int const fd = shm_open(name.c_str(), O_RDWR | O_CREAT, 0600);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(ftruncate(fd, 64), 0);
  close(fd);

  std::string error;
  shm::Region region;
  ASSERT_TRUE(region.create(base, 8192, error)) << error;
  EXPECT_GE(region.size(), 8192u);
  EXPECT_EQ(static_cast<const std::uint8_t*>(region.data())[8191], 0);
  region.close();
  shm::Region gone;
  EXPECT_FALSE(gone.open_read(base, error));
}
#endif

}  // namespace
