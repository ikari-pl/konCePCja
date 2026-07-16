// Drive Sound Lab — synthesis parameter persistence & generation invariants.
//
// The Drive Sound Lab (devtools) edits DriveSoundParams live and persists the
// whole tuning as one CSV token under [sound] drivesnd_params. The two things
// that can silently corrupt a user's saved sound are (a) the serialize field
// order drifting out of sync with the struct, and (b) the built-in defaults
// changing (which would alter the out-of-the-box drive sound). Both are guarded
// here, plus the buffer lengths the generators must produce.

#include "drive_sounds.h"

#include "gtest/gtest.h"

namespace {

// Source rate the generators bake at (drive_sounds.cpp GEN_RATE).
constexpr int kGenRate = 44100;

// A distinctive, non-default tuning whose every field is exactly representable
// at the precisions drive_sounds_params_to_string() prints (so the round-trip
// is loss-free and we can compare tightly).
void set_distinctive(DriveSoundParams& p) {
  p.motor_len_s = 1.25f;
  p.motor_f0 = 73.0f;
  p.motor_a0 = 0.6f;
  p.motor_a1 = 0.3f;
  p.motor_a2 = 0.15f;
  p.motor_rumble = 0.2f;
  p.motor_gain = 5000.0f;
  p.seek_len_ms = 42.0f;
  p.seek_freq = 1234.0f;
  p.seek_decay = 60.0f;
  p.seek_noise = 0.45f;
  p.seek_gain = 9000.0f;
  p.tape_len_s = 1.5f;
  p.tape_noise = 0.8f;
  p.tape_lpf = 0.6f;
  p.tape_gain = 3000.0f;
}

}  // namespace

// Guards the out-of-the-box drive sound: a fresh DriveSoundParams must hold the
// profile dialed in via the Drive Sound Lab. If someone tweaks a default, this
// fails on purpose — the default drive sound is a user-facing decision, not an
// accident.
TEST(DriveSounds, DefaultsMatchTunedProfile) {
  DriveSoundParams p;  // default-constructed
  EXPECT_FLOAT_EQ(p.motor_len_s, 1.0f);
  EXPECT_FLOAT_EQ(p.motor_f0, 43.6f);
  EXPECT_FLOAT_EQ(p.motor_a0, 0.40f);
  EXPECT_FLOAT_EQ(p.motor_a1, 0.67f);
  EXPECT_FLOAT_EQ(p.motor_a2, 0.122f);
  EXPECT_FLOAT_EQ(p.motor_rumble, 0.243f);
  EXPECT_FLOAT_EQ(p.motor_gain, 1778.0f);
  EXPECT_FLOAT_EQ(p.seek_len_ms, 69.0f);
  EXPECT_FLOAT_EQ(p.seek_freq, 208.0f);
  EXPECT_FLOAT_EQ(p.seek_decay, 33.0f);
  EXPECT_FLOAT_EQ(p.seek_noise, 0.111f);
  EXPECT_FLOAT_EQ(p.seek_gain, 8000.0f);
  EXPECT_FLOAT_EQ(p.tape_len_s, 2.0f);
  EXPECT_FLOAT_EQ(p.tape_noise, 1.0f);
  EXPECT_FLOAT_EQ(p.tape_lpf, 0.70f);
  EXPECT_FLOAT_EQ(p.tape_gain, 2000.0f);

  DriveSounds d;  // live playback defaults
  EXPECT_EQ(d.volume, 40);
  EXPECT_FLOAT_EQ(d.pan_left, 0.80f);
}

// to_string -> from_string must reconstruct every field. Catches the CSV field
// order drifting out of sync with the struct.
TEST(DriveSounds, ParamsRoundTrip) {
  set_distinctive(g_drive_sounds.params);
  g_drive_sounds.volume = 55;
  g_drive_sounds.pan_left = 0.5f;

  const std::string blob = drive_sounds_params_to_string();

  // Wipe to defaults, then restore from the serialized blob.
  g_drive_sounds.params = DriveSoundParams{};
  g_drive_sounds.volume = 0;
  g_drive_sounds.pan_left = 1.0f;
  drive_sounds_params_from_string(blob);

  const DriveSoundParams& p = g_drive_sounds.params;
  EXPECT_EQ(g_drive_sounds.volume, 55);
  EXPECT_NEAR(g_drive_sounds.pan_left, 0.5f, 1e-4);
  EXPECT_NEAR(p.motor_len_s, 1.25f, 1e-4);
  EXPECT_NEAR(p.motor_f0, 73.0f, 1e-3);
  EXPECT_NEAR(p.motor_a0, 0.6f, 1e-4);
  EXPECT_NEAR(p.motor_a1, 0.3f, 1e-4);
  EXPECT_NEAR(p.motor_a2, 0.15f, 1e-4);
  EXPECT_NEAR(p.motor_rumble, 0.2f, 1e-4);
  EXPECT_NEAR(p.motor_gain, 5000.0f, 0.1);
  EXPECT_NEAR(p.seek_len_ms, 42.0f, 0.01);
  EXPECT_NEAR(p.seek_freq, 1234.0f, 0.01);
  EXPECT_NEAR(p.seek_decay, 60.0f, 0.01);
  EXPECT_NEAR(p.seek_noise, 0.45f, 1e-4);
  EXPECT_NEAR(p.seek_gain, 9000.0f, 0.1);
  EXPECT_NEAR(p.tape_len_s, 1.5f, 1e-4);
  EXPECT_NEAR(p.tape_noise, 0.8f, 1e-4);
  EXPECT_NEAR(p.tape_lpf, 0.6f, 1e-4);
  EXPECT_NEAR(p.tape_gain, 3000.0f, 0.1);
}

// Malformed / empty input must be a no-op (leave the current tuning intact),
// never a partial-parse or crash — a corrupt config line can't wipe the sound.
TEST(DriveSounds, FromStringRejectsMalformed) {
  set_distinctive(g_drive_sounds.params);
  const float f0_before = g_drive_sounds.params.motor_f0;

  drive_sounds_params_from_string("");              // empty
  drive_sounds_params_from_string("not,a,tuning");  // wrong tag, too short
  drive_sounds_params_from_string("v1,1,2,3");      // right tag, too few fields

  EXPECT_FLOAT_EQ(g_drive_sounds.params.motor_f0, f0_before);
}

// Regenerate must bake buffers whose lengths follow the length knobs, and
// honour the DriveSoundBuffer mask (only the selected buffer changes).
TEST(DriveSounds, RegenerateHonoursLengthsAndMask) {
  g_drive_sounds.params = DriveSoundParams{};
  g_drive_sounds.params.motor_len_s = 0.5f;    // -> 22050
  g_drive_sounds.params.seek_len_ms = 100.0f;  // -> 4410
  g_drive_sounds.params.tape_len_s = 1.0f;     // -> 44100
  drive_sounds_regenerate(DS_ALL);

  EXPECT_EQ(g_drive_sounds.motor_samples.size(),
            static_cast<size_t>(0.5f * kGenRate));
  EXPECT_EQ(g_drive_sounds.seek_samples.size(),
            static_cast<size_t>(100.0f * kGenRate / 1000.0));
  EXPECT_EQ(g_drive_sounds.tape_samples.size(), static_cast<size_t>(kGenRate));

  // Change only the seek length and rebake only DS_SEEK: motor/tape untouched.
  const size_t motor_before = g_drive_sounds.motor_samples.size();
  g_drive_sounds.params.seek_len_ms = 50.0f;  // -> 2205
  drive_sounds_regenerate(DS_SEEK);
  EXPECT_EQ(g_drive_sounds.seek_samples.size(),
            static_cast<size_t>(50.0f * kGenRate / 1000.0));
  EXPECT_EQ(g_drive_sounds.motor_samples.size(), motor_before);
}
