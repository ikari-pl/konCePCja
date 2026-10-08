// Host state (beads-cv2.5): the telemetry, tape scopes and status-bar values
// the emulation loop publishes and the UI / IPC read.  None of it may depend
// on the UI header; this test links against host_state.cpp alone for the API.

#include "host_state.h"

#include <gtest/gtest.h>

namespace {

class HostStateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_metrics_ = host_frame_metrics();
    saved_tape_ = g_host_tape;
    saved_status_ = g_host_status;
  }
  void TearDown() override {
    host_frame_metrics_publish(saved_metrics_);
    g_host_tape = saved_tape_;
    g_host_status = saved_status_;
  }

 private:
  HostFrameMetrics saved_metrics_;
  HostTapeView saved_tape_;
  HostStatusView saved_status_;
};

TEST_F(HostStateTest, MetricsPublishRoundTripsEveryField) {
  HostFrameMetrics m;
  m.frame_time_avg_us = 20000.5f;
  m.frame_time_min_us = 19000.0f;
  m.frame_time_max_us = 21000.0f;
  m.display_time_avg_us = 1200.0f;
  m.sleep_time_avg_us = 15000.0f;
  m.z80_time_avg_us = 3800.0f;
  m.audio_underruns = 3;
  m.audio_near_underruns = 7;
  m.audio_pushes = 50;
  m.audio_queue_avg_ms = 42.5f;
  m.audio_queue_min_ms = 12.25f;
  m.audio_push_interval_max_us = 21500.0f;
  host_frame_metrics_publish(m);

  HostFrameMetrics const got = host_frame_metrics();
  EXPECT_FLOAT_EQ(got.frame_time_avg_us, 20000.5f);
  EXPECT_FLOAT_EQ(got.frame_time_min_us, 19000.0f);
  EXPECT_FLOAT_EQ(got.frame_time_max_us, 21000.0f);
  EXPECT_FLOAT_EQ(got.display_time_avg_us, 1200.0f);
  EXPECT_FLOAT_EQ(got.sleep_time_avg_us, 15000.0f);
  EXPECT_FLOAT_EQ(got.z80_time_avg_us, 3800.0f);
  EXPECT_EQ(got.audio_underruns, 3);
  EXPECT_EQ(got.audio_near_underruns, 7);
  EXPECT_EQ(got.audio_pushes, 50);
  EXPECT_FLOAT_EQ(got.audio_queue_avg_ms, 42.5f);
  EXPECT_FLOAT_EQ(got.audio_queue_min_ms, 12.25f);
  EXPECT_FLOAT_EQ(got.audio_push_interval_max_us, 21500.0f);
}

TEST_F(HostStateTest, WaveRingWrapsAndHeadPointsAtOldest) {
  g_host_tape.wave_head = 0;
  for (int i = 0; i < HostTapeView::kWaveSamples + 3; ++i) {
    host_tape_push_wave(static_cast<byte>(i & 1));
  }
  // 131 pushes into 128 slots: the head sits on slot 3, the oldest sample.
  EXPECT_EQ(g_host_tape.wave_head, 3);
  EXPECT_EQ(g_host_tape.wave_buf[2], 0);  // sample 130
  EXPECT_EQ(g_host_tape.wave_buf[0], 0);  // sample 128
  EXPECT_EQ(g_host_tape.wave_buf[1], 1);  // sample 129
}

TEST_F(HostStateTest, DecodedRingWrapsAndClears) {
  host_tape_clear_decoded();
  EXPECT_EQ(g_host_tape.decoded_head, 0);
  for (int i = 0; i < HostTapeView::kDecodedSamples + 1; ++i) {
    host_tape_push_decoded(1);
  }
  EXPECT_EQ(g_host_tape.decoded_head, 1);
  EXPECT_EQ(g_host_tape.decoded_buf[HostTapeView::kDecodedSamples - 1], 1);

  host_tape_clear_decoded();
  EXPECT_EQ(g_host_tape.decoded_head, 0);
  for (byte const b : g_host_tape.decoded_buf) EXPECT_EQ(b, 0);
}

TEST_F(HostStateTest, StatusViewStartsDark) {
  HostStatusView const fresh;
  EXPECT_FALSE(fresh.drive_a_led);
  EXPECT_FALSE(fresh.drive_b_led);
  EXPECT_TRUE(fresh.fps_text.empty());
}

}  // namespace
