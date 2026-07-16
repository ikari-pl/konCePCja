/* tape_line_in.cpp — see tape_line_in.h. */

#include "tape_line_in.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

#include "log.h"
#include "subcycle/machine.h"

namespace {

SDL_AudioStream* g_stream = nullptr;
int g_channel = 0;  // 0 = left, 1 = right, 2 = mix
int g_rate = 44100;

// The Schmitt stage: hysteresis around a slow-tracking DC baseline — the
// software mirror of the mainboard's input conditioning circuit.
struct Schmitt {
  float baseline = 0.0f;
  uint8_t level = 0;
  uint8_t step(int16_t s) {
    baseline += (static_cast<float>(s) - baseline) * 0.0005f;  // ~DC tracker
    const float x = static_cast<float>(s) - baseline;
    const float kHigh = 1200.0f, kLow = -1200.0f;  // ±~4% FS hysteresis
    if (level == 0 && x > kHigh)
      level = 1;
    else if (level == 1 && x < kLow)
      level = 0;
    return level;
  }
};
Schmitt g_schmitt;

SDL_AudioStream* g_out_stream = nullptr;
int g_out_data_ch = 0;
float g_out_gain = 0.0f;    // soft-start ramp 0 -> 1 over ~2 s
float g_out_volume = 0.35f;  // user level 0..1 ([sound] tape_data_volume)
float g_out_lp = 0.0f;       // de-click one-pole state (rounds the square edges)
float g_carrier_phase = 0.0f;

bool SDLCALL out_device_watch(void* userdata, SDL_Event* ev) {
  // ANY audio device change disarms the output instantly (cable-swap guard).
  if (ev->type == SDL_EVENT_AUDIO_DEVICE_ADDED ||
      ev->type == SDL_EVENT_AUDIO_DEVICE_REMOVED) {
    if (g_out_stream != nullptr) {
      SDL_DestroyAudioStream(g_out_stream);
      g_out_stream = nullptr;
      LOG_INFO("tape line-out: audio device changed - DISARMED");
    }
  }
  (void)userdata;
  return true;
}

}  // namespace

bool tape_line_out_arm(subcycle::Machine& machine, int data_channel,
                       bool source_rdata) {
  tape_line_out_disarm(machine);
  SDL_AudioSpec want{};
  want.format = SDL_AUDIO_S16LE;
  want.channels = 2;
  want.freq = g_rate;
  g_out_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                           &want, nullptr, nullptr);
  if (g_out_stream == nullptr) {
    LOG_ERROR("tape line-out: no playback device: " << SDL_GetError());
    return false;
  }
  SDL_ResumeAudioStreamDevice(g_out_stream);
  SDL_AddEventWatch(out_device_watch, nullptr);
  g_out_data_ch = data_channel ? 1 : 0;
  g_out_gain = 0.0f;
  g_out_lp = 0.0f;
  g_carrier_phase = 0.0f;
  machine.tape_out_capture(true, source_rdata);
  LOG_INFO("tape line-out: ARMED (data="
           << (g_out_data_ch ? "right" : "left")
           << (source_rdata ? ", source=rdata" : ", source=wdata")
           << "), ramping from silence");
  return true;
}

void tape_line_out_disarm(subcycle::Machine& machine) {
  machine.tape_out_capture(false, true);
  if (g_out_stream != nullptr) {
    SDL_DestroyAudioStream(g_out_stream);
    g_out_stream = nullptr;
  }
  SDL_RemoveEventWatch(out_device_watch, nullptr);
}

bool tape_line_out_active() { return g_out_stream != nullptr; }

void tape_line_out_set_volume(float level) {
  // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): nested conditional kept intentionally; no clang-tidy auto-fix
  g_out_volume = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
}
float tape_line_out_volume() { return g_out_volume; }

// Emulation speed, published by z80_thread_main (50 = realtime, higher = turbo).
extern std::uint32_t dwFPS;

void tape_line_out_pump(subcycle::Machine& machine) {
  if (g_out_stream == nullptr) return;
  const std::vector<uint8_t>& wires = machine.tape_out_samples();
  if (wires.empty()) return;

  // Play the tape at the EMULATION's speed. The wires are sampled at 44.1 kHz of
  // EMULATED time, so 1x is realtime; in turbo the emulation produces them N×
  // faster than the 44.1 kHz sink drains. Decimate by the live speed (dwFPS/50)
  // — keep ~every Nth sample — so the tone pitches up ×N with the emulation (a
  // deck on fast-forward, as asked) AND the output naturally rate-matches
  // ~44.1 kHz real, so there is no backlog. N≈1 at realtime leaves it untouched.
  const double speed = dwFPS > 50u ? dwFPS / 50.0 : 1.0;

  // Backstop: before dwFPS is first published (~1 s) speed reads 1, so cap the
  // queue at ~200 ms and drop the batch if it's already full — never backlog.
  const int kMaxQueuedBytes =
      g_rate * 2 * static_cast<int>(sizeof(int16_t)) / 5;  // stereo s16, ~200 ms
  if (SDL_GetAudioStreamQueued(g_out_stream) >= kMaxQueuedBytes) return;

  static std::vector<int16_t> frames;
  frames.clear();
  const float kCarrierStep = 2.0f * 3.14159265f * 19000.0f / 44100.0f;
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  static double decim_acc = 0.0;
  for (unsigned char const wire : wires) {
    decim_acc += 1.0;
    if (decim_acc < speed) continue;  // decimate: skip until we've covered `speed`
    decim_acc -= speed;

    g_out_gain += (1.0f - g_out_gain) * 0.00002f;  // ~2 s soft-start (per emitted)
    const bool data = (wire & 1) != 0;
    const bool motor = (wire & 2) != 0;
    const float amp = 24000.0f * g_out_gain * g_out_volume;
    const float d_target = data ? amp : -amp;
    // De-click only near realtime: the full-scale square's hard edges click on
    // sparse transitions. In turbo the transitions are dense (a continuous high
    // buzz, no discrete clicks) and the low-pass would kill the pitched-up
    // brightness, so pass the square straight through.
    if (speed < 2.0)
      g_out_lp += (d_target - g_out_lp) * 0.35f;
    else
      g_out_lp = d_target;
    const int16_t d = static_cast<int16_t>(g_out_lp);
    int16_t carrier = 0;
    if (motor) {
      carrier = static_cast<int16_t>(SDL_sinf(g_carrier_phase) * amp);
      g_carrier_phase += kCarrierStep;
      if (g_carrier_phase > 6.2831853f) g_carrier_phase -= 6.2831853f;
    }
    frames.push_back(g_out_data_ch == 0 ? d : carrier);  // left channel
    frames.push_back(g_out_data_ch == 0 ? carrier : d);  // right channel
  }
  if (!frames.empty())
    SDL_PutAudioStreamData(g_out_stream, frames.data(),
                           static_cast<int>(frames.size() * sizeof(int16_t)));
}

bool tape_line_in_start(int channel) {
  tape_line_in_stop();
  SDL_AudioSpec want{};
  want.format = SDL_AUDIO_S16LE;
  want.channels = 2;
  want.freq = g_rate;
  g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING,
                                       &want, nullptr, nullptr);
  if (g_stream == nullptr) {
    LOG_ERROR("tape line-in: no recording device: " << SDL_GetError());
    return false;
  }
  SDL_ResumeAudioStreamDevice(g_stream);
  g_channel = channel;
  g_schmitt = Schmitt{};
  LOG_INFO("tape line-in: recording (channel " << (channel == 0   ? "left"
                                                   : channel == 1 ? "right"
                                                                  : "mix")
                                               << ")");
  return true;
}

void tape_line_in_stop() {
  if (g_stream != nullptr) {
    SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
  }
}

bool tape_line_in_active() { return g_stream != nullptr; }

void tape_line_in_pump(subcycle::Machine& machine) {
  if (g_stream == nullptr) return;
  int16_t buf[2048 * 2];
  std::vector<uint8_t> levels;
  int got;
  while ((got = SDL_GetAudioStreamData(g_stream, buf, sizeof(buf))) > 0) {
    const int frames = got / (2 * static_cast<int>(sizeof(int16_t)));
    for (int i = 0; i < frames; ++i) {
      const int16_t left = buf[i * 2];
      const int16_t right = buf[(i * 2) + 1];
      const int16_t s =
          g_channel == 0 ? left
          // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): nested conditional kept intentionally; no clang-tidy auto-fix
          : g_channel == 1
              ? right
              : static_cast<int16_t>((static_cast<int>(left) + right) / 2);
      levels.push_back(g_schmitt.step(s));
    }
    if (got < static_cast<int>(sizeof(buf))) break;
  }
  if (!levels.empty())
    machine.feed_line_levels(levels.data(), static_cast<int>(levels.size()),
                             g_rate);
}
