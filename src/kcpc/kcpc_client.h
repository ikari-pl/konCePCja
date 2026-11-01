#pragma once

// KCPC binary client: the C++ side of the protocol a frontend speaks to a core
// (beads-3sp.1). The contract is docs/protocol/kcpc-binary-v1.md; the Python
// reference client is test/integrated/kcpc/kcpc_client.py.
//
// The transport and nothing else: one loopback connection, HELLO, a reader
// thread that matches each response to its request by `id` and hands events
// to a callback or a queue, blocking and asynchronous requests, CONTROL, and
// a PING whenever the client has sent nothing for a while, so a controller is
// never timed out for being quiet (section 7). Every payload is read and
// written with the codec of kcpc_proto.h and kcpc_events.h; there is no
// second one here.
//
// Threads: any number of threads may send at once (frames are written whole,
// one at a time, each within its request's timeout). Callbacks run on the
// reader thread, in the order the frames arrived, so an event caused by a
// request is seen after that request's response, as the server sends them. A
// callback must not wait for a reply: the reader thread is the one that would
// deliver it. It may close() the client, but not connect() it again (refused
// with kClosed: the reader cannot join itself) and not destroy it (the reader
// is running it); reconnect from a thread of the owner's.
//
// Emulator-free, like the server: test_runner runs it against kcpc::Server.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kcpc/kcpc_events.h"
#include "kcpc/kcpc_proto.h"
#include "net_socket.h"

namespace kcpc {

// How a request ended, before its status is looked at.
enum class Outcome : std::uint8_t {
  kReplied,   // the server answered; `status` is its answer
  kTimeout,   // no answer in time; a late one is dropped
  kClosed,    // the connection ended first (or was never open)
  kBadReply,  // the answer was shorter than its fields
};

struct Result {
  Outcome outcome = Outcome::kClosed;
  // The server's answer; with kClosed, close_status(): the status of the
  // ERROR the server ended the connection with (TOO_MANY_CLIENTS,
  // SHUTTING_DOWN, ...), or kOk when it ended without one. kInternal with
  // kClosed names a misuse: connect() from a callback.
  Status status = Status::kInternal;
  std::string message;  // the server's, or why the connection ended

  bool ok() const {
    return outcome == Outcome::kReplied && status == Status::kOk;
  }
};

// A response: the result, and on success the reply fields after the status
// header.
struct Reply : Result {
  std::vector<std::uint8_t> fields;
};

// One pushed event, decoded. `seq` is its number on this connection: 1 for the
// first and one more for each after it.
struct ClientEvent {
  std::uint32_t seq = 0;
  Event event;
  MediaNote media;  // EVT_MEDIA_CHANGED
};

using EventFn = std::function<void(const ClientEvent&)>;
// Called once per asynchronous request, on the reader thread, or on the thread
// that closed the connection.
using ReplyFn = std::function<void(const Reply&)>;

struct ClientOptions {
  std::string name = "kcpc_client";  // HELLO client_name, for the core's log
  Role role = Role::kObserver;       // asked for in HELLO
  std::uint32_t max_frame = 0;       // HELLO max_frame; 0 = 16 MiB
  std::uint64_t want_caps = 0;
  int connect_timeout_ms = 5000;
  // The timeout of a request that names none.
  int call_timeout_ms = 5000;
  // A PING is sent when nothing was for this long; the server drops a silent
  // controller after 30 s. 0: never. It is sent by the reader thread, and
  // only while no other frame is being sent and the socket can take it at
  // once: a stalled send never parks the reader behind it (a frame in flight
  // is itself the sign of life).
  int keepalive_ms = 10000;
  // With a callback, every event goes to it. Without one events are queued
  // for next_event(), at most `max_queued_events`; beyond that the oldest is
  // dropped and counted (dropped_events()).
  EventFn on_event;
  std::size_t max_queued_events = 4096;
};

class Client {
 public:
  Client() = default;
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Connects to 127.0.0.1:port and completes HELLO. On failure the client is
  // closed again and `result` says why: kClosed for no connection, the
  // server's status for a refused HELLO, whether it answered the HELLO with
  // it (kReplied) or ended the connection with an ERROR (kClosed, the status
  // copied from it). Refused with kClosed from a callback (see Threads).
  bool connect(int port, const ClientOptions& options, Result& result);
  // Closes the connection and joins the reader thread. Every request still
  // waiting ends with kClosed. Safe to call twice; from a callback it only
  // asks the reader to stop (the destructor joins it): the reader then
  // half-closes the socket, so the server sees the client go at once.
  void close();

  bool connected() const { return open_.load(std::memory_order_acquire); }
  const HelloReply& hello() const { return hello_; }
  std::uint32_t client_id() const { return hello_.client_id; }
  // Whether this client holds control, as the server last said: HELLO, a
  // CONTROL reply, or EVT_CONTROL_CHANGED.
  bool is_controller() const {
    return controller_.load(std::memory_order_acquire) == hello_.client_id &&
           hello_.client_id != 0;
  }
  // The controller's client_id as last heard; 0 = nobody.
  std::uint32_t controller() const {
    return controller_.load(std::memory_order_acquire);
  }
  // Why the connection ended: the status of the server's ERROR (SHUTTING_DOWN,
  // IDLE_TIMEOUT, ...), or kOk while it is open or when it just closed.
  Status close_status() const;

  // One request; blocks until its response, the timeout (-1: the options'),
  // or the end of the connection. The timeout bounds the whole call, the
  // send included: a request the socket took nothing of in time is kTimeout
  // and the connection is as it was; one it took only part of ends the
  // connection (kClosed), since the stream is cut.
  Reply call(Opcode opcode, const std::vector<std::uint8_t>& payload,
             int timeout_ms = -1);
  // One request without waiting: `done` (may be empty) gets the response.
  // Returns the request's id, 0 when it could not be sent: `done` was then
  // called with kClosed, or with kTimeout when the socket took nothing of it
  // within the options' call timeout.
  std::uint32_t send(Opcode opcode, const std::vector<std::uint8_t>& payload,
                     ReplyFn done);

  // The next queued event (no on_event callback): false on timeout, or when
  // the connection has ended and nothing is left.
  bool next_event(ClientEvent& out, int timeout_ms);
  std::uint64_t dropped_events() const {
    return dropped_events_.load(std::memory_order_relaxed);
  }

  // -- typed requests, one per opcode (section 12) ---------------------------
  // Session.
  Result ping(int timeout_ms = -1);
  Result subscribe(const Subscription& wanted, Subscription& accepted);
  Result acquire_control(bool force, ControlReply& reply);
  Result release_control(ControlReply& reply);
  Result machine_info(MachineInfo& out);
  // Run control.
  Result run_state(RunState& out);
  Result pause(Stamp& stamp);
  Result run(Stamp& stamp);
  Result reset(bool stay_paused, Stamp& stamp);
  // `request_id` is what the step's EVT_STOPPED names as its cause_request.
  Result step(const StepRequest& step, std::uint32_t& request_id);
  // Registers and memory.
  Result regs_get(Stamp& stamp, Regs& out);
  Result regs_set(const RegsSetRequest& request, Stamp& stamp, Regs& after);
  Result mem_info(MemInfo& out);
  Result mem_read(const MemReadRequest& request, Stamp& stamp,
                  std::vector<std::uint8_t>& data);
  Result mem_write(const MemWriteRequest& request, const std::uint8_t* data,
                   std::size_t len, Stamp& stamp);
  // Breakpoints.
  Result bp_list(std::uint64_t& generation, std::vector<BpEntry>& entries);
  Result bp_add(const BpEntry& entry, std::uint64_t& generation);
  Result bp_del(const BpDelRequest& request, std::uint64_t& generation);
  Result bp_clear(std::uint8_t kind, std::uint64_t& generation);
  // Input.
  Result key(const KeyRequest& request);
  Result keys_release();
  Result text(const std::string& text, std::uint32_t& pending);
  Result mouse(const MouseRequest& request);
  Result gun(const GunRequest& request);
  Result input_state(KeyMatrix& rows);
  // DevTools data.
  Result chip_get(Chip chip, ChipState& out, std::uint16_t& layout);
  Result sym_list(std::uint64_t& generation, std::vector<Symbol>& symbols);
  Result sym_add(const Symbol& symbol, std::uint64_t& generation);
  Result sym_del(const std::string& name, std::uint64_t& generation);
  Result sym_load(const std::string& path, std::uint64_t& generation,
                  std::uint32_t& loaded);
  Result fb_info(FbInfo& out);
  // Media, snapshots, configuration.
  Result media_insert(const MediaInsertRequest& request,
                      std::uint64_t& generation);
  Result media_eject(MediaSlot slot, std::uint64_t& generation);
  Result media_info(MediaSlot slot, MediaInfo& out);
  Result tape_control(const TapeControlRequest& request, TapeState& out);
  Result snapshot_save(const std::string& path, Stamp& stamp);
  Result snapshot_load(const SnapshotLoadRequest& request, Stamp& stamp);
  Result config_get(const std::string& key, ConfigValue& out);
  Result config_set(const ConfigSetRequest& request, ConfigEffect& effect);
  // A rebuild may take longer than a request usually does.
  Result config_apply(Stamp& stamp, int timeout_ms = -1);

 private:
  struct Pending {
    Opcode opcode = Opcode::kPing;
    bool async = false;  // `fn` gets the reply; nobody waits for it
    bool done = false;
    Reply reply;
    ReplyFn fn;
  };

  using TimePoint = std::chrono::steady_clock::time_point;

  // How write_frame() ended.
  enum class SendOutcome : std::uint8_t {
    kSent,
    kNotSent,   // nothing of it was taken by the deadline; nothing changed
    kCutShort,  // part of it was: the stream is unusable
    kFailed,    // the socket failed, or the client is stopping
  };

  void reader_loop();
  void dispatch(const FrameHeader& header,
                const std::vector<std::uint8_t>& payload);
  void deliver(const ClientEvent& event);
  // Ends every pending request with kClosed and marks the connection shut.
  void fail_all(const std::string& why);
  // Writes one frame whole, by `deadline`. With `only_if_idle` it does not
  // wait for its turn at the socket, nor for the socket to take anything.
  SendOutcome write_frame(const std::vector<std::uint8_t>& frame,
                          TimePoint deadline, bool only_if_idle);
  void keepalive();
  std::uint32_t next_id();
  void set_close_reason(const std::string& why);
  // A kClosed reply with why the connection ended and the status it ended
  // with (mutex_ held).
  Reply closed_reply_locked() const;
  Reply closed_reply_now();
  // How start() ended.
  enum class Started : std::uint8_t {
    kSent,    // the request is pending
    kUnsent,  // `pending` is done: kTimeout when nothing of it went out,
              // kClosed when the send failed (the connection is then over);
              // its `fn`, if any, has not been called
    kShut,    // the connection was shut already; nothing was registered
  };
  // Registers a request and sends it by `deadline`.
  Started start(Opcode opcode, const std::vector<std::uint8_t>& payload,
                ReplyFn fn, bool async, int timeout_ms, TimePoint deadline,
                bool only_if_idle, std::shared_ptr<Pending>& pending,
                std::uint32_t& id);
  Reply call_with_id(Opcode opcode, const std::vector<std::uint8_t>& payload,
                     int timeout_ms, std::uint32_t& id);
  // A request whose reply is the status header alone, or one `parse` reads;
  // kBadReply when it reads short.
  Result exchange(Opcode opcode, const std::vector<std::uint8_t>& payload,
                  const std::function<bool(Reader&)>& parse = nullptr,
                  int timeout_ms = -1);
  Result control(std::uint8_t action, std::uint8_t flags, ControlReply& reply);

  ClientOptions options_;
  HelloReply hello_;
  net::Socket socket_ = net::kInvalidSocket;
  bool net_started_ = false;
  std::atomic<bool> open_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> hello_done_{false};
  std::atomic<std::uint32_t> controller_{0};
  std::atomic<std::uint32_t> ids_{0};
  // steady_clock nanoseconds of the last frame sent, for the keepalive.
  std::atomic<std::int64_t> last_send_ns_{0};
  std::thread reader_;

  // One frame on the socket at a time; a sender waits for its turn no longer
  // than its deadline, and the keepalive not at all.
  std::timed_mutex send_mutex_;

  mutable std::mutex mutex_;  // everything below
  std::condition_variable cv_;
  std::map<std::uint32_t, std::shared_ptr<Pending>> pending_;
  std::deque<ClientEvent> events_;
  bool closed_ = true;  // no reply will come any more
  Status close_status_ = Status::kOk;
  std::string close_reason_;
  std::atomic<std::uint64_t> dropped_events_{0};
};

}  // namespace kcpc
