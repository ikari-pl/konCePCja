#pragma once

#include <atomic>
#include <thread>
#include <cstdint>

// Kaprys IPC Server (minimal stub)
class KaprysIpcServer {
public:
  ~KaprysIpcServer();
  void start();
  void stop();

  void notify_breakpoint_hit(uint16_t pc, bool watchpoint);
  bool consume_breakpoint_hit(uint16_t& pc, bool& watchpoint);

private:
  void run();
  std::atomic<bool> running{false};
  std::thread server_thread;

  std::atomic<bool> breakpoint_hit{false};
  std::atomic<uint16_t> breakpoint_pc{0};
  std::atomic<bool> breakpoint_watchpoint{false};
};
