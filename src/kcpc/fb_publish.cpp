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
  g_running.store(true, std::memory_order_release);
  LOG_INFO("Frame ring: publishing to " << shm::os_name(name));
  return true;
}

void fb_publish_frame(const std::uint8_t* rgb24) {
  if (rgb24 == nullptr || !g_running.load(std::memory_order_acquire)) return;
  g_writer.publish(rgb24);
}

void fb_publish_stop() {
  if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
  g_region.close();
}
