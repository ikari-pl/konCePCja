#include "kcpc/kcpc_server.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <numeric>
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
      return "the machine did not go idle in time, was held by another "
             "pause lease, or a step is already in progress";
    case Status::kShuttingDown:
      return "the core is shutting down";
    case Status::kOutOfRange:
      return "address, length or bank outside the space";
    case Status::kBadPayload:
      return "unknown memory space, or a bank where none applies";
    case Status::kNotPresent:
      return "the hardware is not fitted";
    case Status::kNotFound:
      return "no such breakpoint";
    case Status::kLimitReached:
      return "the probe has no comparator left: it holds " +
             std::to_string(kMaxExecBreakpoints) + " execution, " +
             std::to_string(kMaxWatchpoints) + " watch and " +
             std::to_string(kMaxIoBreakpoints) + " I/O comparators";
    default:
      return "the core could not serve the request";
  }
}

}  // namespace

// One TCP connection: a reader thread that parses and answers requests, and a
// writer thread that sends what the reader queued. The reader owns the
// connection's life: it starts the writer, joins it and closes the socket.
class Server::Connection {
 public:
  Connection(Server& owner, net::Socket socket, bool rejected)
      : owner_(owner),
        config_(owner.config_),
        controller_(owner.controller_),
        socket_(socket),
        rejected_(rejected) {}
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

  // The frame interval this client wants EVT_FRAME at, or 0 for none.
  std::uint32_t frame_interval() const {
    return (classes_.load(std::memory_order_acquire) & kEventClassFrame) != 0
               ? frame_interval_.load(std::memory_order_acquire)
               : 0;
  }

  // The fan-out thread's half: queues the event if this client is to have it.
  // `media` is what an EVT_MEDIA_CHANGED says besides its slot.
  void deliver(const Event& event, const MediaNote& media) {
    std::uint32_t const id = event_client_id_.load(std::memory_order_acquire);
    if (id == 0 || !wanted(event, id)) return;
    {
      std::scoped_lock const lock(queue_mutex_);
      if (closing_) return;
      // An event a request of this client caused follows the response to
      // that request, and nothing precedes the reply to HELLO. What happens
      // after a held event waits behind it: the order stays the order.
      if (in_flight_id_ != 0 && (hold_all_ || !held_.empty() ||
                                 (event.cause_client == id &&
                                  event.cause_request == in_flight_id_))) {
        held_.push_back(Held{event, media});
        return;
      }
      queue_event_locked(event, media);
    }
    queue_cv_.notify_one();
  }

  // Events this client never saw because the ring was full.
  void add_dropped(std::uint64_t count) {
    if (event_client_id_.load(std::memory_order_acquire) == 0) return;
    {
      std::scoped_lock const lock(queue_mutex_);
      if (closing_) return;
      dropped_ += count;
      if (queued_bytes_ < config_.max_event_queue_bytes) flush_overrun_locked();
    }
    queue_cv_.notify_one();
  }

 private:
  // Why the reader stopped serving, and what the peer is told about it.
  struct End {
    bool send_error = false;
    Status status = Status::kOk;
    std::string message;
    int flush_ms = kFlushMs;
    int linger_ms = 0;
    bool shutdown = false;  // the core is quitting: EVT_SHUTDOWN goes first
  };

  enum class ReadResult : std::uint8_t {
    kOk,
    kGone,          // peer closed, connection failed, or the writer gave up
    kStop,          // the server is stopping
    kHelloTimeout,  // no completed HELLO in time
    kIdleTimeout,   // the controller sent no frame in time
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
    // A controller that goes leaves control free, and at once: not after the
    // time it takes to deliver its last frame.
    if (client_id_ != 0 && give_up_control()) {
      LOG_INFO("KCPC: control is free: client " << client_id_ << " is leaving");
      owner_.release_held(client_id_);
      control_changed(0, client_id_, kControlLeft);
    }
    {
      // No more events for this connection, and none left behind a request
      // that will not be answered.
      std::scoped_lock const lock(queue_mutex_);
      in_flight_id_ = 0;
      held_.clear();
      if (end.shutdown && hello_done_) {
        Event bye;
        bye.opcode = Opcode::kEvtShutdown;
        push_event_locked(bye);
      }
    }
    event_client_id_.store(0, std::memory_order_release);
    classes_.store(0, std::memory_order_release);
    owner_.update_frame_stride();
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
      if (is_controller() && Clock::now() >= last_frame_ + idle_timeout()) {
        return ReadResult::kIdleTimeout;
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
        end.shutdown = true;
        return end;
      }
      case ReadResult::kHelloTimeout:
        return fatal(Status::kHelloRequired,
                     "no HELLO within " +
                         std::to_string(config_.hello_timeout_ms) + " ms");
      case ReadResult::kIdleTimeout:
        return fatal(Status::kIdleTimeout,
                     "the controller sent no frame for " +
                         std::to_string(config_.controller_idle_timeout_ms) +
                         " ms");
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
      {
        std::scoped_lock const lock(queue_mutex_);
        in_flight_id_ = header.id;
      }
      bool const serving = dispatch(header, payload, end);
      request_done();
      if (!serving) return end;
      if (overflowed_) return End{};
      // The controller's silence is counted from here: the time the core
      // took to serve its request is not the client's.
      last_frame_ = Clock::now();
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
      case Opcode::kSubscribe:
        handle_subscribe(header, payload);
        return true;
      case Opcode::kRunState:
        if (!run_control()) break;
        handle_run_state(header);
        return true;
      case Opcode::kPause:
        if (!run_control()) break;
        if (controller_only(header)) {
          run_request(header, [this](const Cause& cause, Stamp& stamp) {
            return config_.pause(cause, stamp);
          });
        }
        return true;
      case Opcode::kRun:
        if (!run_control()) break;
        if (controller_only(header)) {
          run_request(header, [this](const Cause& cause, Stamp& stamp) {
            return config_.run(cause, stamp);
          });
        }
        return true;
      case Opcode::kReset:
        if (!run_control()) break;
        if (controller_only(header)) handle_reset(header, payload);
        return true;
      case Opcode::kStep:
        if (!run_control()) break;
        if (controller_only(header)) handle_step(header, payload);
        return true;
      case Opcode::kBpList:
        if (!breakpoints()) break;
        handle_bp_list(header);
        return true;
      case Opcode::kBpAdd:
        if (!breakpoints()) break;
        if (controller_only(header)) handle_bp_add(header, payload);
        return true;
      case Opcode::kBpDel:
        if (!breakpoints()) break;
        if (controller_only(header)) handle_bp_del(header, payload);
        return true;
      case Opcode::kBpClear:
        if (!breakpoints()) break;
        if (controller_only(header)) handle_bp_clear(header, payload);
        return true;
      case Opcode::kKey:
        if (!input()) break;
        if (controller_only(header)) handle_key(header, payload);
        return true;
      case Opcode::kKeysRelease:
        if (!input()) break;
        if (controller_only(header)) {
          respond_input(header, owner_.release_all_keys(client_id_));
        }
        return true;
      case Opcode::kText:
        if (!input()) break;
        if (controller_only(header)) handle_text(header, payload);
        return true;
      case Opcode::kMouse:
        if (!input()) break;
        if (controller_only(header)) handle_mouse(header, payload);
        return true;
      case Opcode::kInputState:
        if (!input()) break;
        handle_input_state(header);
        return true;
      case Opcode::kChipGet:
        if (!chips()) break;
        handle_chip_get(header, payload);
        return true;
      case Opcode::kSymList:
        if (!symbols()) break;
        handle_sym_list(header);
        return true;
      case Opcode::kSymAdd:
        if (!symbols()) break;
        if (controller_only(header)) handle_sym_add(header, payload);
        return true;
      case Opcode::kSymDel:
        if (!symbols()) break;
        if (controller_only(header)) handle_sym_del(header, payload);
        return true;
      case Opcode::kSymLoad:
        if (!symbols()) break;
        if (controller_only(header)) handle_sym_load(header, payload);
        return true;
      case Opcode::kFbInfo:
        if (!config_.fb_info) break;
        handle_fb_info(header);
        return true;
      case Opcode::kControl:
        if (!machine_writes()) break;
        handle_control(header, payload);
        return true;
      case Opcode::kRegsSet:
        if (!machine_writes()) break;
        if (controller_only(header)) handle_regs_set(header, payload);
        return true;
      case Opcode::kMemWrite:
        if (!machine_writes()) break;
        if (controller_only(header)) handle_mem_write(header, payload);
        return true;
      default:
        break;
    }
    respond_error(header, Status::kUnknownOpcode,
                  "opcode not implemented by this server");
    return true;
  }

  // CAP_MACHINE_WRITE, and the control role that comes with it.
  bool machine_writes() const { return config_.regs_set && config_.mem_write; }

  // CAP_RUN_CONTROL: the group is there whole or not at all.
  bool run_control() const {
    return config_.run_state && config_.pause && config_.run && config_.reset &&
           config_.step;
  }

  void handle_subscribe(const FrameHeader& header,
                        const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    Subscription wanted;
    if (!read_subscription(r, wanted)) {
      respond_error(header, Status::kBadPayload, "SUBSCRIBE payload too short");
      return;
    }
    // Class bits this server does not know are not an error: a later minor
    // version adds classes, and the reply says what was accepted.
    wanted.classes &= kEventClassAll;
    classes_.store(wanted.classes, std::memory_order_release);
    frame_interval_.store(wanted.frame_interval, std::memory_order_release);
    owner_.update_frame_stride();
    Writer w;
    write_status(w, Status::kOk);
    write_subscription(w, wanted);
    respond(header, w.take());
  }

  void handle_run_state(const FrameHeader& header) {
    RunState state;
    Status const status = config_.run_state(state.stamp, state.walk_active);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    state.controller = controller_.load(std::memory_order_acquire);
    Writer w;
    write_status(w, Status::kOk);
    write_run_state(w, state);
    respond(header, w.take());
  }

  // PAUSE and RUN: no payload, a stamp back.
  template <typename Call>
  void run_request(const FrameHeader& header, Call&& call) {
    Stamp stamp;
    Status const status = call(Cause{client_id_, header.id}, stamp);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    write_stamp(w, stamp);
    respond(header, w.take());
  }

  void handle_reset(const FrameHeader& header,
                    const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    std::uint8_t flags = 0;
    r.u8(flags);
    if (!r.skip(3)) {
      respond_error(header, Status::kBadPayload, "RESET payload too short");
      return;
    }
    bool const stay_paused = (flags & kResetFlagStayPaused) != 0;
    run_request(header, [&](const Cause& cause, Stamp& stamp) {
      return config_.reset(cause, stay_paused, stamp);
    });
  }

  void handle_step(const FrameHeader& header,
                   const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    StepRequest request;
    if (!read_step_request(r, request)) {
      respond_error(header, Status::kBadPayload, "STEP payload too short");
      return;
    }
    if (request.mode > static_cast<std::uint8_t>(StepMode::kFrames)) {
      respond_error(header, Status::kBadPayload, "unknown STEP mode");
      return;
    }
    auto const mode = static_cast<StepMode>(request.mode);
    if (step_mode_walks(mode) && !breakpoints()) {
      respond_error(header, Status::kBadPayload,
                    "STEP over, out and to come with CAP_BREAKPOINTS");
      return;
    }
    if (request.count == 0) request.count = 1;
    if (mode == StepMode::kIn && request.count > kMaxStepInCount) {
      respond_error(header, Status::kOutOfRange,
                    "at most " + std::to_string(kMaxStepInCount) +
                        " instructions in one STEP in; step over, or run to "
                        "an address, to go further");
      return;
    }
    if (mode == StepMode::kFrames && request.count > kMaxStepFrames) {
      respond_error(header, Status::kOutOfRange, "too many frames");
      return;
    }
    if (step_mode_walks(mode) && request.timeout_ms == 0) {
      request.timeout_ms = kDefaultStepTimeoutMs;
    }
    Status const status = config_.step(Cause{client_id_, header.id}, request);
    if (status != Status::kOk) {
      respond_error(header, status,
                    status == Status::kLimitReached
                        ? "a step that runs to an address needs an execution "
                          "comparator, and breakpoints hold all " +
                              std::to_string(kMaxExecBreakpoints)
                        : machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    respond(header, w.take());
  }

  // CAP_BREAKPOINTS: the group is there whole or not at all.
  bool breakpoints() const {
    return config_.bp_list && config_.bp_add && config_.bp_del &&
           config_.bp_clear;
  }

  void respond_generation(const FrameHeader& header, std::uint64_t generation) {
    Writer w;
    write_status(w, Status::kOk);
    w.u64(generation);
    respond(header, w.take());
  }

  void handle_bp_list(const FrameHeader& header) {
    std::uint64_t generation = 0;
    std::vector<BpEntry> entries;
    Status const status = config_.bp_list(generation, entries);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    w.u64(generation);
    w.u32(static_cast<std::uint32_t>(entries.size()));
    for (const BpEntry& entry : entries) write_bp_entry(w, entry);
    if (w.size() > max_frame_) {
      respond_error(header, Status::kTooLarge,
                    "the list would exceed this connection's frame limit of " +
                        std::to_string(max_frame_) + " bytes");
      return;
    }
    respond(header, w.take());
  }

  void handle_bp_add(const FrameHeader& header,
                     const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    BpEntry entry;
    if (!read_bp_entry(r, entry)) {
      respond_error(header, Status::kBadPayload, "BP_ADD payload too short");
      return;
    }
    if (const char* const refusal = bp_entry_refusal(entry)) {
      respond_error(header, Status::kBadPayload, refusal);
      return;
    }
    if (static_cast<BpKind>(entry.kind) == BpKind::kWatch &&
        !bp_watch_range_ok(entry)) {
      respond_error(header, Status::kOutOfRange,
                    "the watched range passes the end of the address space");
      return;
    }
    std::uint64_t generation = 0;
    std::string message;
    Status const status = config_.bp_add(Cause{client_id_, header.id}, entry,
                                         generation, message);
    if (status != Status::kOk) {
      respond_error(header, status,
                    message.empty() ? machine_refusal(status) : message);
      return;
    }
    respond_generation(header, generation);
  }

  void handle_bp_del(const FrameHeader& header,
                     const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    BpDelRequest request;
    if (!read_bp_del_request(r, request)) {
      respond_error(header, Status::kBadPayload, "BP_DEL payload too short");
      return;
    }
    if (request.kind > static_cast<std::uint8_t>(BpKind::kIo)) {
      respond_error(header, Status::kBadPayload, "unknown breakpoint kind");
      return;
    }
    std::uint64_t generation = 0;
    Status const status =
        config_.bp_del(Cause{client_id_, header.id}, request, generation);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    respond_generation(header, generation);
  }

  void handle_bp_clear(const FrameHeader& header,
                       const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    std::uint8_t kind = 0;
    if (!r.u8(kind)) {
      respond_error(header, Status::kBadPayload, "BP_CLEAR payload too short");
      return;
    }
    if (kind != kBpKindAll && kind > static_cast<std::uint8_t>(BpKind::kIo)) {
      respond_error(header, Status::kBadPayload, "unknown breakpoint kind");
      return;
    }
    std::uint64_t generation = 0;
    Status const status =
        config_.bp_clear(Cause{client_id_, header.id}, kind, generation);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    respond_generation(header, generation);
  }

  // CAP_CHIPS: CHIP_GET, and with it the memory space ASIC_PAGE, which the
  // machine reads serve.
  bool chips() const { return machine_reads() && config_.chip_get; }

  void handle_chip_get(const FrameHeader& header,
                       const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    ChipState state;
    if (!r.u8(state.chip) || !r.skip(3)) {
      respond_error(header, Status::kBadPayload, "CHIP_GET payload too short");
      return;
    }
    if (!chip_known(state.chip)) {
      respond_error(header, Status::kBadPayload, "unknown chip");
      return;
    }
    Status const status = config_.chip_get(state);
    if (status != Status::kOk) {
      respond_error(header, status,
                    status == Status::kNotPresent
                        ? "this machine has no such chip: the ASIC is a "
                          "6128+'s"
                        : machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    write_chip_reply(w, state);
    respond(header, w.take());
  }

  // CAP_SYMBOLS: the group is there whole or not at all. The edits need the
  // control role.
  bool symbols() const {
    return machine_writes() && config_.sym_list && config_.sym_add &&
           config_.sym_del && config_.sym_load;
  }

  void handle_sym_list(const FrameHeader& header) {
    std::uint64_t generation = 0;
    std::vector<Symbol> list;
    Status const status = config_.sym_list(generation, list);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    w.u64(generation);
    w.u32(static_cast<std::uint32_t>(list.size()));
    for (const Symbol& symbol : list) write_symbol(w, symbol);
    if (w.size() > max_frame_) {
      respond_error(header, Status::kTooLarge,
                    "the list would exceed this connection's frame limit of " +
                        std::to_string(max_frame_) + " bytes");
      return;
    }
    respond(header, w.take());
  }

  void handle_sym_add(const FrameHeader& header,
                      const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    Symbol symbol;
    if (!read_symbol(r, symbol)) {
      respond_error(header, Status::kBadPayload, "SYM_ADD payload too short");
      return;
    }
    if (const char* const refusal = symbol_name_refusal(symbol.name)) {
      respond_error(header, Status::kBadPayload, refusal);
      return;
    }
    std::uint64_t generation = 0;
    Status const status =
        config_.sym_add(Cause{client_id_, header.id}, symbol, generation);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    respond_generation(header, generation);
  }

  void handle_sym_del(const FrameHeader& header,
                      const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    std::string name;
    if (!r.str(name)) {
      respond_error(header, Status::kBadPayload, "SYM_DEL payload too short");
      return;
    }
    std::uint64_t generation = 0;
    Status const status =
        config_.sym_del(Cause{client_id_, header.id}, name, generation);
    if (status != Status::kOk) {
      respond_error(header, status,
                    status == Status::kNotFound ? "no symbol of that name"
                                                : machine_refusal(status));
      return;
    }
    respond_generation(header, generation);
  }

  void handle_sym_load(const FrameHeader& header,
                       const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    std::string path;
    if (!r.str(path)) {
      respond_error(header, Status::kBadPayload, "SYM_LOAD payload too short");
      return;
    }
    if (path.empty()) {
      respond_error(header, Status::kBadPayload, "SYM_LOAD needs a path");
      return;
    }
    std::uint64_t generation = 0;
    std::uint32_t loaded = 0;
    Status const status = config_.sym_load(Cause{client_id_, header.id}, path,
                                           generation, loaded);
    if (status != Status::kOk) {
      respond_error(header, status,
                    status == Status::kNotFound
                        ? "the symbol file could not be opened"
                        : machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    w.u64(generation);
    w.u32(loaded);
    respond(header, w.take());
  }

  // CAP_FB_INFO.
  void handle_fb_info(const FrameHeader& header) {
    FbInfo info;
    Status const status = config_.fb_info(info);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    write_fb_info(w, info);
    respond(header, w.take());
  }

  // CAP_INPUT: the group is there whole or not at all. It needs the control
  // role, which comes with the machine writes.
  bool input() const {
    return machine_writes() && config_.key && config_.keys_release &&
           config_.text && config_.mouse && config_.input_state;
  }

  // The reply to KEY, KEYS_RELEASE and MOUSE: the status alone.
  void respond_input(const FrameHeader& header, Status status) {
    if (status == Status::kNotController) {
      // Control was taken between the request's start and the change.
      refuse_observer(header);
      return;
    }
    if (status != Status::kOk) {
      respond_error(header, status,
                    status == Status::kNotPresent
                        ? "no AMX or Symbiface mouse is enabled"
                        : machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    respond(header, w.take());
  }

  void handle_key(const FrameHeader& header,
                  const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    KeyRequest request;
    if (!read_key_request(r, request)) {
      respond_error(header, Status::kBadPayload, "KEY payload too short");
      return;
    }
    if (request.down > 1) {
      respond_error(header, Status::kBadPayload,
                    "down must be 1 (press) or 0 (release)");
      return;
    }
    if (request.row >= kKeyRows || request.bit >= kKeyBits) {
      respond_error(header, Status::kOutOfRange,
                    "the keyboard matrix has rows 0 to 9 and bits 0 to 7");
      return;
    }
    respond_input(header, owner_.press_key(client_id_, request));
  }

  void handle_text(const FrameHeader& header,
                   const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    std::string text;
    if (!r.str(text)) {
      respond_error(header, Status::kBadPayload, "TEXT payload too short");
      return;
    }
    std::uint32_t pending = 0;
    std::string message;
    Status const status = config_.text(text, pending, message);
    if (status != Status::kOk) {
      respond_error(header, status,
                    message.empty() ? machine_refusal(status) : message);
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    w.u32(pending);
    respond(header, w.take());
  }

  void handle_mouse(const FrameHeader& header,
                    const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    MouseRequest request;
    if (!read_mouse_request(r, request)) {
      respond_error(header, Status::kBadPayload, "MOUSE payload too short");
      return;
    }
    respond_input(header, owner_.move_mouse(client_id_, request));
  }

  void handle_input_state(const FrameHeader& header) {
    KeyMatrix rows{};
    Status const status = config_.input_state(rows);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    w.bytes(rows.data(), rows.size());
    respond(header, w.take());
  }

  // Tells every client who has control now. Sent after the response to the
  // request that changed it, when one did.
  void control_changed(std::uint32_t controller, std::uint32_t previous,
                       std::uint8_t reason, std::uint32_t request = 0) {
    Event event;
    event.opcode = Opcode::kEvtControlChanged;
    event.controller = controller;
    event.previous = previous;
    event.reason = reason;
    event.cause_client = client_id_;
    event.cause_request = request;
    owner_.post(event);
  }

  bool is_controller() const {
    return client_id_ != 0 &&
           controller_.load(std::memory_order_acquire) == client_id_;
  }

  std::chrono::milliseconds idle_timeout() const {
    return std::chrono::milliseconds(config_.controller_idle_timeout_ms);
  }

  // Takes control if nobody has it. True when this client now holds it.
  bool take_free_control(std::uint32_t request) {
    std::uint32_t holder = 0;
    if (controller_.compare_exchange_strong(holder, client_id_,
                                            std::memory_order_acq_rel)) {
      LOG_INFO("KCPC: client " << client_id_ << " has control");
      control_changed(client_id_, 0, kControlAcquired, request);
      return true;
    }
    return holder == client_id_;
  }

  // True when this client had control and it is now free.
  bool give_up_control() {
    std::uint32_t holder = client_id_;
    return controller_.compare_exchange_strong(holder, 0,
                                               std::memory_order_acq_rel);
  }

  // Answers NOT_CONTROLLER and returns false for an observer. Asked when the
  // request starts: one that was queued behind a takeover is refused.
  bool controller_only(const FrameHeader& header) {
    if (is_controller()) return true;
    refuse_observer(header);
    return false;
  }

  void refuse_observer(const FrameHeader& header) {
    respond_error(header, Status::kNotController,
                  "this request changes the machine and needs control; ask "
                  "for it with CONTROL");
  }

  void handle_control(const FrameHeader& header,
                      const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    ControlRequest request;
    if (!read_control_request(r, request)) {
      respond_error(header, Status::kBadPayload, "CONTROL payload too short");
      return;
    }
    if (request.action == kControlRelease) {
      if (give_up_control()) {
        LOG_INFO("KCPC: client " << client_id_ << " released control");
        owner_.release_held(client_id_);
        control_changed(0, client_id_, kControlReleased, header.id);
      }
    } else if (request.action == kControlAcquire) {
      if (!take_free_control(header.id) &&
          (request.flags & kControlFlagForce) != 0) {
        // Takeover: the newest request wins. The previous controller stays
        // connected as an observer, and learns it from the event.
        std::uint32_t const previous =
            controller_.exchange(client_id_, std::memory_order_acq_rel);
        LOG_INFO("KCPC: client " << client_id_ << " took control from client "
                                 << previous);
        // A demoted controller cannot let go of its keys any more.
        if (previous != 0) owner_.release_held(previous);
        control_changed(client_id_, previous,
                        previous == 0 ? kControlAcquired : kControlTaken,
                        header.id);
      }
    } else {
      respond_error(header, Status::kBadPayload, "unknown CONTROL action");
      return;
    }
    ControlReply reply;
    reply.controller = controller_.load(std::memory_order_acquire);
    reply.role =
        reply.controller == client_id_ ? Role::kController : Role::kObserver;
    Writer w;
    write_status(w, Status::kOk);
    write_control_reply(w, reply);
    respond(header, w.take());
  }

  void handle_regs_set(const FrameHeader& header,
                       const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    RegsSetRequest request;
    if (!read_regs_set_request(r, request)) {
      respond_error(header, Status::kBadPayload, "REGS_SET payload too short");
      return;
    }
    if (const char* const refusal = regs_set_refusal(request)) {
      respond_error(header, Status::kBadPayload, refusal);
      return;
    }
    Stamp stamp;
    Regs after;
    Status const status =
        config_.regs_set(request.mask, request.regs, stamp, after);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    write_stamp(w, stamp);
    write_regs(w, after);
    respond(header, w.take());
  }

  void handle_mem_write(const FrameHeader& header,
                        const std::vector<std::uint8_t>& payload) {
    Reader r(payload);
    MemWriteRequest request;
    if (!read_mem_write_request(r, request)) {
      respond_error(header, Status::kBadPayload, "MEM_WRITE payload too short");
      return;
    }
    if (!mem_space_writable(request.space)) {
      respond_error(header, Status::kBadPayload,
                    "only the spaces CPU_RAM and PHYS_RAM can be written");
      return;
    }
    if (request.bank != 0) {
      respond_error(header, Status::kBadPayload,
                    "bank must be 0 for this space");
      return;
    }
    Stamp stamp;
    Status const status =
        config_.mem_write(request, payload.data() + kMemWriteHeaderSize,
                          payload.size() - kMemWriteHeaderSize, stamp);
    if (status != Status::kOk) {
      respond_error(header, status, machine_refusal(status));
      return;
    }
    Writer w;
    write_status(w, Status::kOk);
    write_stamp(w, stamp);
    respond(header, w.take());
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
    reply.caps = kCapEvents;
    if (machine_reads()) reply.caps |= kCapMachineRead | kCapMemRom;
    if (machine_writes()) reply.caps |= kCapMachineWrite;
    if (run_control()) reply.caps |= kCapRunControl;
    if (breakpoints()) reply.caps |= kCapBreakpoints;
    if (input()) reply.caps |= kCapInput;
    if (chips()) reply.caps |= kCapChips;
    if (symbols()) reply.caps |= kCapSymbols;
    if (config_.fb_info) reply.caps |= kCapFbInfo;
    reply.max_frame = request.max_frame == 0
                          ? kMaxServerFrame
                          : std::min(request.max_frame, kMaxServerFrame);
    reply.client_id = next_client_id();
    reply.pid = config_.pid;
    reply.server_version = config_.server_version;

    client_id_ = reply.client_id;
    max_frame_ = reply.max_frame;
    hello_done_ = true;
    // HELLO never takes control away from anyone: a client that asks for it
    // while another has it is admitted as an observer, and the reply says so.
    last_frame_ = Clock::now();
    {
      // From here the fan-out thread may have events for this client. They
      // wait behind the reply: nothing is sent before HELLO is answered.
      std::scoped_lock const lock(queue_mutex_);
      hold_all_ = true;
    }
    event_client_id_.store(client_id_, std::memory_order_release);
    if (machine_writes() &&
        request.role == static_cast<std::uint8_t>(Role::kController) &&
        take_free_control(header.id)) {
      reply.role = Role::kController;
    }
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
    request_done();
  }

  // The request in flight has been answered (or will not be): the events it
  // caused, which were kept back for the response to go first, follow now.
  void request_done() {
    {
      std::scoped_lock const lock(queue_mutex_);
      if (in_flight_id_ == 0) return;
      in_flight_id_ = 0;
      hold_all_ = false;
      for (const Held& held : held_) {
        queue_event_locked(held.event, held.media);
      }
      held_.clear();
    }
    queue_cv_.notify_one();
  }

  bool wanted(const Event& event, std::uint32_t id) const {
    std::uint32_t const bit = event_class(event.opcode);
    if (bit == 0) return true;  // SHUTDOWN, OVERRUN, CONTROL_CHANGED
    // The end of a step always reaches the client that asked for the step.
    if (event.opcode == Opcode::kEvtStopped && event.step_client == id) {
      return true;
    }
    if ((classes_.load(std::memory_order_acquire) & bit) == 0) return false;
    if (event.opcode != Opcode::kEvtFrame) return true;
    std::uint32_t const interval =
        frame_interval_.load(std::memory_order_acquire);
    return interval != 0 && event.frame % interval == 0;
  }

  // Events go in only while the queue is under its event cap. Beyond it they
  // are counted, and the client is told how many once there is room.
  void queue_event_locked(const Event& event, const MediaNote& media = {}) {
    if (queued_bytes_ >= config_.max_event_queue_bytes) {
      ++dropped_;
      return;
    }
    flush_overrun_locked();
    push_event_locked(event, media);
  }

  void flush_overrun_locked() {
    if (dropped_ == 0) return;
    Event overrun;
    overrun.opcode = Opcode::kEvtOverrun;
    overrun.dropped = dropped_ > 0xFFFFFFFFULL
                          ? 0xFFFFFFFFU
                          : static_cast<std::uint32_t>(dropped_);
    dropped_ = 0;
    push_event_locked(overrun);
  }

  // The event's id is its place among the events of this connection.
  void push_event_locked(const Event& event, const MediaNote& media = {}) {
    std::vector<std::uint8_t> frame =
        encode_frame(Kind::kEvent, static_cast<std::uint16_t>(event.opcode),
                     next_event_id_++, encode_event(event, media));
    queued_bytes_ += frame.size();
    queue_.push_back(std::move(frame));
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
        if (!closing_ && queued_bytes_ < config_.max_event_queue_bytes) {
          flush_overrun_locked();  // there is room again: say what was lost
        }
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

  Server& owner_;
  const ServerConfig& config_;
  std::atomic<std::uint32_t>& controller_;  // the server's; see Server
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
  // When this client's last request had been answered: where a controller's
  // silence is counted from.
  Clock::time_point last_frame_;

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<std::vector<std::uint8_t>> queue_;
  std::size_t queued_bytes_ = 0;
  bool overflowed_ = false;  // written and read by the reader only
  bool closing_ = false;
  Clock::time_point flush_deadline_;

  // Events. The subscription is written by the reader and read by the
  // fan-out thread; event_client_id_ is client_id_ once HELLO has completed
  // and 0 before that and at the end, when no event is wanted.
  std::atomic<std::uint32_t> event_client_id_{0};
  std::atomic<std::uint32_t> classes_{0};
  std::atomic<std::uint32_t> frame_interval_{0};
  // Under queue_mutex_.
  std::uint32_t next_event_id_ = 1;
  std::uint64_t dropped_ = 0;       // events not queued since the last OVERRUN
  std::uint32_t in_flight_id_ = 0;  // the request being served; 0 = none
  bool hold_all_ = false;           // HELLO is being served
  struct Held {
    Event event;
    MediaNote media;
  };
  std::vector<Held> held_;  // waiting for the response to go first
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
  frame_stride_.store(0, std::memory_order_release);
  {
    std::scoped_lock const lock(held_mutex_);
    held_owner_ = 0;
    held_keys_.fill(0);
    held_buttons_ = 0;
  }
  {
    // Whatever was posted to a server that has since stopped is history.
    Event stale;
    while (ring_.pop(stale)) {
    }
    ring_.take_lost();
  }
  running_.store(true, std::memory_order_release);
  fan_out_ = std::thread(&Server::fan_out_loop, this);
  acceptor_ = std::thread(&Server::accept_loop, this);
  LOG_INFO("KCPC: listening on port " << port());
  return true;
}

void Server::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  stopping_.store(true, std::memory_order_release);
  if (acceptor_.joinable()) acceptor_.join();
  {
    std::scoped_lock const lock(fan_out_mutex_);
  }
  fan_out_cv_.notify_all();
  if (fan_out_.joinable()) fan_out_.join();
  net::close_socket(listener_);
  listener_ = net::kInvalidSocket;
  port_.store(0, std::memory_order_release);
  // The acceptor and the fan-out thread are gone, so nothing adds or walks
  // connections any more. Ask them all first, then join: they shut down side
  // by side, not one by one. Joined outside the lock, because a connection
  // on its way out takes it (update_frame_stride()).
  std::list<std::unique_ptr<Connection>> leaving;
  {
    std::scoped_lock const lock(connections_mutex_);
    leaving.swap(connections_);
  }
  for (auto& connection : leaving) connection->request_stop();
  for (auto& connection : leaving) connection->join();
  leaving.clear();
  frame_stride_.store(0, std::memory_order_release);
  net::cleanup();
}

// The three changes below are made under held_mutex_, and only while the
// client is the controller. A client that loses control has what it held
// released under the same mutex, after controller_ has changed: so a KEY that
// was already past its role check either lands before that release, and is
// undone by it, or finds it is no longer the controller.
Status Server::press_key(std::uint32_t client, const KeyRequest& key) {
  std::scoped_lock const lock(held_mutex_);
  if (controller_.load(std::memory_order_acquire) != client) {
    return Status::kNotController;
  }
  // What a previous controller left is not this client's to keep down.
  if (held_owner_ != client) release_held_locked();
  held_owner_ = client;
  bool const down = key.down != 0;
  Status const status = config_.key(key.row, key.bit, down);
  if (status != Status::kOk) return status;
  auto const mask = static_cast<std::uint8_t>(1U << key.bit);
  if (down) {
    held_keys_[key.row] |= mask;
  } else {
    held_keys_[key.row] &= static_cast<std::uint8_t>(~mask);
  }
  return Status::kOk;
}

Status Server::release_all_keys(std::uint32_t client) {
  std::scoped_lock const lock(held_mutex_);
  if (controller_.load(std::memory_order_acquire) != client) {
    return Status::kNotController;
  }
  Status const status = config_.keys_release();
  if (status == Status::kOk) held_keys_.fill(0);
  return status;
}

Status Server::move_mouse(std::uint32_t client, const MouseRequest& mouse) {
  std::scoped_lock const lock(held_mutex_);
  if (controller_.load(std::memory_order_acquire) != client) {
    return Status::kNotController;
  }
  if (held_owner_ != client) release_held_locked();
  held_owner_ = client;
  bool const set_buttons = (mouse.flags & kMouseFlagButtons) != 0;
  auto const buttons =
      static_cast<std::uint8_t>(mouse.buttons & kMouseButtonsAll);
  Status const status = config_.mouse(mouse.dx, mouse.dy, set_buttons, buttons);
  if (status == Status::kOk && set_buttons) held_buttons_ = buttons;
  return status;
}

void Server::release_held(std::uint32_t client) {
  std::scoped_lock const lock(held_mutex_);
  if (held_owner_ == client) release_held_locked();
}

void Server::release_held_locked() {
  for (std::uint8_t row = 0; row < kKeyRows; ++row) {
    for (std::uint8_t bit = 0; bit < kKeyBits; ++bit) {
      if ((held_keys_[row] & (1U << bit)) != 0) config_.key(row, bit, false);
    }
  }
  // A mouse that has been unplugged meanwhile has no buttons to let go of.
  if (held_buttons_ != 0) config_.mouse(0, 0, true, 0);
  held_keys_.fill(0);
  held_buttons_ = 0;
  held_owner_ = 0;
}

void Server::post(const Event& event) {
  if (!running_.load(std::memory_order_acquire)) return;
  ring_.push(event);
  // Wake the fan-out thread only if it sleeps. It holds its mutex for the
  // one check it makes before sleeping, so this waits for nothing longer.
  if (fan_out_sleeping_.exchange(false, std::memory_order_acq_rel)) {
    {
      std::scoped_lock const lock(fan_out_mutex_);
    }
    fan_out_cv_.notify_one();
  }
}

void Server::fan_out_loop() {
  Event event;
  for (;;) {
    while (ring_.pop(event)) {
      // What the slot holds is asked for here, not where the event was
      // posted: that may have been the frame runner, and a path is no
      // fixed-size field. An event is a notice; it says what is true now.
      MediaNote media;
      if (event.opcode == Opcode::kEvtMediaChanged && config_.media_state) {
        config_.media_state(event.slot, media);
      }
      std::scoped_lock const lock(connections_mutex_);
      for (auto& connection : connections_) connection->deliver(event, media);
    }
    if (std::uint64_t const lost = ring_.take_lost()) {
      std::scoped_lock const lock(connections_mutex_);
      for (auto& connection : connections_) connection->add_dropped(lost);
    }
    std::unique_lock lock(fan_out_mutex_);
    fan_out_sleeping_.store(true, std::memory_order_seq_cst);
    if (!ring_.empty()) {
      fan_out_sleeping_.store(false, std::memory_order_release);
      continue;
    }
    if (stopping_.load(std::memory_order_acquire)) return;
    fan_out_cv_.wait_for(lock, std::chrono::milliseconds(kPollMs));
    fan_out_sleeping_.store(false, std::memory_order_release);
  }
}

void Server::update_frame_stride() {
  std::scoped_lock const lock(connections_mutex_);
  std::uint32_t stride = 0;
  for (auto const& connection : connections_) {
    stride = std::gcd(stride, connection->frame_interval());
  }
  frame_stride_.store(stride, std::memory_order_release);
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
    connections_.push_back(std::make_unique<Connection>(*this, socket, reject));
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
