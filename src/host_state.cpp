// UI-neutral host state (beads-cv2.4, beads-cv2.5).  See host_state.h.

#include "host_state.h"

#include <mutex>

HostTapeView g_host_tape;
HostStatusView g_host_status;

namespace {
// Guards g_metrics: the frame runner writes it once a second, the render
// thread (--debug bar) and the IPC thread ('metrics') read it.
std::mutex g_metrics_mutex;
HostFrameMetrics g_metrics;
}  // namespace

void host_tape_push_wave(byte level) {
  g_host_tape.wave_buf[g_host_tape.wave_head] = level;
  g_host_tape.wave_head =
      (g_host_tape.wave_head + 1) % HostTapeView::kWaveSamples;
}

void host_tape_push_decoded(byte bit) {
  g_host_tape.decoded_buf[g_host_tape.decoded_head] = bit;
  g_host_tape.decoded_head =
      (g_host_tape.decoded_head + 1) % HostTapeView::kDecodedSamples;
}

void host_tape_clear_decoded() {
  g_host_tape.decoded_head = 0;
  for (byte& b : g_host_tape.decoded_buf) b = 0;
}

HostFrameMetrics host_frame_metrics() {
  std::scoped_lock const lock(g_metrics_mutex);
  return g_metrics;
}

void host_frame_metrics_publish(const HostFrameMetrics& m) {
  std::scoped_lock const lock(g_metrics_mutex);
  g_metrics = m;
}
