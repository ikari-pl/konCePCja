#include "drive_sounds.h"
#include <cmath>
#include <cstdlib>

DriveSounds g_drive_sounds;

static const int GEN_RATE = 44100;

// Generate procedural motor hum (~1 second loop at GEN_RATE)
static void generate_motor_samples(std::vector<int16_t>& out)
{
   int len = GEN_RATE;  // 1 second
   out.resize(len);
   for (int i = 0; i < len; i++) {
      double t = static_cast<double>(i) / GEN_RATE;
      // Low-frequency hum with harmonics (fundamental ~50 Hz motor)
      double v = sin(2 * M_PI * 50 * t) * 0.4
               + sin(2 * M_PI * 100 * t) * 0.2
               + sin(2 * M_PI * 150 * t) * 0.1;
      // Add slight rumble noise
      v += (rand() / static_cast<double>(RAND_MAX) - 0.5) * 0.1;
      out[i] = static_cast<int16_t>(v * 4000);
   }
}

// Generate procedural seek click (~50ms)
static void generate_seek_samples(std::vector<int16_t>& out)
{
   int len = GEN_RATE / 20;  // 50ms
   out.resize(len);
   for (int i = 0; i < len; i++) {
      double t = static_cast<double>(i) / GEN_RATE;
      double env = exp(-t * 80);  // sharp decay
      double v = sin(2 * M_PI * 800 * t) * env;  // click with high-frequency component
      v += (rand() / static_cast<double>(RAND_MAX) - 0.5) * 0.3 * env; // mechanical noise
      out[i] = static_cast<int16_t>(v * 8000);
   }
}

// Generate procedural tape hiss (~2 second loop)
static void generate_tape_samples(std::vector<int16_t>& out)
{
   int len = GEN_RATE * 2;  // 2 seconds
   out.resize(len);
   for (int i = 0; i < len; i++) {
      // Filtered white noise (tape hiss character)
      double v = (rand() / static_cast<double>(RAND_MAX) - 0.5);
      out[i] = static_cast<int16_t>(v * 2000);
   }
   // Simple low-pass filter pass
   for (int i = 1; i < len; i++) {
      out[i] = static_cast<int16_t>(out[i] * 0.3 + out[i - 1] * 0.7);
   }
}

void drive_sounds_init(int target_sample_rate)
{
   generate_motor_samples(g_drive_sounds.motor_samples);
   generate_seek_samples(g_drive_sounds.seek_samples);
   generate_tape_samples(g_drive_sounds.tape_samples);

   g_drive_sounds.resample_ratio = static_cast<double>(target_sample_rate) / GEN_RATE;
   g_drive_sounds.motor_pos = 0;
   g_drive_sounds.seek_pos = 0;
   g_drive_sounds.tape_pos = 0;
   g_drive_sounds.motor_frac = 0.0;
   g_drive_sounds.tape_frac = 0.0;
}

int16_t drive_sounds_next_sample()
{
   int32_t mix = 0;
   double vol = g_drive_sounds.volume / 100.0;

   if (g_drive_sounds.motor_playing && !g_drive_sounds.motor_samples.empty()) {
      mix += static_cast<int32_t>(g_drive_sounds.motor_samples[g_drive_sounds.motor_pos] * vol);
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
      mix += static_cast<int32_t>(g_drive_sounds.seek_samples[g_drive_sounds.seek_pos] * vol);
      g_drive_sounds.seek_pos++;
      if (g_drive_sounds.seek_pos >= g_drive_sounds.seek_samples.size()) {
         g_drive_sounds.seek_playing = false;
         g_drive_sounds.seek_pos = 0;
      }
   }

   if (g_drive_sounds.tape_playing && !g_drive_sounds.tape_samples.empty()) {
      mix += static_cast<int32_t>(g_drive_sounds.tape_samples[g_drive_sounds.tape_pos] * vol);
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
   if (mix > 32767) mix = 32767;
   if (mix < -32768) mix = -32768;
   return static_cast<int16_t>(mix);
}

void drive_sounds_motor(bool on)
{
   if (!g_drive_sounds.disk_enabled) return;
   g_drive_sounds.motor_playing = on;
   if (!on) {
      g_drive_sounds.motor_pos = 0;
      g_drive_sounds.motor_frac = 0.0;
   }
}

void drive_sounds_seek()
{
   if (!g_drive_sounds.disk_enabled) return;
   g_drive_sounds.seek_playing = true;
   g_drive_sounds.seek_pos = 0;
}

void drive_sounds_tape(bool playing)
{
   if (!g_drive_sounds.tape_enabled) return;
   g_drive_sounds.tape_playing = playing;
   if (!playing) {
      g_drive_sounds.tape_pos = 0;
      g_drive_sounds.tape_frac = 0.0;
   }
}
