#include "drive_sounds.h"
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "SDL3/SDL_mutex.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

DriveSounds g_drive_sounds;

static const int GEN_RATE = 44100;

// Serialises the generator's playback state between the SDL audio thread (which
// pulls samples via drive_sounds_fill_stereo) and the emulation/UI threads
// (which raise motor/seek/tape events). Created once in drive_sounds_init and
// intentionally never destroyed — a process-lifetime singleton avoids teardown
// races with the audio callback. Null before init: callers fall back to the
// unlocked single-threaded path, which is safe because no audio thread exists
// yet.
namespace {
SDL_Mutex* s_drive_mutex = nullptr;
}  // namespace

namespace {
inline void drive_sounds_lock() {
  if (s_drive_mutex) SDL_LockMutex(s_drive_mutex);
}
}  // namespace
namespace {
inline void drive_sounds_unlock() {
  if (s_drive_mutex) SDL_UnlockMutex(s_drive_mutex);
}
}  // namespace

namespace {
inline int16_t clamp_i16(double v) {
  if (v > 32767.0) return 32767;
  if (v < -32768.0) return -32768;
  return static_cast<int16_t>(v);
}
}  // namespace

// Generate procedural motor hum (looped) — fundamental + 2nd/3rd harmonic locked
// to 2*f0 / 3*f0, plus broadband rumble.
namespace {
void generate_motor_samples(std::vector<int16_t>& out,
                                   const DriveSoundParams& p) {
  int len = static_cast<int>(p.motor_len_s * GEN_RATE);
  len = std::max(len, 1);
  out.resize(len);
  for (int i = 0; i < len; i++) {
    double const t = static_cast<double>(i) / GEN_RATE;
    double v = (sin(2 * M_PI * p.motor_f0 * t) * p.motor_a0) +
               (sin(2 * M_PI * 2 * p.motor_f0 * t) * p.motor_a1) +
               (sin(2 * M_PI * 3 * p.motor_f0 * t) * p.motor_a2);
    // NOLINTNEXTLINE(misc-predictable-rand): rand() is intentional for emulator weak-bit/fuzz jitter, not security
    v += ((rand() / static_cast<double>(RAND_MAX)) - 0.5) * p.motor_rumble;
    out[i] = clamp_i16(v * p.motor_gain);
  }
}
}  // namespace

// Generate procedural seek click (one-shot) — decaying tone + mechanical noise.
namespace {
void generate_seek_samples(std::vector<int16_t>& out,
                                  const DriveSoundParams& p) {
  int len = static_cast<int>(p.seek_len_ms * GEN_RATE / 1000.0);
  len = std::max(len, 1);
  out.resize(len);
  for (int i = 0; i < len; i++) {
    double const t = static_cast<double>(i) / GEN_RATE;
    double const env = exp(-t * p.seek_decay);
    double v = sin(2 * M_PI * p.seek_freq * t) * env;
    // NOLINTNEXTLINE(misc-predictable-rand): rand() is intentional for emulator weak-bit/fuzz jitter, not security
    v += ((rand() / static_cast<double>(RAND_MAX)) - 0.5) * p.seek_noise * env;
    out[i] = clamp_i16(v * p.seek_gain);
  }
}
}  // namespace

// Generate procedural tape hiss (looped) — white noise through a one-pole
// low-pass: y[i] = (1-lpf)*x[i] + lpf*y[i-1].
namespace {
void generate_tape_samples(std::vector<int16_t>& out,
                                  const DriveSoundParams& p) {
  int len = static_cast<int>(p.tape_len_s * GEN_RATE);
  len = std::max(len, 1);
  out.resize(len);
  for (int i = 0; i < len; i++) {
    double const v =
        // NOLINTNEXTLINE(misc-predictable-rand): rand() is intentional for emulator weak-bit/fuzz jitter, not security
        ((rand() / static_cast<double>(RAND_MAX)) - 0.5) * p.tape_noise;
    out[i] = clamp_i16(v * p.tape_gain);
  }
  const double fwd = 1.0 - p.tape_lpf;
  for (int i = 1; i < len; i++) {
    out[i] = clamp_i16((out[i] * fwd) + (out[i - 1] * p.tape_lpf));
  }
}
}  // namespace

void drive_sounds_regenerate(unsigned which) {
  drive_sounds_lock();
  const DriveSoundParams& p = g_drive_sounds.params;
  if (which & DS_MOTOR) {
    generate_motor_samples(g_drive_sounds.motor_samples, p);
    if (g_drive_sounds.motor_pos >= g_drive_sounds.motor_samples.size())
      g_drive_sounds.motor_pos = 0;
  }
  if (which & DS_SEEK) {
    generate_seek_samples(g_drive_sounds.seek_samples, p);
    if (g_drive_sounds.seek_pos >= g_drive_sounds.seek_samples.size())
      g_drive_sounds.seek_pos = 0;
  }
  if (which & DS_TAPE) {
    generate_tape_samples(g_drive_sounds.tape_samples, p);
    if (g_drive_sounds.tape_pos >= g_drive_sounds.tape_samples.size())
      g_drive_sounds.tape_pos = 0;
  }
  drive_sounds_unlock();
}

void drive_sounds_init(int target_sample_rate) {
  if (!s_drive_mutex) {
    s_drive_mutex = SDL_CreateMutex();  // process-lifetime, never destroyed
  }
  g_drive_sounds.resample_ratio =
      static_cast<double>(target_sample_rate) / GEN_RATE;
  g_drive_sounds.motor_pos = 0;
  g_drive_sounds.seek_pos = 0;
  g_drive_sounds.tape_pos = 0;
  g_drive_sounds.motor_frac = 0.0;
  g_drive_sounds.tape_frac = 0.0;
  drive_sounds_regenerate(DS_ALL);
}

int16_t drive_sounds_next_sample() {
  int32_t mix = 0;
  double const vol = g_drive_sounds.volume / 100.0;

  if (g_drive_sounds.motor_playing && !g_drive_sounds.motor_samples.empty()) {
    mix += static_cast<int32_t>(
        g_drive_sounds.motor_samples[g_drive_sounds.motor_pos] * vol);
    // Advance with resampling
    g_drive_sounds.motor_frac += 1.0 / g_drive_sounds.resample_ratio;
    while (g_drive_sounds.motor_frac >= 1.0) {
      g_drive_sounds.motor_frac -= 1.0;
      g_drive_sounds.motor_pos++;
      if (g_drive_sounds.motor_pos >= g_drive_sounds.motor_samples.size()) {
        g_drive_sounds.motor_pos = 0;  // loop
      }
    }
  }

  if (g_drive_sounds.seek_playing && !g_drive_sounds.seek_samples.empty()) {
    mix += static_cast<int32_t>(
        g_drive_sounds.seek_samples[g_drive_sounds.seek_pos] * vol);
    g_drive_sounds.seek_pos++;
    if (g_drive_sounds.seek_pos >= g_drive_sounds.seek_samples.size()) {
      g_drive_sounds.seek_playing = false;
      g_drive_sounds.seek_pos = 0;
    }
  }

  if (g_drive_sounds.tape_playing && !g_drive_sounds.tape_samples.empty()) {
    mix += static_cast<int32_t>(
        g_drive_sounds.tape_samples[g_drive_sounds.tape_pos] * vol);
    g_drive_sounds.tape_frac += 1.0 / g_drive_sounds.resample_ratio;
    while (g_drive_sounds.tape_frac >= 1.0) {
      g_drive_sounds.tape_frac -= 1.0;
      g_drive_sounds.tape_pos++;
      if (g_drive_sounds.tape_pos >= g_drive_sounds.tape_samples.size()) {
        g_drive_sounds.tape_pos = 0;  // loop
      }
    }
  }

  // Clamp
  mix = std::min(mix, 32767);
  mix = std::max(mix, -32768);
  return static_cast<int16_t>(mix);
}

void drive_sounds_fill_stereo(int16_t* out, int frames) {
  // Called from SDL's audio thread; hold the lock for the whole chunk so
  // motor/seek/tape events on the emulation thread cannot tear the playback
  // state mid-render.
  drive_sounds_lock();
  const float pan_left = g_drive_sounds.pan_left;
  for (int i = 0; i < frames; i++) {
    int16_t const ds = drive_sounds_next_sample();
    // Right-pan bias (g_drive_sounds.pan_left, tunable in the Drive Sound Lab):
    // the left channel is attenuated relative to the right for the historical
    // drive-sound feel. pan_left == 1.0 is dead-centre.
    out[2 * i] = static_cast<int16_t>(ds * pan_left);
    out[(2 * i) + 1] = ds;
  }
  drive_sounds_unlock();
}

void drive_sounds_motor(bool on) {
  if (!g_drive_sounds.disk_enabled) return;
  drive_sounds_lock();
  g_drive_sounds.motor_playing = on;
  if (!on) {
    g_drive_sounds.motor_pos = 0;
    g_drive_sounds.motor_frac = 0.0;
  }
  drive_sounds_unlock();
}

void drive_sounds_seek() {
  if (!g_drive_sounds.disk_enabled) return;
  drive_sounds_lock();
  g_drive_sounds.seek_playing = true;
  g_drive_sounds.seek_pos = 0;
  drive_sounds_unlock();
}

void drive_sounds_tape(bool playing) {
  if (!g_drive_sounds.tape_enabled) return;
  drive_sounds_lock();
  g_drive_sounds.tape_playing = playing;
  if (!playing) {
    g_drive_sounds.tape_pos = 0;
    g_drive_sounds.tape_frac = 0.0;
  }
  drive_sounds_unlock();
}

// ── Tuning persistence ─────────────────────────
//
// The whole tuning (live volume/pan + all generation params) round-trips as one
// compact, versioned CSV token so it fits a single [sound] drivesnd_params key.
// Field order matches the "v1" layout below; bump the tag if it ever changes.

std::string drive_sounds_params_to_string() {
  const DriveSoundParams& p = g_drive_sounds.params;
  char buf[512];
  snprintf(buf, sizeof(buf),
           "v1,%d,%.4f,"
           "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.1f,"  // motor
           "%.2f,%.2f,%.2f,%.4f,%.1f,"            // seek
           "%.4f,%.4f,%.4f,%.1f",                 // tape
           g_drive_sounds.volume, g_drive_sounds.pan_left, p.motor_len_s,
           p.motor_f0, p.motor_a0, p.motor_a1, p.motor_a2, p.motor_rumble,
           p.motor_gain, p.seek_len_ms, p.seek_freq, p.seek_decay, p.seek_noise,
           p.seek_gain, p.tape_len_s, p.tape_noise, p.tape_lpf, p.tape_gain);
  return {buf};
}

void drive_sounds_params_from_string(const std::string& s) {
  if (s.empty()) return;
  std::vector<std::string> tok;
  size_t start = 0;
  while (start <= s.size()) {
    size_t const comma = s.find(',', start);
    if (comma == std::string::npos) {
      tok.push_back(s.substr(start));
      break;
    }
    tok.push_back(s.substr(start, comma - start));
    start = comma + 1;
  }
  // "v1" tag + 2 live + 7 motor + 5 seek + 4 tape = 19 tokens.
  if (tok.size() < 19 || tok[0] != "v1") return;
  auto flt = [&](int i) { return strtof(tok[i].c_str(), nullptr); };
  DriveSoundParams& p = g_drive_sounds.params;
  g_drive_sounds.volume = static_cast<int>(strtol(tok[1].c_str(), nullptr, 10));
  g_drive_sounds.pan_left = flt(2);
  p.motor_len_s = flt(3);
  p.motor_f0 = flt(4);
  p.motor_a0 = flt(5);
  p.motor_a1 = flt(6);
  p.motor_a2 = flt(7);
  p.motor_rumble = flt(8);
  p.motor_gain = flt(9);
  p.seek_len_ms = flt(10);
  p.seek_freq = flt(11);
  p.seek_decay = flt(12);
  p.seek_noise = flt(13);
  p.seek_gain = flt(14);
  p.tape_len_s = flt(15);
  p.tape_noise = flt(16);
  p.tape_lpf = flt(17);
  p.tape_gain = flt(18);
}

// ── I/O dispatch registration ──────────────────

#include "io_dispatch.h"

// Drive sounds have two independent enable flags (disk_enabled, tape_enabled).
// We use a single "always enabled" flag for the hooks and let the existing
// drive_sounds_motor/tape functions check their own enable flags internally.
namespace {
bool s_drive_sounds_always_enabled = true;
}  // namespace

void drive_sounds_register_hooks() {
  io_register_tape_motor_hook(drive_sounds_tape,
                              &s_drive_sounds_always_enabled);
  io_register_fdc_motor_hook(drive_sounds_motor,
                             &s_drive_sounds_always_enabled);
}
