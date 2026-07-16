/* konCePCja - Amstrad CPC Emulator
   Drive/Tape sound effects

   Audio effects for FDC and tape operations:
   - Disk motor: continuous loop while FDC motor is on
   - Disk head seek: click on track change
   - Tape loading: hiss during tape playback

   Sounds are generated procedurally (no WAV files needed).

   These are host-side *cosmetic* effects, physically decoupled from the
   emulated AY-3-8912: they are rendered into their OWN SDL audio stream
   (see drive_sounds_fill_stereo) and mixed by the SDL audio device, never
   summed into the AY sample buffer. The emulation core emits pure AY.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "types.h"

// Tunable knobs for the procedural synthesis. The defaults are the profile
// dialed in via the Drive Sound Lab (a deep, quiet motor bed and a low, slow
// seek thunk) — a fresh struct or "Reset to defaults" reproduces exactly that.
// Editing any field and calling drive_sounds_regenerate() re-bakes the affected
// buffer live. These are *generation* parameters only — the audio thread never
// reads them, it reads the baked buffers — so they may be edited freely from
// the UI/main thread. Live playback knobs (volume, pan) live on DriveSounds
// instead and need no re-bake.
struct DriveSoundParams {
  // Motor — looped spindle hum (fundamental + 2nd/3rd harmonic + rumble).
  float motor_len_s = 1.0f;      // loop length, seconds
  float motor_f0 = 43.6f;        // fundamental, Hz
  float motor_a0 = 0.40f;        // fundamental amplitude
  float motor_a1 = 0.67f;        // 2nd harmonic (2*f0) amplitude
  float motor_a2 = 0.122f;       // 3rd harmonic (3*f0) amplitude
  float motor_rumble = 0.243f;   // broadband rumble amount
  float motor_gain = 1778.0f;    // → int16 scale

  // Seek — one-shot head-step click (decaying tone + mechanical noise).
  float seek_len_ms = 69.0f;   // click length, ms
  float seek_freq = 208.0f;    // tone, Hz
  float seek_decay = 33.0f;    // envelope exp(-decay*t)
  float seek_noise = 0.111f;   // mechanical noise amount
  float seek_gain = 8000.0f;   // → int16 scale

  // Tape — looped hiss (white noise through a one-pole low-pass).
  float tape_len_s = 2.0f;    // loop length, seconds
  float tape_noise = 1.0f;    // white-noise amount (pre-gain)
  float tape_lpf = 0.70f;     // one-pole feedback coeff (0..0.99); fwd = 1-lpf
  float tape_gain = 2000.0f;  // → int16 scale
};

// Bitmask selecting which baked buffers drive_sounds_regenerate() rebuilds.
enum DriveSoundBuffer : std::uint8_t {
  DS_MOTOR = 1,
  DS_SEEK = 2,
  DS_TAPE = 4,
  DS_ALL = DS_MOTOR | DS_SEEK | DS_TAPE,
};

struct DriveSounds {
  bool disk_enabled = false;
  bool tape_enabled = false;

  // Tunable synthesis parameters (see DriveSoundParams).
  DriveSoundParams params;

  // Generated sample data (at 44100 Hz, resampled on output)
  std::vector<int16_t> motor_samples;
  std::vector<int16_t> seek_samples;
  std::vector<int16_t> tape_samples;

  // Playback state
  size_t motor_pos = 0;
  bool motor_playing = false;
  size_t seek_pos = 0;
  bool seek_playing = false;
  size_t tape_pos = 0;
  bool tape_playing = false;

  // Resampling state (source rate = 44100)
  double resample_ratio = 1.0;  // target_rate / 44100
  double motor_frac = 0.0;
  double tape_frac = 0.0;

  int volume = 40;       // 0-100, applied live (no re-bake)
  float pan_left = 0.80f;  // left-channel attenuation (right = 1.0), applied live
};

extern DriveSounds g_drive_sounds;

void drive_sounds_init(int target_sample_rate);
// Re-bake the selected buffers (DriveSoundBuffer mask) from g_drive_sounds.params.
// Thread-safe: holds the generator lock while swapping so the SDL audio thread
// never reads a torn buffer. Playback positions are clamped to the new sizes.
void drive_sounds_regenerate(unsigned which);
int16_t drive_sounds_next_sample();
// Render `frames` interleaved stereo (L,R) int16 samples of the current drive/
// tape effects into `out` (out must hold frames*2 int16s). Applies a slight
// right-pan bias. Thread-safe: locks internally so it can be called from SDL's
// audio thread while motor/seek/tape events arrive on the emulation thread.
void drive_sounds_fill_stereo(int16_t* out, int frames);
void drive_sounds_motor(bool on);
void drive_sounds_seek();
void drive_sounds_tape(bool playing);
void drive_sounds_register_hooks();

// Persist / restore the full tuning (params + live volume/pan) as a compact
// versioned CSV string, stored under [sound] drivesnd_params. from_string is
// tolerant of malformed/empty input (leaves the defaults in place) and does NOT
// re-bake — the caller regenerates (drive_sounds_init does this at startup).
std::string drive_sounds_params_to_string();
void drive_sounds_params_from_string(const std::string& s);
