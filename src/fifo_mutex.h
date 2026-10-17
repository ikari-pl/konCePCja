#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

// A mutex granted in the order it was asked for (a ticket lock).
//
// std::mutex promises no order: the thread that just unlocked usually takes
// it straight back, ahead of a waiter it woke that has yet to be scheduled.
// A thread that locks it again microseconds after unlocking, as the frame
// runner does around every machine frame, then starves the others: on a
// 2-core Windows runner a pause lease waited for minutes (beads-m1lz). Here
// whoever asked first goes first, so a waiter is in at most one holder late.
//
// Lockable enough for std::scoped_lock and std::lock_guard. Not recursive.
class FifoMutex {
 public:
  FifoMutex() = default;
  FifoMutex(const FifoMutex&) = delete;
  FifoMutex& operator=(const FifoMutex&) = delete;

  void lock() {
    std::unique_lock<std::mutex> lock(m_);
    uint64_t const ticket = next_++;
    turn_.wait(lock, [&] { return serving_ == ticket; });
  }

  void unlock() {
    {
      std::scoped_lock const lock(m_);
      ++serving_;
    }
    turn_.notify_all();
  }

 private:
  std::mutex m_;
  std::condition_variable turn_;
  uint64_t next_ = 0;     // the ticket the next lock() takes
  uint64_t serving_ = 0;  // the ticket that holds the mutex
};
