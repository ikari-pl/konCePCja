#pragma once

// Host clock for the runtime around the board (beads-29zx).
//
// The emulation loop, the speed limiter and the frame telemetry need a
// monotonic clock and a sleep.  The GUI build keeps SDL's (SDL_GetTicks,
// SDL_GetPerformanceCounter, SDL_Delay), so its pacing is exactly what it was.
// The SDL-free build (KONCPC_MODERN_UI=0, the `koncepcja -H` binary) uses
// std::chrono::steady_clock and std::this_thread::sleep_for instead.
//
// Only differences between readings are meaningful: the epoch is unspecified
// and differs between the two builds.

#include <cstdint>

// Milliseconds since an arbitrary point at or before the first call.
uint64_t host_ticks_ms();

// A high-resolution counter and its rate in counts per second.
uint64_t host_perf_counter();
uint64_t host_perf_frequency();

// Sleep for at least `ms` milliseconds (0 yields).
void host_delay_ms(uint32_t ms);
