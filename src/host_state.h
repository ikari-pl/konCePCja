#pragma once

// UI-neutral state owned by the host runtime (beads-cv2.4, beads-cv2.5).
//
// Data the emulation side produces and both the modern UI and the IPC server
// read: frame/audio telemetry, the tape scopes and block index, the drive
// LEDs and the FPS text.  It used to live in imgui_state, which made the
// emulation loop, the bridge and the IPC server depend on a UI header.
// Nothing here knows about ImGui; the core build links it as is.

#include <string>
#include <vector>

#include "types.h"

// The loaded tape, as the host sees it.
//
// block_offsets is rebuilt by tape_scan_blocks() on every tape load.  Entry N
// points at the Nth CDT block in pbTapeImage, in the same order the deck
// Device walks, so an ordinal here is an ordinal the deck can seek to.
//
// The two scope rings are written by the frame runner (the Z80 thread, or the
// main thread under -H) and read by the render thread after the frame
// handoff, so the same happens-before that publishes the framebuffer covers
// them.  The UI clears the decoded ring when the tape is ejected.
struct HostTapeView {
  std::vector<byte*> block_offsets;
  int current_block = 0;  // the deck's block ordinal, mirrored each frame

  // RAW scope: the deck's output level, one sample per frame while it plays.
  static constexpr int kWaveSamples = 128;
  byte wave_buf[kWaveSamples] = {};
  int wave_head = 0;  // next slot to write == the oldest sample

  // BITS scope: the data bits the deck decoded.
  static constexpr int kDecodedSamples = 200;
  byte decoded_buf[kDecodedSamples] = {};
  int decoded_head = 0;
};

extern HostTapeView g_host_tape;

void host_tape_push_wave(byte level);
void host_tape_push_decoded(byte bit);
void host_tape_clear_decoded();

// The status-bar values the frame runner refreshes once per completed frame,
// before it signals the frame ready, so the render thread sees them after
// wait_ready() returns.
struct HostStatusView {
  bool drive_a_led = false;
  bool drive_b_led = false;
  std::string fps_text;  // "50FPS 100%", empty when CPC.scr_fps is off
};

extern HostStatusView g_host_status;

// One sample of the frame/audio telemetry the frame runner publishes once a
// second.  The IPC 'metrics' command and the --debug bar report it.
struct HostFrameMetrics {
  float frame_time_avg_us = 0.0f;
  float frame_time_min_us = 0.0f;
  float frame_time_max_us = 0.0f;
  float display_time_avg_us = 0.0f;
  float sleep_time_avg_us = 0.0f;
  float z80_time_avg_us = 0.0f;
  int audio_underruns = 0;       // SDL queue empty when we pushed
  int audio_near_underruns = 0;  // queue below one buffer
  int audio_pushes = 0;          // pushes this second
  float audio_queue_avg_ms = 0.0f;
  float audio_queue_min_ms = 0.0f;
  float audio_push_interval_max_us = 0.0f;  // longest gap between pushes
};

// Consistent snapshot of the telemetry, taken under the stats lock.  Safe
// from any thread.
HostFrameMetrics host_frame_metrics();
// Replace the telemetry under the stats lock.  The frame runner is the only
// writer, so it may read with host_frame_metrics(), change some fields and
// publish without losing an update.
void host_frame_metrics_publish(const HostFrameMetrics& m);
