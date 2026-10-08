#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Event trigger types
enum class EventTrigger : std::uint8_t { PC, MEM_WRITE, VBL };

struct IpcEvent {
  int id;
  EventTrigger trigger;
  uint16_t address;     // PC or memory address (for PC/MEM_WRITE triggers)
  uint8_t value;        // expected value (for MEM_WRITE, 0 = any)
  bool match_value;     // whether to check value on MEM_WRITE
  int vbl_interval;     // fire every N VBLs (for VBL trigger)
  int vbl_counter;      // countdown for VBL
  bool one_shot;        // remove after first fire
  std::string command;  // IPC command to execute when triggered
};

// konCePCja IPC Server
class KoncepcjaIpcServer {
 public:
  ~KoncepcjaIpcServer();
  void start();
  void stop();
  int port() const { return actual_port.load(); }
  // False once stop() has been called; long-running commands (the `wait`
  // family) give up on it so stop()'s join does not wait out their deadline.
  bool is_running() const { return running.load(); }

  void notify_breakpoint_hit(uint16_t pc, bool watchpoint,
                             uint64_t arming_generation);
  bool consume_breakpoint_hit(uint16_t& pc, bool& watchpoint);
  // Non-consuming peek. A caller that resumes the machine on the caller's
  // behalf (see `input key`'s tap) must not resume THROUGH a breakpoint that
  // fired while it held the CPU; the hit itself still belongs to `wait bp`.
  // Generation-aware so it agrees with consume_breakpoint_hit(): a latched hit
  // from a PREVIOUS arming is one the consumer would drop, so it must not
  // suppress the tap's resume and strand the machine paused.
  // Defined in the .cpp: it consults z80_breakpoint_generation(), and pulling
  // z80_view.h into this header just for that would widen the include graph.
  bool breakpoint_hit_pending() const;

  // Frame stepping: set by IPC "step frame N", decremented by main loop each
  // frame
  std::atomic<int> frame_step_remaining{0};
  // Set true when frame stepping is active; main loop pauses when count reaches
  // 0
  std::atomic<bool> frame_step_active{false};
  // Only frames that began with a keyboard snapshot newer than this serial
  // (g_kbd_publish_serial) count toward the step. 0 counts every frame; a key
  // tap arms it with the serial read after its press, so the hold is measured
  // in frames the firmware could actually scan the key in (beads-cjej).
  std::atomic<uint64_t> frame_step_after_serial{0};

  // Arm a frame step of n counted frames (see frame_step_after_serial).
  void arm_frame_step(int n, uint64_t after_serial = 0);
  // Called by the emulation loop at each completed frame with the serial of
  // the keyboard snapshot that frame began with. Returns true when this frame
  // finished the step; the caller then pauses and calls
  // notify_frame_step_done().
  bool frame_step_tick(uint64_t frame_kbd_serial);

  // Signal that frame stepping has completed (called from main loop)
  void notify_frame_step_done();
  // Block until frame_step_active becomes false
  void wait_frame_step_done();

  // How wait_frame_step_until() ended.
  enum class FrameStepWait {
    Done,        // the counted frames ran; the main loop paused the machine
    Breakpoint,  // a breakpoint/watchpoint stopped the machine first
    Timeout,     // the deadline passed first
    Aborted,     // should_abort() said to give up (server shutting down)
  };
  // Like wait_frame_step_done(), but bounded by `deadline` and outliving a
  // plain pause: frames only count while the machine runs, so a pause someone
  // else lifts again (the -i injection's) just delays the count. Anything but
  // Done disarms the step, so a late tick cannot pause a later run.
  FrameStepWait wait_frame_step_until(
      std::chrono::steady_clock::time_point deadline,
      const std::function<bool()>& should_abort);

  // Event system — called from hot paths, must be fast. A matching event's
  // command runs on the calling thread after the event lock is dropped, so it
  // may edit the event list itself (`event off <own id>`).
  void check_pc_events(uint16_t pc);
  void check_mem_write_events(uint16_t addr, uint8_t val);
  void check_vbl_events();
  // The distinct addresses armed by pc= (PC) or mem= (MEM_WRITE) events, up
  // to `max`, for the board's bus probe. Returns 0 without locking while no
  // event of that trigger exists.
  int event_addresses(EventTrigger trigger, uint16_t* out, int max) const;

  // Event management
  int add_event(const IpcEvent& ev);
  bool remove_event(int id);
  std::vector<IpcEvent> list_events() const;

 private:
  void run();
  void execute_event_command(const std::string& cmd);

  std::atomic<bool> running{false};
  std::atomic<int> actual_port{0};
  std::thread server_thread;

  std::atomic<bool> breakpoint_hit{false};
  // Arming generation the latched hit fired under; see
  // z80_breakpoint_generation(). A hit from an older generation is dropped
  // rather than reported as if the currently-armed breakpoint had fired.
  std::atomic<uint64_t> breakpoint_hit_generation{0};
  std::atomic<uint16_t> breakpoint_pc{0};
  std::atomic<bool> breakpoint_watchpoint{false};

  // Condition variable for frame-step completion (replaces busy-wait)
  std::mutex frame_step_mutex;
  std::condition_variable frame_step_cv;

  // Events — guarded by mutex for add/remove, but checks use atomic flag for
  // fast path
  mutable std::mutex events_mutex;
  std::vector<IpcEvent> events;
  int next_event_id{1};
  std::atomic<bool> has_pc_events{false};
  std::atomic<bool> has_mem_events{false};
  std::atomic<bool> has_vbl_events{false};
  void update_event_flags();
};

// Free functions for calling from z80.cpp / main loop (use g_ipc_instance
// internally)
void ipc_check_pc_events(uint16_t pc);
void ipc_check_mem_write_events(uint16_t addr, uint8_t val);
void ipc_check_vbl_events();
// What subcycle_bridge_sync_probe() arms on the bus probe so pc= / mem= events
// fire (beads-uj1c). Each returns 0 at the cost of one atomic load while no
// such event is armed.
int ipc_event_pc_addresses(uint16_t* out, int max);
int ipc_event_mem_addresses(uint16_t* out, int max);

// Flush staged IPC input (mouse deltas/buttons) into the emulated devices.
// MUST be called once per frame on the main thread — the IPC server thread only
// accumulates; this applies. Cheap no-op when nothing is pending.
void ipc_drain_input();

// Publish whether a mouse (AMX/Symbiface) and a light gun are fitted, for the
// `input mouse`/`input gun` gates on the IPC thread. ipc_drain_input() does it
// every frame; loadConfiguration() does it too, so the gates are right before
// the first drain -- in GUI mode the Z80 thread (and a client talking to it)
// can run before the main loop's first frame (beads-i834).
void ipc_publish_device_gates(bool mouse_fitted, bool gun_fitted);
// The published state, as the gates read it.
bool ipc_mouse_gate_open();
bool ipc_gun_gate_open();

// Main thread: publish the host keymap in use and the directory its *.map
// files live in, for `config get|set kbd_layout(s)` to answer from without
// touching CPC.kbd_layout / CPC.resources_path off-thread. Called every time
// the InputMapper is (re)loaded; until the first call those commands answer
// `ERR 503 not-ready`.
void ipc_publish_host_keymap(const std::string& layout,
                             const std::string& resources_path);

// Main thread: publish the configuration file this session loaded, for
// `config get file`.
void ipc_publish_config_file(const std::string& path);

// Main thread: publish the main window's live geometry and fullscreen state
// for `config get window|fullscreen`. ipc_drain_input() does it once per
// frame; a fullscreen toggle re-publishes right after the transition.
void ipc_publish_window_state();

// The deadline `wait vbl <n>` gets when the caller gives none: the wait's own
// nominal length (20ms per blank) plus the 5s every `wait` allows, so a long
// but legitimate count no longer times out at 5s regardless of n.
std::chrono::milliseconds ipc_wait_vbl_default_timeout(int count);
