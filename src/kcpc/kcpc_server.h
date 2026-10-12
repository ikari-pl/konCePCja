#pragma once

// KCPC binary frontend server: the listener and the per-client threads.
//
// The contract is docs/protocol/kcpc-binary-v1.md. So far: framing, HELLO,
// PING, MACHINE_INFO, ERROR (slice 1), the machine reads REGS_GET, MEM_INFO,
// MEM_READ (slice 2), the control role with the machine writes REGS_SET
// and MEM_WRITE (slice 3), and run control with the pushed events: SUBSCRIBE,
// RUN_STATE, PAUSE, RUN, RESET, STEP and the fan-out thread (slice 4), and
// the breakpoints BP_LIST, BP_ADD, BP_DEL, BP_CLEAR with the STEP modes that
// walk the machine (slice 5). At most one client is the controller.
//
// The server knows nothing about the emulator. What it reports about the
// machine comes through the callbacks of ServerConfig, so the same class runs
// in the GUI build, under -H, in the UI-free build and in test_runner (with a
// fake machine). It uses std::thread and net_socket.h and nothing from SDL.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kcpc/kcpc_events.h"
#include "kcpc/kcpc_proto.h"
#include "net_socket.h"

namespace kcpc {

// The machine, as the server reaches it. Each callback is called on a
// client's reader thread and returns once the read has been taken, at one
// instant, with the machine not being clocked (kcpc_gate.h). What it returns
// is the status the request answers with:
//   kOk            the out-parameters are filled in
//   kBusy          the runner did not go idle in time; nothing was read
//   kShuttingDown  the core is quitting
//   kBadPayload, kOutOfRange, kNotPresent, kInternal   the request itself
using MachineInfoFn = std::function<Status(MachineInfo&)>;
using RegsGetFn = std::function<Status(Stamp&, Regs&)>;
using MemInfoFn = std::function<Status(MemInfo&)>;
// Copies request.len bytes into dst, which the server has sized: the read
// itself allocates nothing.
using MemReadFn =
    std::function<Status(const MemReadRequest&, Stamp&, std::uint8_t* dst)>;

// The machine changes. Each is called on the controller's reader thread and
// returns once the change has been made, with the machine not being clocked
// and its run state put back as it was; kBusy when that could not be had, and
// nothing was changed then.
//
// Sets the fields of `values` that `mask` selects (the server has checked
// both with regs_set_refusal()); `after` is the register block afterwards.
using RegsSetFn = std::function<Status(std::uint32_t mask, const Regs& values,
                                       Stamp&, Regs& after)>;
// Writes `len` bytes at request.addr. The server has checked the space and
// the bank; the range is the machine's to judge (kOutOfRange).
using MemWriteFn = std::function<Status(
    const MemWriteRequest&, const std::uint8_t* data, std::size_t len, Stamp&)>;

// Run control. Each is called on a reader thread and returns without waiting
// for emulated time: a step answers once it is armed, and how it ended
// arrives as an event (Server::post()). `cause` names the request, for the
// events it leads to.
struct Cause {
  std::uint32_t client = 0;
  std::uint32_t request = 0;
};
// The run state as last announced, and whether a step is in progress. Must
// not wait for the machine: clients poll it.
using RunStateFn = std::function<Status(Stamp&, bool& walk_active)>;
using PauseFn = std::function<Status(const Cause&, Stamp&)>;
using RunFn = std::function<Status(const Cause&, Stamp&)>;
using ResetFn = std::function<Status(const Cause&, bool stay_paused, Stamp&)>;
// The server has checked the mode and the count (0 already made 1), and has
// made a timeout of 0 the default for the modes that have one.
using StepFn = std::function<Status(const Cause&, const StepRequest&)>;

// Breakpoints: the editing lists the text IPC and the DevTools share. Each
// is called on a reader thread. The edits return the generation of the lists
// afterwards; every edit, from any source, is also posted by the machine's
// side as EVT_BREAKPOINTS_CHANGED (Server::post()), with `cause` for an edit
// a request made, so that the client hears of it after its reply.
using BpListFn =
    std::function<Status(std::uint64_t& generation, std::vector<BpEntry>&)>;
// The server has checked the entry's fields (bp_entry_refusal()) and a
// watch's range. kBadPayload when the condition does not parse (`message`
// says why), kLimitReached when the probe has no comparator left for it;
// nothing was added then.
using BpAddFn =
    std::function<Status(const Cause&, const BpEntry&,
                         std::uint64_t& generation, std::string& message)>;
// kNotFound when there is no such entry.
using BpDelFn = std::function<Status(const Cause&, const BpDelRequest&,
                                     std::uint64_t& generation)>;
// `kind` is a BpKind or kBpKindAll; the server has checked it.
using BpClearFn = std::function<Status(const Cause&, std::uint8_t kind,
                                       std::uint64_t& generation)>;

struct ServerConfig {
  int base_port = kDefaultPort;
  int probe_count = kPortProbeCount;
  std::size_t max_clients = kMaxClients;
  // A connection holds one of the max_clients places from the moment it is
  // accepted. One that has not completed HELLO within this time is closed
  // with HELLO_REQUIRED, so idle connections cannot keep real clients out.
  int hello_timeout_ms = 5000;
  // A client that lets this many bytes of responses pile up unread is
  // disconnected (section 9).
  std::size_t max_queue_bytes = 32U << 20;
  // Events are queued for a client only while its queue holds less than
  // this. Beyond it they are dropped for that client alone, which is told
  // how many with one EVT_OVERRUN once there is room again.
  std::size_t max_event_queue_bytes = 4U << 20;
  std::string server_version;
  std::uint32_t pid = 0;
  MachineInfoFn machine_info;
  // The CAP_MACHINE_READ group. HELLO announces the capability (and
  // CAP_MEM_ROM with it) only when all three are set; without them the
  // opcodes answer UNKNOWN_OPCODE.
  RegsGetFn regs_get;
  MemInfoFn mem_info;
  MemReadFn mem_read;
  // The CAP_MACHINE_WRITE group, and with it the control role: HELLO grants
  // control, and CONTROL, REGS_SET and MEM_WRITE exist, only when both are
  // set. Without them every client is an observer.
  RegsSetFn regs_set;
  MemWriteFn mem_write;
  // The CAP_RUN_CONTROL group: RUN_STATE, PAUSE, RUN, RESET and STEP exist,
  // and HELLO announces the capability, only when all five are set.
  RunStateFn run_state;
  PauseFn pause;
  RunFn run;
  ResetFn reset;
  StepFn step;
  // The CAP_BREAKPOINTS group: BP_LIST, BP_ADD, BP_DEL and BP_CLEAR exist,
  // HELLO announces the capability and STEP takes the modes over, out and
  // to, only when all four are set.
  BpListFn bp_list;
  BpAddFn bp_add;
  BpDelFn bp_del;
  BpClearFn bp_clear;
  // A controller that has sent no frame for this long is disconnected, so
  // that a client that hangs does not keep control for ever (section 7).
  int controller_idle_timeout_ms = 30000;
};

class Server {
 public:
  Server();
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds the first free port of the configured range on 127.0.0.1 and starts
  // the acceptor. The port is bound before this returns, so port() is valid
  // at once. False when no port in the range could be bound.
  bool start(const ServerConfig& config);
  // Stops accepting, tells every client the core is shutting down, closes the
  // connections and joins every thread. Safe to call twice or without start().
  void stop();

  bool running() const { return running_.load(std::memory_order_acquire); }
  // The bound port, or 0 when the server is not running.
  int port() const { return port_.load(std::memory_order_acquire); }
  // Connections that currently hold a client place.
  std::size_t client_count();
  // The client_id of the controller, or 0 when nobody has control.
  std::uint32_t controller() const {
    return controller_.load(std::memory_order_acquire);
  }

  // Hands an event to the fan-out thread, which copies it to the clients
  // that subscribed. Callable from any thread, the frame runner included:
  // one lock-free push into a fixed ring, no allocation, no socket call. A
  // full ring loses the event; the clients are then told with EVT_OVERRUN.
  // Does nothing while the server is not running.
  void post(const Event& event);
  // Whether any client is due an EVT_FRAME for this frame: the runner asks
  // before it posts one. One relaxed load when nobody subscribed to frames.
  bool frame_due(std::uint64_t frame) const {
    std::uint32_t const stride = frame_stride_.load(std::memory_order_relaxed);
    return stride != 0 && frame % stride == 0;
  }

 private:
  class Connection;

  void accept_loop();
  void accept_pending();
  void reap_finished();
  void fan_out_loop();
  // Recomputes which frames the runner has to post, from every client's
  // subscription.
  void update_frame_stride();

  ServerConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<int> port_{0};
  // The one controller, by client_id; 0 = control is free. Connections take
  // and give it with compare-and-swap, so there is never more than one.
  std::atomic<std::uint32_t> controller_{0};
  net::Socket listener_ = net::kInvalidSocket;
  std::thread acceptor_;
  std::mutex connections_mutex_;
  std::list<std::unique_ptr<Connection>> connections_;

  EventRing ring_;
  std::thread fan_out_;
  std::mutex fan_out_mutex_;
  std::condition_variable fan_out_cv_;
  // True while the fan-out thread is, or is about to be, asleep: a producer
  // then wakes it. Otherwise posting is the push alone.
  std::atomic<bool> fan_out_sleeping_{false};
  // The greatest common divisor of the frame intervals clients asked for; 0
  // when nobody wants EVT_FRAME.
  std::atomic<std::uint32_t> frame_stride_{0};
};

}  // namespace kcpc
