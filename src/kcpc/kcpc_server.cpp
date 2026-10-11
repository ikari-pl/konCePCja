#include "kcpc/kcpc_server.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <utility>
#include <vector>

#include "log.h"

namespace kcpc {

namespace {

using Clock = std::chrono::steady_clock;

// How long any thread sleeps in the kernel before it looks at its stop flag
// again. Bounds how late stop() is noticed; nothing spins.
constexpr int kPollMs = 50;
// Time allowed to push a connection's last frames out before giving up on a
// peer that does not read.
constexpr int kFlushMs = 1000;
constexpr int kShutdownFlushMs = 200;
// After the last frame the server half-closes and reads until the peer closes
// too, so the frame is not destroyed by a reset (see net::shutdown_send).
constexpr int kLingerMs = 500;
constexpr int kShutdownLingerMs = 200;
// Connections beyond the client limit get an ERROR frame from a short-lived
// thread pair of their own. Beyond this many at once they are just closed.
constexpr std::size_t kMaxRejectedConnections = 8;

std::atomic<std::uint32_t> g_next_client_id{1};

// Unique for the life of the core and never 0, across stop() and start().
std::uint32_t next_client_id() {
  std::uint32_t id = g_next_client_id.fetch_add(1, std::memory_order_relaxed);
  while (id == 0) {
    id = g_next_client_id.fetch_add(1, std::memory_order_relaxed);
  }
  return id;
}

// A client's name goes into the log: keep it short and printable.
std::string printable(const std::string& name) {
  constexpr std::size_t kMaxLogged = 64;
  std::string out;
  for (const char c : name.substr(0, kMaxLogged)) {
    out += (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  return out;
}

// What a request that the machine refused says in its message.
std::string machine_refusal(Status status) {
  switch (status) {
    case Status::kBusy:
      return "the machine did not go idle in time";
    case Status::kShuttingDown:
      return "the core is shutting down";
    case Status::kOutOfRange:
      return "address, length or bank outside the space";
    case Status::kBadPayload:
      return "unknown memory space, or a bank where none applies";
    case Status::kNotPresent:
      return "the hardware is not fitted";
    default:
      return "the machine could not be read";
  }
}

}  // namespace

// One TCP connection: a reader thread that parses and answers requests, and a
// writer thread that sends what the reader queued. The reader owns the
// connection's life: it starts the writer, joins it and closes the socket.
class Server::Connection {
 public:
  Connection(const ServerConfig& config, net::Socket socket, bool rejected)
      : config_(config), socket_(socket), rejected_(rejected) {}
  ~Connection() { join(); }
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  void start() { reader_ = std::thread(&Connection::run, this); }
  void request_stop() { stop_.store(true, std::memory_order_release); }
  bool finished() const { return finished_.load(std::memory_order_acquire); }
  bool rejected() const { return rejected_; }
  void join() {
    if (reader_.joinable()) reader_.join();
  }

 private:
  // Why the reader stopped serving, and what the peer is told about it.
  struct End {
    bool send_error = false;
    Status status = Status::kOk;
    std::string message;
    int flush_ms = kFlushMs;
    int linger_ms = 0;
  };

  enum class ReadResult : std::uint8_t {
    kOk,
    kGone,          // peer closed, connection failed, or the writer gave up
    kStop,          // the server is stopping
    kHelloTimeout,  // no completed HELLO in time
  };

  static End fatal(Status status, std::string message) {
    End end;
    end.send_error = true;
    end.status = status;
    end.message = std::move(message);
    end.linger_ms = kLingerMs;
    return end;
  }

  void run() {
    hello_deadline_ =
        Clock::now() + std::chrono::milliseconds(config_.hello_timeout_ms);
    std::thread writer(&Connection::write_loop, this);
    End const end =
        rejected_
            ? fatal(Status::kTooManyClients,
                    "the server already has its maximum number of clients")
            : serve();
    if (end.send_error) {
      // ERROR: a response nobody asked for, id 0.
      enqueue(encode_frame(Kind::kResponse,
                           static_cast<std::uint16_t>(Opcode::kError), 0,
                           error_payload(end.status, end.message)),
              true);
    }
    {
      std::scoped_lock const lock(queue_mutex_);
      closing_ = true;
      flush_deadline_ = Clock::now() + std::chrono::milliseconds(end.flush_ms);
    }
    queue_cv_.notify_all();
    writer.join();
    if (end.linger_ms > 0 && !broken_.load(std::memory_order_acquire)) {
      linger(end.linger_ms);
    }
    net::close_socket(socket_);
    if (client_id_ != 0) {
      LOG_INFO("KCPC: client " << client_id_ << " disconnected");
    }
    finished_.store(true, std::memory_order_release);
  }

  // Half-close, then read and discard until the peer closes or time is up.
  void linger(int linger_ms) {
    net::shutdown_send(socket_);
    auto const deadline = Clock::now() + std::chrono::milliseconds(linger_ms);
    std::uint8_t sink[1024];
    while (Clock::now() < deadline) {
      if (!net::wait_readable(socket_, kPollMs)) continue;
      net::IoResult const r = net::recv_some(socket_, sink, sizeof(sink));
      if (r.status == net::IoStatus::kClosed ||
          r.status == net::IoStatus::kError) {
        return;
      }
    }
  }

  ReadResult read_exact(std::uint8_t* buf, std::size_t size) {
    std::size_t got = 0;
    while (got < size) {
      if (stop_.load(std::memory_order_acquire)) return ReadResult::kStop;
      if (broken_.load(std::memory_order_acquire)) return ReadResult::kGone;
      if (!hello_done_ && Clock::now() >= hello_deadline_) {
        return ReadResult::kHelloTimeout;
      }
      if (!net::wait_readable(socket_, kPollMs)) continue;
      net::IoResult const r = net::recv_some(socket_, buf + got, size - got);
      switch (r.status) {
        case net::IoStatus::kOk:
          got += r.bytes;
          break;
        case net::IoStatus::kWouldBlock:
          break;
        case net::IoStatus::kClosed:
        case net::IoStatus::kError:
          return ReadResult::kGone;
      }
    }
    return ReadResult::kOk;
  }

  End end_of_read(ReadResult r) const {
    switch (r) {
      case ReadResult::kStop: {
        End end = fatal(Status::kShuttingDown, "the core is shutting down");
        end.flush_ms = kShutdownFlushMs;
        end.linger_ms = kShutdownLingerMs;
        return end;
      }
      case ReadResult::kHelloTimeout:
        return fatal(Status::kHelloRequired,
                     "no HELLO within " +
                         std::to_string(config_.hello_timeout_ms) + " ms");
      case ReadResult::kOk:
      case ReadResult::kGone:
        break;
    }
    return End{};
  }

  // Reads and answers requests until the connection has to end.
  End serve() {
    std::vector<std::uint8_t> payload;
    for (;;) {
      std::uint8_t raw[kHeaderSize];
      ReadResult r = read_exact(raw, sizeof(raw));
      if (r != ReadResult::kOk) return end_of_read(r);

      FrameHeader header;
      HeaderError const error = parse_header(raw, kMaxRequestPayload, header);
      switch (error) {
        case HeaderError::kOk:
          break;
        case HeaderError::kBadMagic:
          return fatal(Status::kBadFrame, "bad magic: this is the KCPC port");
        case HeaderError::kBadVersion:
          return fatal(Status::kUnsupportedVersion,
                       "this server speaks protocol major " +
                           std::to_string(kVersionMajor));
        case HeaderError::kBadKind:
          return fatal(Status::kBadFrame, "unknown frame kind");
        case HeaderError::kTooLarge:
          return fatal(Status::kBadFrame,
                       "payload longer than " +
                           std::to_string(kMaxRequestPayload) + " bytes");
      }
      if (header.kind != static_cast<std::uint8_t>(Kind::kRequest)) {
        return fatal(Status::kBadFrame, "a client may only send requests");
      }
      if (header.id == 0) {
        return fatal(Status::kBadFrame, "request id 0 is reserved");
      }

      // parse_header() has bounded length at kMaxRequestPayload.
      payload.resize(header.length);
      r = read_exact(payload.data(), payload.size());
      if (r != ReadResult::kOk) return end_of_read(r);

      End end;
      if (!dispatch(header, payload, end)) return end;
      if (overflowed_) return End{};
    }
  }

  // False when the request ends the connection; `end` then says how.
  bool dispatch(const FrameHeader& header,
                const std::vector<std::uint8_t>& payload, End& end) {
    auto const opcode = static_cast<Opcode>(header.opcode);
    if (!hello_done_) {
      if (opcode != Opcode::kHello) {
        end = fatal(Status::kHelloRequired, "the first frame must be HELLO");
        return false;
      }
      handle_hello(header, payload);
      return true;
    }
    switch (opcode) {
      case Opcode::kHello:
        end = fatal(Status::kHelloRequired,
                    "HELLO was already completed on this connection");
        return false;
      case Opcode::kPing: {
        Writer w;
        write_status(w, Status::kOk);
        respond(header, w.take());
        return true;
      }
      case Opcode::kMachineInfo: {
        MachineInfo info;
        Status const status = read_machine(info);
        if (status != Status::kOk) {
          respond_error(header, status, machine_refusal(status));
          return true;
        }
        Writer w;
        write_status(w, Status::kOk);
        write_machine_info(w, info);
        respond(header, w.take());
        return true;
      }
      case Opcode::kRegsGet:
        if (!machine_reads()) break;
        handle_regs_get(header);
        return true;
      case Opcode::kMemInfo:
        if (!machine_reads()) break;
        handle_mem_info(header);
        return true;
      case Opcode::kMemRead:
        if (!machine_reads()) break;
        handle_mem_read(header, payload);
        return true;
      default:
        break;
    }
    respond_error(header, Status::kUnknownOpcode,
                  "opcode not implemented by this server");
    return true;
  }

  // CAP_MACHINE_READ: the group is there whole or not at all.
  bool machine_reads() const {
    return config_.regs_get && config_.mem_info && config_.mem_read;
  }

  void handle_regs_get(const FrameHeader& header) {
    Stamp stamp;
    Regs regs;
    Status const status = config_.regs_get(stamp, regs);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    write_stamp(w, stamp);
    write_regs(w, regs);
    respond(header, w.take());
  }

  void handle_mem_info(const FrameHeader& header) {
    MemInfo info;
    Status const status = config_.mem_info(info);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    write_mem_info(w, info);
    respond(header, w.take());
  }

  void handle_mem_read(const FrameHeader& header,
                       const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    MemReadRequest request;
    if (!read_mem_read_request(r, request)) {
      respond_error(header, Status::kBadPayload, "MEM_READ payload too short");
      return;
    }
    // Before anything is allocated: `len` is the client's word, up to 4 GiB.
    if (request.len > max_frame_ - kMemReadReplyOverhead) {
      respond_error(header, Status::kTooLarge,
                    "the reply would exceed this connection's frame limit of " +
                        std::to_string(max_frame_) + " bytes");
      return;
    }
    // The reply is built in place, so the bytes are copied out of the machine
    // straight into it: status header, stamp, data.
    std::vector<std::uint8_t> reply(kMemReadReplyOverhead + request.len);
    Stamp stamp;
    Status const status =
        config_.mem_read(request, stamp, reply.data() + kMemReadReplyOverhead);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    write_stamp(w, stamp);
    std::copy(w.data().begin(), w.data().end(), reply.begin());
    respond(header, reply);
  }

  // A HELLO that is refused leaves the connection open and not yet a client:
  // the peer may send HELLO again.
  void handle_hello(const FrameHeader& header,
                    const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    HelloRequest request;
    if (!read_hello_request(r, request)) {
      respond_error(header, Status::kBadPayload, "HELLO payload too short");
      return;
    }
    if (request.role > static_cast<std::uint8_t>(Role::kController)) {
      respond_error(header, Status::kBadPayload, "unknown role");
      return;
    }
    if (request.max_frame != 0 && request.max_frame < kMinClientMaxFrame) {
      respond_error(header, Status::kBadPayload,
                    "max_frame below " + std::to_string(kMinClientMaxFrame));
      return;
    }
    HelloReply reply;
    Status const machine = read_machine(reply.machine);
    if (machine != Status::kOk) {
      respond_error(header, machine, machine_refusal(machine));
      return;
    }
    reply.caps = machine_reads() ? (kCapMachineRead | kCapMemRom) : 0;
    // Roles arrive with CONTROL (slice 3); until then everyone observes.
    reply.role = Role::kObserver;
    reply.max_frame = request.max_frame == 0
                          ? kMaxServerFrame
                          : std::min(request.max_frame, kMaxServerFrame);
    reply.client_id = next_client_id();
    reply.pid = config_.pid;
    reply.server_version = config_.server_version;

    client_id_ = reply.client_id;
    max_frame_ = reply.max_frame;
    hello_done_ = true;
    LOG_INFO("KCPC: client " << client_id_ << " connected ("
                             << printable(request.client_name) << ")");
    Writer w;
    write_status(w, Status::kOk);
    write_hello_reply(w, reply);
    respond(header, w.take());
  }

  Status read_machine(MachineInfo& info) const {
    return config_.machine_info ? config_.machine_info(info)
                                : Status::kInternal;
  }

  void respond(const FrameHeader& request,
               const std::vector<std::uint8_t>& payload) {
    enqueue(encode_frame(Kind::kResponse, request.opcode, request.id, payload),
            false);
  }

  void respond_error(const FrameHeader& request, Status status,
                     const std::string& message) {
    respond(request, error_payload(status, message));
  }

  // `always` is for the connection's last frame, which is tiny and must not
  // be lost to the cap that is the reason for sending it.
  void enqueue(std::vector<std::uint8_t> frame, bool always) {
    {
      std::scoped_lock const lock(queue_mutex_);
      if (!always && queued_bytes_ + frame.size() > config_.max_queue_bytes) {
        overflowed_ = true;
        return;
      }
      queued_bytes_ += frame.size();
      queue_.push_back(std::move(frame));
    }
    queue_cv_.notify_one();
  }

  void write_loop() {
    for (;;) {
      std::vector<std::uint8_t> frame;
      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !queue_.empty() || closing_; });
        if (queue_.empty()) return;  // closing, and everything was sent
        frame = std::move(queue_.front());
        queue_.pop_front();
      }
      bool const sent = send_all(frame);
      {
        std::scoped_lock const lock(queue_mutex_);
        queued_bytes_ -= frame.size();
      }
      if (!sent) {
        broken_.store(true, std::memory_order_release);
        return;
      }
    }
  }

  bool send_all(const std::vector<std::uint8_t>& frame) {
    std::size_t sent = 0;
    while (sent < frame.size()) {
      net::IoResult const r =
          net::send_some(socket_, frame.data() + sent, frame.size() - sent);
      if (r.status == net::IoStatus::kOk) {
        sent += r.bytes;
        continue;
      }
      if (r.status != net::IoStatus::kWouldBlock) return false;
      {
        // The peer is not reading. While the connection is being closed that
        // is given a deadline; otherwise the reader's queue cap deals with it.
        std::scoped_lock const lock(queue_mutex_);
        if (closing_ && Clock::now() >= flush_deadline_) return false;
      }
      net::wait_writable(socket_, kPollMs);
    }
    return true;
  }

  const ServerConfig& config_;
  net::Socket socket_;
  const bool rejected_;
  std::thread reader_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> finished_{false};
  // Set by the writer when a send failed; the reader then stops serving.
  std::atomic<bool> broken_{false};

  // Reader thread only.
  bool hello_done_ = false;
  std::uint32_t client_id_ = 0;
  std::uint32_t max_frame_ = kMaxServerFrame;  // the limit HELLO agreed on
  Clock::time_point hello_deadline_;

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<std::vector<std::uint8_t>> queue_;
  std::size_t queued_bytes_ = 0;
  bool overflowed_ = false;  // written and read by the reader only
  bool closing_ = false;
  Clock::time_point flush_deadline_;
};

Server::Server() = default;

Server::~Server() { stop(); }

bool Server::start(const ServerConfig& config) {
  if (running()) return true;
  if (!net::startup()) {
    LOG_ERROR("KCPC: socket startup failed; the binary server is off");
    return false;
  }
  config_ = config;
  for (int p = config_.base_port; p < config_.base_port + config_.probe_count;
       ++p) {
    listener_ = net::listen_loopback(p, 16);
    if (listener_ != net::kInvalidSocket) {
      port_.store(p, std::memory_order_release);
      break;
    }
  }
  if (listener_ == net::kInvalidSocket) {
    LOG_ERROR("KCPC: could not bind to any port in range "
              << config_.base_port << "-"
              << (config_.base_port + config_.probe_count - 1));
    net::cleanup();
    return false;
  }
  stopping_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  acceptor_ = std::thread(&Server::accept_loop, this);
  LOG_INFO("KCPC: listening on port " << port());
  return true;
}

void Server::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  stopping_.store(true, std::memory_order_release);
  if (acceptor_.joinable()) acceptor_.join();
  net::close_socket(listener_);
  listener_ = net::kInvalidSocket;
  port_.store(0, std::memory_order_release);
  {
    // The acceptor is gone, so nothing adds connections any more. Ask them
    // all first, then join: they shut down side by side, not one by one.
    std::scoped_lock const lock(connections_mutex_);
    for (auto& connection : connections_) connection->request_stop();
    for (auto& connection : connections_) connection->join();
    connections_.clear();
  }
  net::cleanup();
}

std::size_t Server::client_count() {
  std::scoped_lock const lock(connections_mutex_);
  return static_cast<std::size_t>(
      std::count_if(connections_.begin(), connections_.end(),
                    [](auto& c) { return !c->rejected() && !c->finished(); }));
}

void Server::accept_loop() {
  while (!stopping_.load(std::memory_order_acquire)) {
    if (net::wait_readable(listener_, kPollMs)) accept_pending();
    reap_finished();
  }
}

void Server::accept_pending() {
  for (;;) {
    net::Socket const socket = net::accept_client(listener_);
    if (socket == net::kInvalidSocket) return;
    // Free the places of connections that have ended before counting.
    reap_finished();
    std::scoped_lock const lock(connections_mutex_);
    std::size_t clients = 0;
    std::size_t rejected = 0;
    for (auto const& connection : connections_) {
      (connection->rejected() ? rejected : clients) += 1;
    }
    bool const reject = clients >= config_.max_clients;
    if (reject && rejected >= kMaxRejectedConnections) {
      net::close_socket(socket);
      continue;
    }
    connections_.push_back(
        std::make_unique<Connection>(config_, socket, reject));
    connections_.back()->start();
  }
}

void Server::reap_finished() {
  std::scoped_lock const lock(connections_mutex_);
  for (auto it = connections_.begin(); it != connections_.end();) {
    if ((*it)->finished()) {
      (*it)->join();
      it = connections_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace kcpc
