#include "kcpc/kcpc_client.h"

#include <chrono>
#include <utility>

namespace kcpc {

namespace {

// Every wait is cut into slices this long, so close() is never held up by a
// quiet server (as in net_socket.h and the server).
constexpr int kSliceMs = 200;

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

Reply closed_reply(const std::string& why) {
  Reply r;
  r.outcome = Outcome::kClosed;
  r.message = why;
  return r;
}

std::vector<std::uint8_t> one_str(const std::string& s) {
  Writer w;
  w.str(s);
  return w.take();
}

std::vector<std::uint8_t> slot_payload(std::uint8_t slot) {
  Writer w;
  write_media_slot_request(w, slot);
  return w.take();
}

}  // namespace

Client::~Client() { close(); }

bool Client::connect(int port, const ClientOptions& options, Result& result) {
  close();
  options_ = options;
  hello_ = HelloReply{};
  if (!net_started_) net_started_ = net::startup();
  if (!net_started_) {
    result = closed_reply("socket setup failed");
    return false;
  }
  socket_ = net::connect_loopback(port, options_.connect_timeout_ms);
  if (socket_ == net::kInvalidSocket) {
    result = closed_reply("cannot connect to 127.0.0.1:" +
                          std::to_string(port));
    return false;
  }
  {
    std::scoped_lock const lock(mutex_);
    pending_.clear();
    events_.clear();
    closed_ = false;
    close_status_ = Status::kOk;
    close_reason_.clear();
  }
  stop_ = false;
  hello_done_ = false;
  controller_ = 0;
  ids_ = 0;
  dropped_events_ = 0;
  last_send_ns_ = now_ns();
  open_ = true;
  reader_ = std::thread([this] { reader_loop(); });

  HelloRequest hello;
  hello.client_minor = kVersionMinor;
  hello.role = static_cast<std::uint8_t>(options_.role);
  hello.max_frame = options_.max_frame;
  hello.want_caps = options_.want_caps;
  hello.client_name = options_.name;
  Writer w;
  write_hello_request(w, hello);
  Reply const reply = call(Opcode::kHello, w.data());
  result = reply;
  if (reply.ok() && !hello_done_.load(std::memory_order_acquire)) {
    result.outcome = Outcome::kBadReply;
    result.message = "HELLO reply too short";
  }
  if (!result.ok()) {
    close();
    return false;
  }
  return true;
}

void Client::close() {
  stop_ = true;
  if (reader_.joinable()) {
    // From a callback the reader cannot join itself: it stops once the
    // callback returns, and whoever closes next joins it.
    if (std::this_thread::get_id() == reader_.get_id()) return;
    reader_.join();
  }
  fail_all("closed by the client");
  {
    std::scoped_lock const lock(send_mutex_);
    if (socket_ != net::kInvalidSocket) net::close_socket(socket_);
    socket_ = net::kInvalidSocket;
  }
  if (net_started_) net::cleanup();
  net_started_ = false;
}

Status Client::close_status() const {
  std::scoped_lock const lock(mutex_);
  return close_status_;
}

std::uint32_t Client::next_id() {
  std::uint32_t id = ids_.fetch_add(1, std::memory_order_relaxed) + 1;
  // 0 is the server's own ERROR; a wrapped counter skips it.
  if (id == 0) id = ids_.fetch_add(1, std::memory_order_relaxed) + 1;
  return id;
}

bool Client::write_frame(const std::vector<std::uint8_t>& frame) {
  std::scoped_lock const lock(send_mutex_);
  std::size_t sent = 0;
  while (sent < frame.size()) {
    if (socket_ == net::kInvalidSocket || stop_.load()) return false;
    net::IoResult const r =
        net::send_some(socket_, frame.data() + sent, frame.size() - sent);
    if (r.status == net::IoStatus::kOk) {
      sent += r.bytes;
    } else if (r.status == net::IoStatus::kWouldBlock) {
      net::wait_writable(socket_, kSliceMs);
    } else {
      return false;
    }
  }
  last_send_ns_.store(now_ns(), std::memory_order_relaxed);
  return true;
}

std::shared_ptr<Client::Pending> Client::start(
    Opcode opcode, const std::vector<std::uint8_t>& payload, ReplyFn fn,
    bool async, std::uint32_t& id) {
  auto pending = std::make_shared<Pending>();
  pending->opcode = opcode;
  pending->async = async;
  pending->fn = std::move(fn);
  {
    std::scoped_lock const lock(mutex_);
    if (closed_) return nullptr;
    id = next_id();
    pending_[id] = pending;
  }
  if (!write_frame(encode_frame(Kind::kRequest,
                                static_cast<std::uint16_t>(opcode), id,
                                payload))) {
    // The connection is unusable. The reader ends it, and with it this
    // request, as it ends every other: once, with kClosed.
    stop_ = true;
  }
  return pending;
}

Reply Client::call_with_id(Opcode opcode,
                           const std::vector<std::uint8_t>& payload,
                           int timeout_ms, std::uint32_t& id) {
  id = 0;
  std::shared_ptr<Pending> const pending =
      start(opcode, payload, nullptr, false, id);
  if (!pending) return closed_reply(close_reason());
  if (timeout_ms < 0) timeout_ms = options_.call_timeout_ms;
  std::unique_lock lock(mutex_);
  if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                    [&] { return pending->done; })) {
    pending_.erase(id);
    Reply r;
    r.outcome = Outcome::kTimeout;
    r.message = "no reply within " + std::to_string(timeout_ms) + " ms";
    return r;
  }
  return pending->reply;
}

Reply Client::call(Opcode opcode, const std::vector<std::uint8_t>& payload,
                   int timeout_ms) {
  std::uint32_t id = 0;
  return call_with_id(opcode, payload, timeout_ms, id);
}

std::uint32_t Client::send(Opcode opcode,
                           const std::vector<std::uint8_t>& payload,
                           ReplyFn done) {
  std::uint32_t id = 0;
  if (!start(opcode, payload, done, true, id)) {
    if (done) done(closed_reply(close_reason()));
    return 0;
  }
  return id;
}

std::string Client::close_reason() {
  std::scoped_lock const lock(mutex_);
  return close_reason_.empty() ? "not connected" : close_reason_;
}

bool Client::next_event(ClientEvent& out, int timeout_ms) {
  std::unique_lock lock(mutex_);
  cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
               [&] { return !events_.empty() || closed_; });
  if (events_.empty()) return false;
  out = std::move(events_.front());
  events_.pop_front();
  return true;
}

void Client::keepalive() {
  if (options_.keepalive_ms <= 0 || !hello_done_.load()) return;
  std::int64_t const quiet =
      now_ns() - last_send_ns_.load(std::memory_order_relaxed);
  if (quiet < std::int64_t{options_.keepalive_ms} * 1000000) return;
  send(Opcode::kPing, {}, nullptr);
}

void Client::reader_loop() {
  std::vector<std::uint8_t> buffer;
  std::vector<std::uint8_t> chunk(64U << 10);
  std::string why = "closed by the client";
  while (!stop_.load()) {
    keepalive();
    if (!net::wait_readable(socket_, kSliceMs)) continue;
    net::IoResult const r = net::recv_some(socket_, chunk.data(), chunk.size());
    if (r.status == net::IoStatus::kWouldBlock) continue;
    if (r.status != net::IoStatus::kOk) {
      why = r.status == net::IoStatus::kClosed ? "the server closed"
                                               : "the connection failed";
      break;
    }
    buffer.insert(buffer.end(), chunk.begin(),
                  chunk.begin() + static_cast<std::ptrdiff_t>(r.bytes));
    std::size_t used = 0;
    bool bad = false;
    while (buffer.size() - used >= kHeaderSize) {
      FrameHeader header;
      if (parse_header(buffer.data() + used, kMaxServerFrame, header) !=
          HeaderError::kOk) {
        bad = true;
        break;
      }
      if (buffer.size() - used - kHeaderSize < header.length) break;
      auto const begin =
          buffer.begin() + static_cast<std::ptrdiff_t>(used + kHeaderSize);
      std::vector<std::uint8_t> const payload(begin, begin + header.length);
      used += kHeaderSize + header.length;
      dispatch(header, payload);
    }
    buffer.erase(buffer.begin(),
                 buffer.begin() + static_cast<std::ptrdiff_t>(used));
    if (bad) {
      why = "the server sent a frame this client cannot read";
      break;
    }
  }
  {
    std::scoped_lock const lock(mutex_);
    if (close_status_ != Status::kOk) why = close_reason_;
  }
  fail_all(why);
}

void Client::dispatch(const FrameHeader& header,
                      const std::vector<std::uint8_t>& payload) {
  if (header.kind == static_cast<std::uint8_t>(Kind::kEvent)) {
    ClientEvent event;
    event.seq = header.id;
    // An event this code does not know (a later minor's) is skipped.
    if (!decode_event(header.opcode, payload, event.event, event.media)) return;
    if (event.event.opcode == Opcode::kEvtControlChanged) {
      controller_.store(event.event.controller, std::memory_order_release);
    }
    deliver(event);
    return;
  }
  if (header.kind != static_cast<std::uint8_t>(Kind::kResponse)) return;

  Reply reply;
  reply.outcome = Outcome::kReplied;
  Reader r(payload);
  std::uint16_t status = 0;
  r.u16(status);
  if (!r.skip(2)) {
    reply.outcome = Outcome::kBadReply;
    reply.message = "a response without its status";
  }
  reply.status = static_cast<Status>(status);
  if (reply.outcome == Outcome::kReplied) {
    if (reply.status == Status::kOk) {
      r.bytes(reply.fields);
    } else {
      r.str(reply.message);
    }
  }

  if (header.opcode == static_cast<std::uint16_t>(Opcode::kError) &&
      header.id == 0) {
    // The server's last word before it closes the connection (section 5).
    std::scoped_lock const lock(mutex_);
    close_status_ = reply.status;
    close_reason_ = reply.message.empty() ? "the server closed the connection"
                                          : reply.message;
    stop_ = true;
    return;
  }

  std::shared_ptr<Pending> pending;
  {
    std::scoped_lock const lock(mutex_);
    auto const it = pending_.find(header.id);
    if (it == pending_.end()) return;  // timed out already
    pending = it->second;
    pending_.erase(it);
  }
  if (header.opcode != static_cast<std::uint16_t>(pending->opcode) &&
      reply.outcome == Outcome::kReplied) {
    reply.outcome = Outcome::kBadReply;
    reply.message = "the response names another opcode";
  }
  // What the server says about this client's role, before the caller (or a
  // later event) can look at it.
  if (reply.ok() && pending->opcode == Opcode::kHello) {
    Reader fields(reply.fields);
    if (read_hello_reply(fields, hello_)) {
      controller_.store(hello_.role == Role::kController ? hello_.client_id : 0,
                        std::memory_order_release);
      hello_done_.store(true, std::memory_order_release);
    }
  } else if (reply.ok() && pending->opcode == Opcode::kControl) {
    Reader fields(reply.fields);
    ControlReply control;
    if (read_control_reply(fields, control)) {
      controller_.store(control.controller, std::memory_order_release);
    }
  }
  if (pending->async) {
    if (pending->fn) pending->fn(reply);
    return;
  }
  {
    std::scoped_lock const lock(mutex_);
    pending->reply = std::move(reply);
    pending->done = true;
  }
  cv_.notify_all();
}

void Client::deliver(const ClientEvent& event) {
  if (options_.on_event) {
    options_.on_event(event);
    return;
  }
  {
    std::scoped_lock const lock(mutex_);
    events_.push_back(event);
    while (events_.size() > options_.max_queued_events) {
      events_.pop_front();
      dropped_events_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  cv_.notify_all();
}

void Client::fail_all(const std::string& why) {
  std::map<std::uint32_t, std::shared_ptr<Pending>> ended;
  {
    std::scoped_lock const lock(mutex_);
    closed_ = true;
    if (close_reason_.empty()) close_reason_ = why;
    ended.swap(pending_);
    for (auto& [id, pending] : ended) {
      pending->reply = closed_reply(close_reason_);
      pending->done = true;
    }
  }
  open_ = false;
  cv_.notify_all();
  for (auto& [id, pending] : ended) {
    if (pending->async && pending->fn) pending->fn(pending->reply);
  }
}

// -- typed requests ----------------------------------------------------------

Result Client::exchange(Opcode opcode, const std::vector<std::uint8_t>& payload,
                        const std::function<bool(Reader&)>& parse,
                        int timeout_ms) {
  Reply const reply = call(opcode, payload, timeout_ms);
  Result result = reply;
  if (!reply.ok() || !parse) return result;
  Reader r(reply.fields);
  if (!parse(r)) {
    result.outcome = Outcome::kBadReply;
    result.message = "the reply is shorter than its fields";
  }
  return result;
}

Result Client::ping(int timeout_ms) {
  return exchange(Opcode::kPing, {}, nullptr, timeout_ms);
}

Result Client::subscribe(const Subscription& wanted, Subscription& accepted) {
  Writer w;
  write_subscription(w, wanted);
  return exchange(Opcode::kSubscribe, w.data(), [&](Reader& r) {
    return read_subscription(r, accepted);
  });
}

Result Client::control(std::uint8_t action, std::uint8_t flags,
                       ControlReply& reply) {
  Writer w;
  write_control_request(w, ControlRequest{action, flags});
  return exchange(Opcode::kControl, w.data(),
                  [&](Reader& r) { return read_control_reply(r, reply); });
}

Result Client::acquire_control(bool force, ControlReply& reply) {
  return control(kControlAcquire, force ? kControlFlagForce : 0, reply);
}

Result Client::release_control(ControlReply& reply) {
  return control(kControlRelease, 0, reply);
}

Result Client::machine_info(MachineInfo& out) {
  return exchange(Opcode::kMachineInfo, {},
                  [&](Reader& r) { return read_machine_info(r, out); });
}

Result Client::run_state(RunState& out) {
  return exchange(Opcode::kRunState, {},
                  [&](Reader& r) { return read_run_state(r, out); });
}

Result Client::pause(Stamp& stamp) {
  return exchange(Opcode::kPause, {},
                  [&](Reader& r) { return read_stamp(r, stamp); });
}

Result Client::run(Stamp& stamp) {
  return exchange(Opcode::kRun, {},
                  [&](Reader& r) { return read_stamp(r, stamp); });
}

Result Client::reset(bool stay_paused, Stamp& stamp) {
  Writer w;
  w.u8(stay_paused ? kResetFlagStayPaused : 0);
  w.zeros(3);
  return exchange(Opcode::kReset, w.data(),
                  [&](Reader& r) { return read_stamp(r, stamp); });
}

Result Client::step(const StepRequest& step, std::uint32_t& request_id) {
  Writer w;
  write_step_request(w, step);
  return call_with_id(Opcode::kStep, w.data(), -1, request_id);
}

Result Client::regs_get(Stamp& stamp, Regs& out) {
  return exchange(Opcode::kRegsGet, {}, [&](Reader& r) {
    return read_stamp(r, stamp) && read_regs(r, out);
  });
}

Result Client::regs_set(const RegsSetRequest& request, Stamp& stamp,
                        Regs& after) {
  Writer w;
  write_regs_set_request(w, request);
  return exchange(Opcode::kRegsSet, w.data(), [&](Reader& r) {
    return read_stamp(r, stamp) && read_regs(r, after);
  });
}

Result Client::mem_info(MemInfo& out) {
  return exchange(Opcode::kMemInfo, {},
                  [&](Reader& r) { return read_mem_info(r, out); });
}

Result Client::mem_read(const MemReadRequest& request, Stamp& stamp,
                        std::vector<std::uint8_t>& data) {
  Writer w;
  write_mem_read_request(w, request);
  return exchange(Opcode::kMemRead, w.data(), [&](Reader& r) {
    return read_stamp(r, stamp) && r.bytes(data) && data.size() == request.len;
  });
}

Result Client::mem_write(const MemWriteRequest& request,
                         const std::uint8_t* data, std::size_t len,
                         Stamp& stamp) {
  Writer w;
  write_mem_write_request(w, request);
  w.bytes(data, len);
  return exchange(Opcode::kMemWrite, w.data(),
                  [&](Reader& r) { return read_stamp(r, stamp); });
}

Result Client::bp_list(std::uint64_t& generation,
                       std::vector<BpEntry>& entries) {
  return exchange(Opcode::kBpList, {}, [&](Reader& r) {
    std::uint32_t count = 0;
    entries.clear();
    if (!r.u64(generation) || !r.u32(count)) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
      BpEntry entry;
      if (!read_bp_entry(r, entry)) return false;
      entries.push_back(std::move(entry));
    }
    return true;
  });
}

Result Client::bp_add(const BpEntry& entry, std::uint64_t& generation) {
  Writer w;
  write_bp_entry(w, entry);
  return exchange(Opcode::kBpAdd, w.data(),
                  [&](Reader& r) { return r.u64(generation); });
}

Result Client::bp_del(const BpDelRequest& request, std::uint64_t& generation) {
  Writer w;
  write_bp_del_request(w, request);
  return exchange(Opcode::kBpDel, w.data(),
                  [&](Reader& r) { return r.u64(generation); });
}

Result Client::bp_clear(std::uint8_t kind, std::uint64_t& generation) {
  return exchange(Opcode::kBpClear, {kind},
                  [&](Reader& r) { return r.u64(generation); });
}

Result Client::key(const KeyRequest& request) {
  Writer w;
  write_key_request(w, request);
  return exchange(Opcode::kKey, w.data());
}

Result Client::keys_release() { return exchange(Opcode::kKeysRelease, {}); }

Result Client::text(const std::string& text, std::uint32_t& pending) {
  return exchange(Opcode::kText, one_str(text),
                  [&](Reader& r) { return r.u32(pending); });
}

Result Client::mouse(const MouseRequest& request) {
  Writer w;
  write_mouse_request(w, request);
  return exchange(Opcode::kMouse, w.data());
}

Result Client::gun(const GunRequest& request) {
  Writer w;
  write_gun_request(w, request);
  return exchange(Opcode::kGun, w.data());
}

Result Client::input_state(KeyMatrix& rows) {
  return exchange(Opcode::kInputState, {}, [&](Reader& r) {
    for (std::uint8_t& row : rows) {
      if (!r.u8(row)) return false;
    }
    return true;
  });
}

Result Client::chip_get(Chip chip, ChipState& out, std::uint16_t& layout) {
  Writer w;
  w.u8(static_cast<std::uint8_t>(chip));
  w.zeros(3);
  return exchange(Opcode::kChipGet, w.data(), [&](Reader& r) {
    return read_chip_reply(r, out, layout) &&
           out.chip == static_cast<std::uint8_t>(chip);
  });
}

Result Client::sym_list(std::uint64_t& generation,
                        std::vector<Symbol>& symbols) {
  return exchange(Opcode::kSymList, {}, [&](Reader& r) {
    std::uint32_t count = 0;
    symbols.clear();
    if (!r.u64(generation) || !r.u32(count)) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
      Symbol symbol;
      if (!read_symbol(r, symbol)) return false;
      symbols.push_back(std::move(symbol));
    }
    return true;
  });
}

Result Client::sym_add(const Symbol& symbol, std::uint64_t& generation) {
  Writer w;
  write_symbol(w, symbol);
  return exchange(Opcode::kSymAdd, w.data(),
                  [&](Reader& r) { return r.u64(generation); });
}

Result Client::sym_del(const std::string& name, std::uint64_t& generation) {
  return exchange(Opcode::kSymDel, one_str(name),
                  [&](Reader& r) { return r.u64(generation); });
}

Result Client::sym_load(const std::string& path, std::uint64_t& generation,
                        std::uint32_t& loaded) {
  return exchange(Opcode::kSymLoad, one_str(path), [&](Reader& r) {
    return r.u64(generation) && r.u32(loaded);
  });
}

Result Client::fb_info(FbInfo& out) {
  return exchange(Opcode::kFbInfo, {},
                  [&](Reader& r) { return read_fb_info(r, out); });
}

Result Client::media_insert(const MediaInsertRequest& request,
                            std::uint64_t& generation) {
  Writer w;
  write_media_insert_request(w, request);
  return exchange(Opcode::kMediaInsert, w.data(),
                  [&](Reader& r) { return r.u64(generation); });
}

Result Client::media_eject(MediaSlot slot, std::uint64_t& generation) {
  return exchange(Opcode::kMediaEject,
                  slot_payload(static_cast<std::uint8_t>(slot)),
                  [&](Reader& r) { return r.u64(generation); });
}

Result Client::media_info(MediaSlot slot, MediaInfo& out) {
  return exchange(Opcode::kMediaInfo,
                  slot_payload(static_cast<std::uint8_t>(slot)),
                  [&](Reader& r) { return read_media_info(r, out); });
}

Result Client::tape_control(const TapeControlRequest& request, TapeState& out) {
  Writer w;
  write_tape_control_request(w, request);
  return exchange(Opcode::kTapeControl, w.data(),
                  [&](Reader& r) { return read_tape_state(r, out); });
}

Result Client::snapshot_save(const std::string& path, Stamp& stamp) {
  return exchange(Opcode::kSnapshotSave, one_str(path),
                  [&](Reader& r) { return read_stamp(r, stamp); });
}

Result Client::snapshot_load(const SnapshotLoadRequest& request, Stamp& stamp) {
  Writer w;
  write_snapshot_load_request(w, request);
  return exchange(Opcode::kSnapshotLoad, w.data(),
                  [&](Reader& r) { return read_stamp(r, stamp); });
}

Result Client::config_get(const std::string& key, ConfigValue& out) {
  return exchange(Opcode::kConfigGet, one_str(key),
                  [&](Reader& r) { return read_config_value(r, out); });
}

Result Client::config_set(const ConfigSetRequest& request,
                          ConfigEffect& effect) {
  Writer w;
  write_config_set_request(w, request);
  return exchange(Opcode::kConfigSet, w.data(), [&](Reader& r) {
    std::uint8_t raw = 0;
    if (!r.u8(raw)) return false;
    effect = static_cast<ConfigEffect>(raw);
    return true;
  });
}

Result Client::config_apply(Stamp& stamp, int timeout_ms) {
  return exchange(
      Opcode::kConfigApply, {}, [&](Reader& r) { return read_stamp(r, stamp); },
      timeout_ms);
}

}  // namespace kcpc
