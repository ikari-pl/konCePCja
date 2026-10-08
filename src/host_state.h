#pragma once

// UI-neutral state owned by the host runtime (beads-cv2.4).
//
// Data the emulation side produces and both the modern UI and the IPC server
// read.  It used to live in imgui_state, which made the IPC server depend on a
// UI header.  Nothing here knows about ImGui; the core build links it as is.
//
// beads-cv2.5 moves the rest of the host telemetry (frame timing, audio
// queue, tape waveform, drive LEDs) out of imgui_state into this header.

#include <vector>

#include "types.h"

// The loaded tape's block index, rebuilt by tape_scan_blocks() on every tape
// load.  Entry N points at the Nth CDT block in pbTapeImage, in the same order
// the deck Device walks, so an ordinal here is an ordinal the deck can seek to.
struct HostTapeView {
  std::vector<byte*> block_offsets;
  int current_block = 0;  // the deck's block ordinal, mirrored each frame
};

extern HostTapeView g_host_tape;

// One sample of the frame/audio telemetry the main loop publishes.  The IPC
// 'metrics' command reports it.
struct HostFrameMetrics {
  float frame_time_avg_us = 0.0f;
  float display_time_avg_us = 0.0f;
  float z80_time_avg_us = 0.0f;
  float sleep_time_avg_us = 0.0f;
  float audio_queue_avg_ms = 0.0f;
  float audio_queue_min_ms = 0.0f;
  int audio_underruns = 0;
  int audio_near_underruns = 0;
};

// Consistent snapshot of the telemetry, taken under the stats lock.  Safe
// from any thread.
HostFrameMetrics host_frame_metrics();
