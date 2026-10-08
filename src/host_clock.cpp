#include "host_clock.h"

#ifdef KONCPC_SDL

#include <SDL3/SDL_timer.h>

uint64_t host_ticks_ms() { return SDL_GetTicks(); }
uint64_t host_perf_counter() { return SDL_GetPerformanceCounter(); }
uint64_t host_perf_frequency() { return SDL_GetPerformanceFrequency(); }
void host_delay_ms(uint32_t ms) { SDL_Delay(ms); }

#else

#include <chrono>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;

// SDL_GetTicks counts from SDL_Init; this counts from the first reading, which
// the startup path takes before the loop starts.
const Clock::time_point& clock_origin() {
  static const Clock::time_point origin = Clock::now();
  return origin;
}
}  // namespace

uint64_t host_ticks_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            clock_origin())
          .count());
}

uint64_t host_perf_counter() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

uint64_t host_perf_frequency() { return 1000000000ULL; }

void host_delay_ms(uint32_t ms) {
  if (ms == 0) {
    std::this_thread::yield();
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#endif
