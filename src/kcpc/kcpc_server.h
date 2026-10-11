#pragma once

// KCPC binary frontend server: the listener and the per-client threads.
//
// The contract is docs/protocol/kcpc-binary-v1.md. This is slice 1: framing,
// HELLO, PING, MACHINE_INFO and ERROR. Every client is an observer.
//
// The server knows nothing about the emulator. What it reports about the
// machine comes through ServerConfig::machine_info, so the same class runs in
// the GUI build, under -H, in the UI-free build and in test_runner (with a
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

// Fills in the MACHINE_INFO fields. Called on a client's reader thread.
// False means the runner did not go idle in time and nothing was read: the
// request answers BUSY.
using MachineInfoFn = std::function<bool(MachineInfo&)>;

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

 private:
  class Connection;

  void accept_loop();
  void accept_pending();
  void reap_finished();

  ServerConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<int> port_{0};
  net::Socket listener_ = net::kInvalidSocket;
  std::thread acceptor_;
  std::mutex connections_mutex_;
  std::list<std::unique_ptr<Connection>> connections_;
};

}  // namespace kcpc
