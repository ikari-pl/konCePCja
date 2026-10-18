#pragma once

// KCPC machine gate: how a socket thread reads the machine.
//
// The contract is docs/protocol/kcpc-binary-v1.md, section 8, seam 2. Nobody
// may read machine state while a frame is running, so a read is run at one of
// the two moments when none is:
//
//   - at a frame boundary, by the frame runner itself. The reader thread
//     queues the read and the runner serves it from service_frame(), which
//     the shared frame epilogue calls once per completed frame. This is the
//     path of a live view: it never pauses the machine or the audio.
//   - while the machine is stopped, by the reader thread, which holds it
//     stopped for the length of the copy. No boundary comes on a paused
//     machine, and none is needed: the runner is idle.
//   - under a pause lease, by the reader thread. This is the fallback, for a
//     read that has waited fallback_ms and got neither of the above: a runner
//     that turns out no frames, or a lease that holds the machine that long.
//
// Each queued read has an atomic state, queued -> claimed -> done, and
// whichever side claims it runs it, so it runs exactly once.
//
// The gate knows nothing about the emulator: the pause state and the lease
// come through GateHost, so test_runner drives it with a fake machine.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

#include "kcpc/kcpc_proto.h"

namespace kcpc {

enum class GateResult : std::uint8_t {
  kDone,          // the read ran, once
  kBusy,          // the runner did not go idle for the lease; nothing ran
  kShuttingDown,  // the gate is closed; nothing ran
};

struct GateHost {
  // True while the machine is paused, whether somebody stopped it or a pause
  // lease is holding it. Called from reader threads; must not block.
  std::function<bool()> paused;
  // Calls work() on the calling thread if the machine is stopped and nobody's
  // lease holds it, keeping it stopped until work() returns. False, without
  // waiting and without calling work(), when it is not in that state. Unlike
  // a lease this never makes a Run be refused: a Run that arrives meanwhile
  // waits for work() and then takes effect. May be empty: every read of a
  // paused machine then goes through with_lease.
  std::function<bool(const std::function<void()>&)> read_stopped;
  // Calls work(was_paused) on the calling thread while nothing clocks the
  // machine, then puts the run state back. was_paused is the run state from
  // before any lease. False when the runner did not go idle in time: work was
  // not called.
  std::function<bool(const std::function<void(bool)>&)> with_lease;
};

struct GateTuning {
  // How long a read waits for a frame boundary or a stopped machine before
  // the reader runs it under a pause lease instead. This is a safety net for
  // a runner that turns out no frames although nothing paused it; a machine
  // that is paused is noticed by the poll below, long before. It has to sit
  // well above the longest gap between two boundaries of a healthy machine,
  // because a lease costs what the gate exists to avoid (a pause, and the audio
  // with it) and is no faster: it too waits for the frame in flight to end. A
  // paced frame sleeps to its 50 Hz deadline, and on a loaded host that 19 ms
  // sleep was measured at up to 107 ms. A quarter of a second is also where the
  // frame pacer itself calls a host stalled (frame_pacer.h).
  int fallback_ms = 250;
  // How often a waiting reader looks at the pause state, so a read queued
  // just before the machine stopped does not sit out the whole fallback.
  int poll_ms = 5;
  // Bytes the runner copies at one frame boundary before the reads still
  // queued are left for the next one. Copying 1 MiB takes about 20 us (all
  // 4160 KiB of the largest machine: 80 us), against 20 ms for a paced frame
  // and 400 us for an unpaced one. The first read of a boundary always runs,
  // whatever it costs: a read is never split, because all the bytes of one
  // reply come from the same instant.
  std::size_t frame_budget = 1U << 20;
};

class Gate {
 public:
  // The read itself: copy out of the machine into storage the caller owns and
  // has already sized. Runs on the runner or on the calling thread; it must
  // not block, allocate, or change machine state.
  using Work = std::function<void(const Stamp&)>;

  // Where each read ran, for tests and measurements.
  struct Counters {
    std::uint64_t at_boundary = 0;  // by the runner
    // By the reader, with the machine stopped: when the read arrived, or
    // stopped under it while it was queued.
    std::uint64_t while_paused = 0;
    // By the reader under a pause lease, after fallback_ms without a frame
    // boundary and without a stopped machine.
    std::uint64_t fallback = 0;
    std::uint64_t busy = 0;  // the lease was refused
    // at_boundary() work, run by the runner.
    std::uint64_t writes = 0;
  };

  explicit Gate(GateHost host, GateTuning tuning = {});
  Gate(const Gate&) = delete;
  Gate& operator=(const Gate&) = delete;

  // Runs `work` once, at a moment when the machine is not being clocked, and
  // returns when it has run. `cost` is the number of bytes it copies. Call
  // from a reader thread, never from the runner.
  GateResult read(std::size_t cost, const Work& work);

  // Queues `work` for the runner, behind the reads already queued, and waits
  // up to `timeout_ms` for the frame boundary that runs it. Unlike a read's,
  // this work may change the machine: it only ever runs on the runner, between
  // two frames. False, with work not run, when the machine is paused, pauses
  // before the boundary, or no boundary comes in time: the caller takes it
  // another way. Not counted in waiting(). Call from a reader thread.
  bool at_boundary(std::size_t cost, int timeout_ms, const Work& work);

  // The runner's half, called once per completed frame from the shared frame
  // epilogue: counts the frame and runs queued reads within the budget. With
  // nothing queued it costs one atomic increment and one relaxed load.
  void service_frame();

  // Completed frames since the core started: the `frame` of a stamp.
  std::uint64_t frame() const { return frame_.load(std::memory_order_acquire); }

  // Fails every queued and every later read with kShuttingDown, at once.
  void shut_down();
  void reopen();

  std::size_t queued() const { return queued_.load(std::memory_order_acquire); }
  // Reads that have been asked for and not yet taken, wherever they are
  // waiting: queued for a frame boundary, or for a lease to let go of the
  // machine. A writer looks at this to let them in between two writes.
  std::size_t waiting() const {
    return waiting_.load(std::memory_order_acquire);
  }
  // Whether clients are reading: a read is waiting, or the runner served one
  // at one of the last kReadingFrames boundaries. A reader that has just been
  // answered is not waiting, but its next read is on its way.
  static constexpr std::uint64_t kReadingFrames = 4;
  bool reading() const {
    std::uint64_t const last = last_read_frame_.load(std::memory_order_acquire);
    return waiting() != 0 || (last != 0 && frame() - last <= kReadingFrames);
  }
  Counters counters() const;

 private:
  enum class JobState : std::uint8_t { kQueued, kClaimed, kDone };
  struct Job;

  using Clock = std::chrono::steady_clock;
  // Why a queued job came back to its reader without having run.
  enum class WaitEnd : std::uint8_t { kPaused, kTimedOut, kShutDown };

  // Queues the job and waits. True: the runner ran it. False: the reader has
  // it back, unrun, and `end` says why.
  bool wait_at_boundary(Job& job, Clock::time_point deadline, WaitEnd& end);
  GateResult run_under_lease(const Work& work,
                             std::atomic<std::uint64_t>& counter);
  void unlink(Job& job);

  GateHost host_;
  GateTuning tuning_;

  std::atomic<std::uint64_t> frame_{0};
  // The boundary at which the runner last served a read; 0 until it has
  // served one (the first boundary is frame 1).
  std::atomic<std::uint64_t> last_read_frame_{0};
  // Mirrors the length of the list below so the runner can skip the mutex.
  std::atomic<std::size_t> queued_{0};
  std::atomic<std::size_t> waiting_{0};

  // Guards the list and pairs with done_cv_. Held for pointer updates only,
  // never across a read, an allocation or a system call.
  std::mutex mutex_;
  std::condition_variable done_cv_;
  Job* head_ = nullptr;
  Job* tail_ = nullptr;
  bool shut_down_ = false;

  std::atomic<std::uint64_t> at_boundary_{0};
  std::atomic<std::uint64_t> while_paused_{0};
  std::atomic<std::uint64_t> fallback_{0};
  std::atomic<std::uint64_t> busy_{0};
  std::atomic<std::uint64_t> writes_{0};
};

}  // namespace kcpc
