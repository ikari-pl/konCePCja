#include "kcpc/kcpc_gate.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace kcpc {

namespace {
// How often a reader looks again while the machine is paused but not yet
// stopped. The same step the pause lease itself waits for the runner in.
constexpr auto kSettlePoll = std::chrono::microseconds(100);
}  // namespace

// One queued read. It lives on its reader's stack: the reader does not return
// before the job is done or it has taken the job back off the list, and the
// runner does not touch a job after marking it done.
struct Gate::Job {
  const Work* work = nullptr;
  std::size_t cost = 0;
  std::atomic<JobState> state{JobState::kQueued};
  Job* prev = nullptr;
  Job* next = nullptr;
};

Gate::Gate(GateHost host, GateTuning tuning)
    : host_(std::move(host)), tuning_(tuning) {}

void Gate::unlink(Job& job) {
  (job.prev != nullptr ? job.prev->next : head_) = job.next;
  (job.next != nullptr ? job.next->prev : tail_) = job.prev;
  job.prev = job.next = nullptr;
  queued_.store(queued_.load(std::memory_order_relaxed) - 1,
                std::memory_order_release);
}

GateResult Gate::run_under_lease(const Work& work,
                                 std::atomic<std::uint64_t>& counter) {
  bool const ran = host_.with_lease([this, &work](bool was_paused) {
    // The runner is idle, so the frame count stands still.
    work(Stamp{frame(), was_paused});
  });
  if (!ran) {
    busy_.fetch_add(1, std::memory_order_relaxed);
    return GateResult::kBusy;
  }
  counter.fetch_add(1, std::memory_order_relaxed);
  return GateResult::kDone;
}

bool Gate::wait_at_boundary(Job& job, Clock::time_point deadline,
                            WaitEnd& end) {
  std::unique_lock lock(mutex_);
  if (shut_down_) {
    end = WaitEnd::kShutDown;
    return false;
  }
  job.state.store(JobState::kQueued, std::memory_order_release);
  job.prev = tail_;
  (tail_ != nullptr ? tail_->next : head_) = &job;
  tail_ = &job;
  queued_.store(queued_.load(std::memory_order_relaxed) + 1,
                std::memory_order_release);

  for (;;) {
    if (job.state.load(std::memory_order_acquire) == JobState::kDone) {
      return true;
    }
    auto const now = Clock::now();
    bool const paused = host_.paused();
    if (shut_down_ || paused || now >= deadline) {
      JobState expected = JobState::kQueued;
      if (job.state.compare_exchange_strong(expected, JobState::kClaimed,
                                            std::memory_order_acq_rel)) {
        // Ours now: the runner only ever takes a job off the list.
        unlink(job);
        end = WaitEnd::kTimedOut;
        if (paused) end = WaitEnd::kPaused;
        if (shut_down_) end = WaitEnd::kShutDown;
        return false;
      }
      // The runner claimed it first and is copying; that ends by itself.
      done_cv_.wait(lock, [&job] {
        return job.state.load(std::memory_order_acquire) == JobState::kDone;
      });
      return true;
    }
    auto const slice = std::min<Clock::duration>(
        deadline - now, std::chrono::milliseconds(tuning_.poll_ms));
    done_cv_.wait_for(lock, slice);
  }
}

GateResult Gate::read(std::size_t cost, const Work& work) {
  Job job;
  job.work = &work;
  job.cost = cost;
  auto const deadline =
      Clock::now() + std::chrono::milliseconds(tuning_.fallback_ms);

  // The machine may be paused and resumed while the read waits, so this goes
  // round until one of the ways in has worked.
  for (;;) {
    if (!host_.paused()) {
      WaitEnd end = WaitEnd::kTimedOut;
      if (wait_at_boundary(job, deadline, end)) return GateResult::kDone;
      if (end == WaitEnd::kShutDown) return GateResult::kShuttingDown;
      if (end == WaitEnd::kTimedOut) return run_under_lease(work, fallback_);
      continue;  // paused under it: no boundary is coming
    }

    {
      std::scoped_lock const lock(mutex_);
      if (shut_down_) return GateResult::kShuttingDown;
    }
    if (!host_.read_stopped) return run_under_lease(work, while_paused_);
    // The runner is idle, so the frame count stands still.
    if (host_.read_stopped([this, &work] { work(Stamp{frame(), true}); })) {
      while_paused_.fetch_add(1, std::memory_order_relaxed);
      return GateResult::kDone;
    }
    // Paused but not stopped: somebody's lease holds the machine, or the stop
    // has not quite landed. Both pass in a moment, and then the machine is
    // either stopped or running again. A lease of our own would work now, but
    // readers that answer a lease with a lease keep one another's alive, and
    // while any lease is held a Run is refused.
    if (Clock::now() >= deadline) return run_under_lease(work, fallback_);
    std::this_thread::sleep_for(kSettlePoll);
  }
}

void Gate::service_frame() {
  // Single writer: only the runner counts frames.
  std::uint64_t const frame = frame_.load(std::memory_order_relaxed) + 1;
  frame_.store(frame, std::memory_order_release);
  if (queued_.load(std::memory_order_relaxed) == 0) return;

  Stamp const stamp{frame, false};
  std::size_t spent = 0;
  std::unique_lock lock(mutex_);
  while (head_ != nullptr) {
    Job& job = *head_;
    if (spent != 0 && job.cost > tuning_.frame_budget -
                                     std::min(spent, tuning_.frame_budget)) {
      break;  // the rest waits for the next boundary
    }
    unlink(job);
    JobState expected = JobState::kQueued;
    if (!job.state.compare_exchange_strong(expected, JobState::kClaimed,
                                           std::memory_order_acq_rel)) {
      continue;  // its reader took it back
    }
    lock.unlock();
    (*job.work)(stamp);
    spent += job.cost;
    at_boundary_.fetch_add(1, std::memory_order_relaxed);
    lock.lock();
    // Stored and announced under the mutex, so the reader cannot miss it
    // between its check and its wait. Once the mutex is released the job may
    // be gone.
    job.state.store(JobState::kDone, std::memory_order_release);
    done_cv_.notify_all();
  }
}

void Gate::shut_down() {
  {
    std::scoped_lock const lock(mutex_);
    shut_down_ = true;
  }
  done_cv_.notify_all();
}

void Gate::reopen() {
  std::scoped_lock const lock(mutex_);
  shut_down_ = false;
}

Gate::Counters Gate::counters() const {
  Counters c;
  c.at_boundary = at_boundary_.load(std::memory_order_relaxed);
  c.while_paused = while_paused_.load(std::memory_order_relaxed);
  c.fallback = fallback_.load(std::memory_order_relaxed);
  c.busy = busy_.load(std::memory_order_relaxed);
  return c;
}

}  // namespace kcpc
