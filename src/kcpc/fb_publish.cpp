#include "kcpc/fb_publish.h"

#include <atomic>
#include <string>

#include "kcpc/fb_ring.h"
#include "log.h"
#include "shm_region.h"
#include "startup_manifest.h"  // startup_manifest_self_pid
#include "subcycle/machine.h"  // kFbWidth, kFbHeight

namespace {

// Touched by the main thread before the frame runner starts and after it
// ends (fb_publish_start/stop), by the frame runner alone in between.
shm::Region g_region;
kcpc::FbRingWriter g_writer;
std::atomic<bool> g_running{false};
// Written before g_running is set; read by anyone while it is.
std::string g_os_name;
// The writer's seq() for other threads: FB_INFO reads it off a socket thread.
std::atomic<std::uint64_t> g_seq{0};

}  // namespace

bool fb_publish_start() {
  if (g_running.load(std::memory_order_acquire)) return true;
  constexpr auto kStride = static_cast<std::uint32_t>(subcycle::kFbWidth * 3);
  constexpr auto kHeight = static_cast<std::uint32_t>(subcycle::kFbHeight);
  auto const pid = static_cast<std::uint64_t>(startup_manifest_self_pid());
  std::string const name = kcpc::fb_ring_name(pid);
  std::string error;
  if (!g_region.create(name, kcpc::fb_ring_bytes(kStride, kHeight), error)) {
    LOG_ERROR("Frame ring: cannot create " << shm::os_name(name) << ": "
                                           << error
                                           << "; frames are not published");
    return false;
  }
  if (!g_writer.init(g_region.data(), g_region.size(),
                     static_cast<std::uint32_t>(subcycle::kFbWidth), kHeight,
                     kStride, pid)) {
    LOG_ERROR("Frame ring: " << shm::os_name(name) << " is too small");
    g_region.close();
    return false;
  }
  g_os_name = shm::os_name(name);
  g_seq.store(0, std::memory_order_relaxed);
  g_running.store(true, std::memory_order_release);
  LOG_INFO("Frame ring: publishing to " << shm::os_name(name));
  return true;
}

void fb_publish_frame(const std::uint8_t* rgb24) {
  if (rgb24 == nullptr || !g_running.load(std::memory_order_acquire)) return;
  g_seq.store(g_writer.publish(rgb24), std::memory_order_release);
}

void fb_publish_stop() {
  if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
  g_region.close();
  g_seq.store(0, std::memory_order_relaxed);
}

std::uint64_t fb_publish_seq() {
  if (!g_running.load(std::memory_order_acquire)) return 0;
  return g_seq.load(std::memory_order_acquire);
}

std::string fb_publish_shm_name() {
  if (!g_running.load(std::memory_order_acquire)) return {};
  return g_os_name;
}
