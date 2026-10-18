#pragma once

#include <cstdint>

// The 50 Hz real-time deadline that paces the emulation (both loops, through
// subcycle_bridge_frame). Pure clock arithmetic, so it is unit-testable with a
// synthetic clock: the caller reads the clock, asks arrive() when to release
// the frame, and sleeps until then.
//
// The deadline is drift-corrected: it advances by exactly one period per
// frame, so a frame that ran (or slept) long is made up by the frames after
// it. A loop more than a quarter second behind -- a stalled host -- resyncs to
// "now" instead of racing to catch up. Time the machine spent stopped is not
// owed at all, however short the stop: see stopped().
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
    bool const resync = next_deadline_ == 0 ||
                        now > next_deadline_ + (freq / 4) ||
                        (stopped_ && now > next_deadline_);
    stopped_ = false;
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
    stopped_ = false;
  }

  // The machine was stopped (a pause, a breakpoint, the end of a step) since
  // the last frame. The frames it would have run meanwhile are not made up:
  // if the next frame arrives past its deadline, pacing restarts there.
  // Without this a stop shorter than the quarter second was repaid as a
  // burst of unpaced frames (beads-drzi). A stop that ends before the
  // deadline costs nothing and keeps the grid: it never releases a frame
  // early.
  void stopped() { stopped_ = true; }

  // The last arrive() found the emulation more than a period behind.
  bool late() const { return late_; }

 private:
  uint64_t next_deadline_ = 0;
  bool late_ = false;
  bool stopped_ = false;
};

// Whether the frame runner should be a pacing thread (host_pacing_thread,
// beads-fijd): while it runs paced frames that leave it time to sleep. A
// macOS host that coalesces an ordinary thread's timers ends the 19 ms sleep
// to the deadline up to 140 ms late; a pacing thread is woken on time.
//
// An unpaced frame never sleeps, and a frame that computes for a whole period
// means the runner cannot keep up and would never block: the OS demotes a
// time-constraint thread that does that, and it crowds out other threads
// meanwhile. Such a frame turns pacing off, and kRetryFrames frames that fit
// turn it back on. Every tier fits on a healthy host: Faithful computes about
// 14 ms of the 20. Pure, like FramePacer: the caller measures the frame.
class PacingThreadRule {
 public:
  static constexpr uint32_t kPeriodUs = 20000;
  static constexpr unsigned kRetryFrames = 50;

  // A frame that computed for `busy_us` has finished; `paced` says whether it
  // is about to sleep to its deadline. Returns whether the runner should be a
  // pacing thread for that sleep.
  bool frame(bool paced, uint64_t busy_us) {
    if (busy_us >= kPeriodUs) {
      fit_ = 0;
    } else if (fit_ < kRetryFrames) {
      ++fit_;
    }
    return paced && fit_ >= kRetryFrames;
  }

 private:
  unsigned fit_ = kRetryFrames;
};
