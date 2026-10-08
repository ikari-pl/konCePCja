// UI-neutral host state (beads-cv2.4).  See host_state.h.

#include "host_state.h"

#include <mutex>

// The telemetry storage still lives in imgui_state until beads-cv2.5 moves
// it here.  imgui_state.cpp is part of every build, MODERN_UI or not, so
// reading it from this file adds no UI dependency to the link.
#include "imgui_state.h"
#include "koncepcja.h"

HostTapeView g_host_tape;

HostFrameMetrics host_frame_metrics() {
  HostFrameMetrics m;
  std::scoped_lock const lock(g_imgui_stats_mutex);
  m.frame_time_avg_us = imgui_state.frame_time_avg_us;
  m.display_time_avg_us = imgui_state.display_time_avg_us;
  m.z80_time_avg_us = imgui_state.z80_time_avg_us;
  m.sleep_time_avg_us = imgui_state.sleep_time_avg_us;
  m.audio_queue_avg_ms = imgui_state.audio_queue_avg_ms;
  m.audio_queue_min_ms = imgui_state.audio_queue_min_ms;
  m.audio_underruns = imgui_state.audio_underruns;
  m.audio_near_underruns = imgui_state.audio_near_underruns;
  return m;
}
