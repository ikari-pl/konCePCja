#pragma once

#include <cstdint>

// The 50 Hz real-time deadline that paces the emulation (both loops, through
// subcycle_bridge_frame). Pure clock arithmetic, so it is unit-testable with a
// synthetic clock: the caller reads the clock, asks arrive() when to release
// the frame, and sleeps until then.
//
// The deadline is drift-corrected: it advances by exactly one period per
// frame, so a frame that ran (or slept) long is made up by the frames after
// it. A loop more than a quarter second behind -- a pause, a debugger stop, a
// stalled host -- resyncs to "now" instead of racing to catch up.
class FramePacer {
 public:
  // A frame has finished at `now` (perf-counter ticks, `freq` per second).
  // Returns the tick to release it at (<= now: release immediately), and
  // records whether it arrived late.
  //
  // Late means more than a whole period behind its deadline. Anything less is
  // jitter the drift correction absorbs on the next frame -- typically the
  // sleep itself overshooting on a busy host, which no amount of skipped
  // rendering wins back. Only a machine that cannot keep up falls a period
  // behind, and that is when auto frameskip should drop renders.
  uint64_t arrive(uint64_t now, uint64_t freq) {
    uint64_t const period = freq / 50;
    bool const resync =
        next_deadline_ == 0 || now > next_deadline_ + (freq / 4);
    // A resync is a restart, not lateness: after a pause the stale deadline
    // would otherwise call the first frame back late.
    late_ = !resync && now > next_deadline_ + period;
    if (resync) next_deadline_ = now;
    uint64_t const release = next_deadline_;
    next_deadline_ += period;
    return release;
  }

  // An unpaced frame: forget the deadline, so pacing restarts cleanly.
  void unpaced() {
    next_deadline_ = 0;
    late_ = false;
  }

  // The last arrive() found the emulation more than a period behind.
  bool late() const { return late_; }

 private:
  uint64_t next_deadline_ = 0;
  bool late_ = false;
};
