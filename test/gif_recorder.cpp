#include "gif_recorder.h"

#include <gtest/gtest.h>

TEST(GifRecorderTest, DestructorWithoutRecordingDoesNotCrash) {
  // A GifRecorder that was never started should destroy cleanly.
  GifRecorder gif;
  EXPECT_FALSE(gif.is_recording());
  // Destructor called here — should not crash.
}

TEST(GifRecorderTest, AbortWithoutRecordingDoesNotCrash) {
  GifRecorder gif;
  gif.abort();  // no-op when not recording
  EXPECT_FALSE(gif.is_recording());
}

TEST(GifRecorderTest, FrameCountStartsAtZero) {
  GifRecorder gif;
  EXPECT_EQ(gif.frame_count(), 0);
}
