/* konCePCja - Amstrad CPC Emulator
   Drive/Tape sound effects

   Audio effects for FDC and tape operations:
   - Disk motor: continuous loop while FDC motor is on
   - Disk head seek: click on track change
   - Tape loading: hiss during tape playback

   Sounds are generated procedurally (no WAV files needed).
   Mixed into the main PSG audio buffer at the synthesizer output stage.
*/

#ifndef DRIVE_SOUNDS_H
#define DRIVE_SOUNDS_H

#include "types.h"
#include <vector>
#include <cstdint>

struct DriveSounds {
   bool disk_enabled = false;
   bool tape_enabled = false;

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

   int volume = 40;  // 0-100
};

extern DriveSounds g_drive_sounds;

void drive_sounds_init(int target_sample_rate);
int16_t drive_sounds_next_sample();
void drive_sounds_motor(bool on);
void drive_sounds_seek();
void drive_sounds_tape(bool playing);

#endif
