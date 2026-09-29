/* konCePCja — run a blocking call under a deadline.
 *
 * Some system calls have no timeout of their own: getaddrinfo() against an
 * unreachable resolver blocks for tens of seconds, and it is called from the
 * main thread at startup and from the IPC thread on `serial config set`. A
 * caller that must stay responsive runs the call here instead: the work goes
 * to a worker thread, the caller waits at most `timeout_ms`, and a worker
 * that comes back too late hands its result to `discard` on its own thread.
 *
 * std::async is deliberately not used: the future returned by a
 * std::launch::async call blocks in its destructor, which would re-impose the
 * very wait this bounds.
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

enum class DeadlineResult : std::uint8_t {
  Ok,        // the work finished in time and reported success
  Failed,    // the work finished in time and reported failure
  TimedOut,  // the deadline passed; the work is still running
};

// Run `work(T&)` on a worker thread and wait up to `timeout_ms` for it.
// `work` returns true when it filled the out-parameter. On Ok, `out` holds
// that value. On TimedOut the worker is left to finish on its own and its
// value, if it produces one, goes to `discard(T)` so nothing leaks; `out` is
// untouched.
template <typename T, typename Work, typename Discard>
DeadlineResult run_with_deadline(Work work, Discard discard, int timeout_ms,
                                 T& out) {
  struct Shared {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool abandoned = false;
    bool ok = false;
    T value{};
  };
  auto const shared = std::make_shared<Shared>();

  std::thread([shared, work = std::move(work),
               discard = std::move(discard)]() mutable {
    T value{};
    bool const ok = work(value);
    std::unique_lock lock(shared->mutex);
    if (shared->abandoned) {
      lock.unlock();
      if (ok) discard(std::move(value));
      return;
    }
    shared->value = std::move(value);
    shared->ok = ok;
    shared->done = true;
    lock.unlock();
    shared->cv.notify_one();
  }).detach();

  std::unique_lock lock(shared->mutex);
  if (!shared->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&shared] { return shared->done; })) {
    shared->abandoned = true;
    return DeadlineResult::TimedOut;
  }
  if (!shared->ok) return DeadlineResult::Failed;
  out = std::move(shared->value);
  return DeadlineResult::Ok;
}
