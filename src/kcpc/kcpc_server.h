#pragma once

// KCPC binary frontend server: the listener and the per-client threads.
//
// The contract is docs/protocol/kcpc-binary-v1.md. So far: framing, HELLO,
// PING, MACHINE_INFO, ERROR (slice 1), the machine reads REGS_GET, MEM_INFO,
// MEM_READ (slice 2), and the control role with the machine writes REGS_SET
// and MEM_WRITE (slice 3). At most one client is the controller.
//
// The server knows nothing about the emulator. What it reports about the
// machine comes through the callbacks of ServerConfig, so the same class runs
// in the GUI build, under -H, in the UI-free build and in test_runner (with a
// fake machine). It uses std::thread and net_socket.h and nothing from SDL.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

 private:
  class Connection;

  void accept_loop();
  void accept_pending();
  void reap_finished();

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
};

}  // namespace kcpc
