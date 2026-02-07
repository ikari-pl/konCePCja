#pragma once

#include <atomic>
#include <thread>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

// Event trigger types
enum class EventTrigger { PC, MEM_WRITE, VBL };

struct IpcEvent {
  int id;
  EventTrigger trigger;
  uint16_t address;      // PC or memory address (for PC/MEM_WRITE triggers)
  uint8_t value;         // expected value (for MEM_WRITE, 0 = any)
  bool match_value;      // whether to check value on MEM_WRITE
  int vbl_interval;      // fire every N VBLs (for VBL trigger)
  int vbl_counter;       // countdown for VBL
  bool one_shot;         // remove after first fire
  std::string command;   // IPC command to execute when triggered
};

// konCePCja IPC Server
class KoncepcjaIpcServer {
public:
  ~KoncepcjaIpcServer();
  void start();
  void stop();

  void notify_breakpoint_hit(uint16_t pc, bool watchpoint);
  bool consume_breakpoint_hit(uint16_t& pc, bool& watchpoint);

  // Frame stepping: set by IPC "step frame N", decremented by main loop each frame
  std::atomic<int> frame_step_remaining{0};
  // Set true when frame stepping is active; main loop pauses when count reaches 0
  std::atomic<bool> frame_step_active{false};

  // Event system — called from hot paths, must be fast
  void check_pc_events(uint16_t pc);
  void check_mem_write_events(uint16_t addr, uint8_t val);
  void check_vbl_events();

  // Event management
  int add_event(const IpcEvent& ev);
  bool remove_event(int id);
  std::vector<IpcEvent> list_events() const;

private:
  void run();
  void execute_event_command(const std::string& cmd);

  std::atomic<bool> running{false};
  std::thread server_thread;

  std::atomic<bool> breakpoint_hit{false};
  std::atomic<uint16_t> breakpoint_pc{0};
  std::atomic<bool> breakpoint_watchpoint{false};

  // Events — guarded by mutex for add/remove, but checks use atomic flag for fast path
  mutable std::mutex events_mutex;
  std::vector<IpcEvent> events;
  int next_event_id{1};
  std::atomic<bool> has_pc_events{false};
  std::atomic<bool> has_mem_events{false};
  std::atomic<bool> has_vbl_events{false};
  void update_event_flags();
};

// Free functions for calling from z80.cpp / main loop (use g_ipc_instance internally)
void ipc_check_pc_events(uint16_t pc);
void ipc_check_mem_write_events(uint16_t addr, uint8_t val);
void ipc_check_vbl_events();
