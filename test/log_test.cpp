// The LOG_* macros are called from the Z80 thread, the render thread and the
// IPC, telnet, M4 HTTP and KCPC threads at once: one log line must be one
// write that no other thread's line can enter (beads-2qcw).

#include "log.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace {

// A stream buffer that notices two writers inside it at once. Every write
// lingers for a moment so that, without a lock around the whole line, a
// second thread's write lands inside the first one.
class OverlapDetectingBuf : public std::streambuf {
 public:
  bool overlapped() const { return overlapped_.load(); }
  std::string text() {
    std::lock_guard<std::mutex> lock(text_mutex_);
    return text_;
  }

 protected:
  std::streamsize xsputn(const char* s, std::streamsize n) override {
    enter();
    {
      std::lock_guard<std::mutex> lock(text_mutex_);
      text_.append(s, static_cast<size_t>(n));
    }
    leave();
    return n;
  }
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof())) return 0;
    char c = traits_type::to_char_type(ch);
    xsputn(&c, 1);
    return ch;
  }

 private:
  void enter() {
    if (inside_.fetch_add(1) != 0) overlapped_ = true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  void leave() { inside_.fetch_sub(1); }

  std::atomic<int> inside_{0};
  std::atomic<bool> overlapped_{false};
  std::mutex text_mutex_;
  std::string text_;
};

}  // namespace

TEST(LogTest, ConcurrentLinesAreWrittenOneAtATime) {
  OverlapDetectingBuf buf;
  std::streambuf* saved = std::cerr.rdbuf(&buf);

  constexpr int kThreads = 8;
  constexpr int kLines = 20;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t] {
      for (int i = 0; i < kLines; ++i) {
        LOG_INFO("thread " << t << " line " << i)
      }
    });
  }
  for (auto& th : threads) th.join();
  std::cerr.rdbuf(saved);

  EXPECT_FALSE(buf.overlapped()) << "two threads were writing log lines into "
                                    "std::cerr at the same time";

  // Every line arrived whole: the prefix, the message and the newline.
  std::istringstream lines(buf.text());
  std::string line;
  int count = 0;
  while (std::getline(lines, line)) {
    ++count;
    EXPECT_EQ(line.rfind("INFO    ", 0), 0u) << line;
    EXPECT_NE(line.find(" - thread "), std::string::npos) << line;
  }
  EXPECT_EQ(count, kThreads * kLines);
}
