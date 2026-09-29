/* run_with_deadline(): the bound TcpSocketBackend::open() puts around
 * getaddrinfo().
 *
 * A hostname that resolves slowly is not something a test can arrange
 * deterministically, so the resolver is injected: these tests drive the
 * helper with a fake one that answers at a chosen moment. What must hold is
 * that the caller comes back within the deadline, that a late answer is
 * still cleaned up rather than leaked, and that nothing is handed back from
 * a lookup the caller gave up on.
 */

#include "bounded_deadline.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

long long ms_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t)
      .count();
}

// Stands in for the addrinfo list: a resource the worker allocates and
// somebody must release.
struct FakeAddrinfo {
  std::shared_ptr<std::atomic<int>> freed;
};

}  // namespace

TEST(RunWithDeadline, AResolverThatAnswersInTimeHandsBackItsResult) {
  auto const freed = std::make_shared<std::atomic<int>>(0);
  FakeAddrinfo out{};
  auto const result = run_with_deadline<FakeAddrinfo>(
      [freed](FakeAddrinfo& value) {
        value.freed = freed;
        return true;
      },
      [](FakeAddrinfo late) { late.freed->fetch_add(1); }, 5000, out);

  EXPECT_EQ(DeadlineResult::Ok, result);
  EXPECT_EQ(freed, out.freed);
  EXPECT_EQ(0, freed->load()) << "a result the caller took must not be freed";
}

TEST(RunWithDeadline, AResolverThatFailsIsNotATimeout) {
  FakeAddrinfo out{};
  auto const result = run_with_deadline<FakeAddrinfo>(
      [](FakeAddrinfo&) { return false; },
      [](FakeAddrinfo) { ADD_FAILURE() << "nothing was produced to discard"; },
      5000, out);

  EXPECT_EQ(DeadlineResult::Failed, result);
  EXPECT_EQ(nullptr, out.freed);
}

// The finding: without the bound the caller blocks for as long as the
// resolver takes. With it, the caller is back inside the deadline while the
// lookup is still running.
TEST(RunWithDeadline, ASlowResolverDoesNotHoldTheCaller) {
  auto const freed = std::make_shared<std::atomic<int>>(0);
  auto const release = std::make_shared<std::atomic<bool>>(false);
  FakeAddrinfo out{};

  auto const started = std::chrono::steady_clock::now();
  auto const result = run_with_deadline<FakeAddrinfo>(
      [freed, release](FakeAddrinfo& value) {
        while (!release->load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        value.freed = freed;
        return true;
      },
      [](FakeAddrinfo late) { late.freed->fetch_add(1); }, 50, out);

  EXPECT_EQ(DeadlineResult::TimedOut, result);
  EXPECT_LT(ms_since(started), 2000) << "the caller waited out the resolver";
  EXPECT_EQ(nullptr, out.freed) << "an abandoned lookup must hand back nothing";

  // The lookup finishes later: its result is discarded, not leaked.
  release->store(true);
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (freed->load() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(1, freed->load()) << "the late result was never released";
}
