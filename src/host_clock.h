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

// Whether the calling thread is a frame pacer: one that runs a frame, then
// sleeps to the frame's 50 Hz deadline with host_delay_ms(). `on` asks the OS
// to wake it on time from those sleeps; off makes it an ordinary thread again.
// `period_us` is the frame period and `busy_us` the most a frame is expected
// to compute in it. Cheap to call, but meant for changes only: the bridge calls
// it when pacing starts or stops (beads-fijd).
//
// macOS coalesces the timers of a process it treats as background (a headless
// core, one started from a script or a terminal that is not in front) by up
// to 32 times the interval, capped at 100 ms: a 19 ms sleep was measured
// returning after 160 ms. A time-constraint thread is woken on time (0.01 ms
// late). Linux: the thread's timer slack is set to its minimum (prctl, no
// privileges). Elsewhere it does nothing: SDL_Delay on Windows already uses a
// high-resolution waitable timer.
void host_pacing_thread(bool on, uint32_t period_us, uint32_t busy_us);
