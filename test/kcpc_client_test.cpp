// konCePCja — tests for the C++ KCPC client and the frame-ring reader
// (beads-3sp.1).
//
// The client runs against the real kcpc::Server over loopback, with the fake
// machine of kcpc_server_test.cpp (kcpc_fake_machine.h): every reply the
// client parses was written by the server's own code. The ring reader reads
// frames an FbRingWriter wrote into a region of the test's own.

#include "kcpc/kcpc_client.h"

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kcpc/fb_ring.h"
#include "kcpc/fb_ring_reader.h"
#include "kcpc/kcpc_proto.h"
#include "kcpc/kcpc_server.h"
#include "kcpc_fake_machine.h"
#include "net_socket.h"
#include "shm_region.h"
#include "startup_manifest.h"

namespace {

using kcpc::Opcode;
using kcpc::Outcome;
using kcpc::Status;
using kcpc_fake::FakeBreakpoints;
using kcpc_fake::FakeDevtools;
using kcpc_fake::FakeInput;
using kcpc_fake::FakeMedia;
using kcpc_fake::FakeReads;
using kcpc_fake::FakeRun;
using kcpc_fake::FakeWrites;

// Away from 6545 and from kcpc_server_test's range, wide so two test_runners
// at once (another worktree) do not collide. Each test connects only to the
// port its own server bound.
constexpr int kTestBasePort = 47645;
constexpr int kTestProbeCount = 40;

class KcpcClientTest : public ::testing::Test {
 protected:
  void SetUp() override { start_with(nullptr); }

  // (Re)starts the server with every group of callbacks, as the core has,
  // then lets `tweak` change the config.
  void start_with(const std::function<void(kcpc::ServerConfig&)>& tweak) {
    server_.stop();
    kcpc::ServerConfig c;
    c.base_port = kTestBasePort;
    c.probe_count = kTestProbeCount;
    c.server_version = "v0.0.0-client-test";
    c.pid = 4242;
    c.machine_info = [](kcpc::MachineInfo& info) {
      info = kcpc_fake::fake_machine();
      return Status::kOk;
    };
    reads_.attach(c);
    writes_.attach(c);
    run_.attach(c, server_);
    breakpoints_.attach(c, server_);
    input_.attach(c);
    devtools_.attach(c, server_);
    media_.attach(c, server_);
    if (tweak) tweak(c);
    ASSERT_TRUE(server_.start(c));
  }

  void TearDown() override { server_.stop(); }

  // A client connected with HELLO.
  std::unique_ptr<kcpc::Client> join(kcpc::Role role,
                                     kcpc::EventFn on_event = nullptr) {
    auto client = std::make_unique<kcpc::Client>();
    kcpc::ClientOptions options;
    options.name = "kcpc_client_test";
    options.role = role;
    options.on_event = std::move(on_event);
    kcpc::Result result;
    EXPECT_TRUE(client->connect(server_.port(), options, result))
        << result.message;
    return client;
  }

  // The next queued EVT_CONTROL_CHANGED with this reason; the events
  // before it are dropped.
  static bool wait_control(kcpc::Client& client, std::uint8_t reason,
                           kcpc::ClientEvent& out) {
    while (client.next_event(out, 5000)) {
      if (out.event.opcode == Opcode::kEvtControlChanged &&
          out.event.reason == reason) {
        return true;
      }
    }
    return false;
  }

  kcpc::Server server_;
  FakeReads reads_;
  FakeWrites writes_;
  FakeRun run_;
  FakeBreakpoints breakpoints_;
  FakeInput input_;
  FakeDevtools devtools_;
  FakeMedia media_;
};

}  // namespace

TEST_F(KcpcClientTest, HelloReportsTheServerItsCapsAndTheRole) {
  auto first = join(kcpc::Role::kController);
  auto second = join(kcpc::Role::kController);
  const kcpc::HelloReply& hello = first->hello();
  EXPECT_EQ(hello.server_version, "v0.0.0-client-test");
  EXPECT_EQ(hello.pid, 4242U);
  EXPECT_EQ(hello.caps, kcpc::kServerCaps);
  EXPECT_EQ(hello.machine.model, kcpc_fake::fake_machine().model);
  EXPECT_NE(hello.client_id, 0U);
  EXPECT_NE(second->client_id(), hello.client_id);
  // Control goes to whoever asked first; HELLO never takes it away.
  EXPECT_TRUE(first->is_controller());
  EXPECT_FALSE(second->is_controller());
  EXPECT_EQ(server_.controller(), hello.client_id);
}

TEST_F(KcpcClientTest, NoServerIsAClosedResult) {
  int const port = server_.port();
  server_.stop();
  kcpc::Client client;
  kcpc::Result result;
  EXPECT_FALSE(client.connect(port, kcpc::ClientOptions{}, result));
  EXPECT_EQ(result.outcome, Outcome::kClosed);
  EXPECT_FALSE(client.connected());
  kcpc::Stamp stamp;
  EXPECT_EQ(client.pause(stamp).outcome, Outcome::kClosed);
}

TEST_F(KcpcClientTest, RegsGetReadsTheStampAndTheBlock) {
  auto client = join(kcpc::Role::kObserver);
  kcpc::Stamp stamp;
  kcpc::Regs regs;
  ASSERT_TRUE(client->regs_get(stamp, regs).ok());
  EXPECT_EQ(stamp.frame, 77U);
  EXPECT_TRUE(stamp.paused);
  kcpc::Regs const want = FakeReads::regs();
  EXPECT_EQ(regs.af, want.af);
  EXPECT_EQ(regs.pc, want.pc);
  EXPECT_EQ(regs.hl_alt, want.hl_alt);
  EXPECT_EQ(regs.im, want.im);
  EXPECT_EQ(regs.halt, want.halt);
}

TEST_F(KcpcClientTest, MemReadOf64KiBIsOneReply) {
  auto client = join(kcpc::Role::kObserver);
  kcpc::MemReadRequest request;
  request.space = static_cast<std::uint8_t>(kcpc::MemSpace::kCpu);
  request.len = FakeReads::kSize;
  kcpc::Stamp stamp;
  std::vector<std::uint8_t> data;
  ASSERT_TRUE(client->mem_read(request, stamp, data).ok());
  EXPECT_EQ(reads_.mem_reads.load(), 1);
  EXPECT_EQ(stamp.frame, 79U);
  EXPECT_EQ(data, reads_.memory);
}

TEST_F(KcpcClientTest, AStopIsDeliveredInOrderAfterTheReplyThatCausedIt) {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> seen;
  std::uint32_t stop_cause = 0;
  auto client = join(kcpc::Role::kController, [&](const kcpc::ClientEvent& e) {
    if (e.event.opcode != Opcode::kEvtStopped) return;
    std::scoped_lock const lock(mutex);
    seen.emplace_back("EVT_STOPPED");
    stop_cause = e.event.cause_request;
    cv.notify_all();
  });
  kcpc::Subscription accepted;
  ASSERT_TRUE(client
                  ->subscribe(kcpc::Subscription{kcpc::kEventClassRun, 0},
                              accepted)
                  .ok());
  // The fake machine posts the stop before it answers, and holds the reply
  // long enough for the fan-out thread to have the event first.
  run_.post_from_request = true;
  std::uint32_t const id =
      client->send(Opcode::kPause, {}, [&](const kcpc::Reply& reply) {
        std::scoped_lock const lock(mutex);
        EXPECT_TRUE(reply.ok());
        seen.emplace_back("PAUSE reply");
        cv.notify_all();
      });
  ASSERT_NE(id, 0U);
  std::unique_lock lock(mutex);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                          [&] { return seen.size() == 2; }));
  EXPECT_EQ(seen, (std::vector<std::string>{"PAUSE reply", "EVT_STOPPED"}));
  EXPECT_EQ(stop_cause, id);
}

TEST_F(KcpcClientTest, QueuedEventsKeepTheirConnectionSequence) {
  // An observer: a controller's HELLO is itself announced to it.
  auto client = join(kcpc::Role::kObserver);
  kcpc::Subscription accepted;
  ASSERT_TRUE(
      client->subscribe(kcpc::Subscription{kcpc::kEventClassAll, 0}, accepted)
          .ok());
  EXPECT_EQ(accepted.classes, kcpc::kEventClassAll);
  for (std::uint64_t frame = 1; frame <= 3; ++frame) {
    kcpc::Event e;
    e.opcode = Opcode::kEvtResumed;
    e.frame = frame;
    server_.post(e);
  }
  for (std::uint32_t n = 1; n <= 3; ++n) {
    kcpc::ClientEvent event;
    ASSERT_TRUE(client->next_event(event, 5000));
    EXPECT_EQ(event.seq, n);
    EXPECT_EQ(event.event.opcode, Opcode::kEvtResumed);
    EXPECT_EQ(event.event.frame, n);
  }
  kcpc::ClientEvent none;
  EXPECT_FALSE(client->next_event(none, 50));
}

TEST_F(KcpcClientTest, AForcedTakeoverDemotesTheClient) {
  auto first = join(kcpc::Role::kController);
  auto second = join(kcpc::Role::kObserver);
  ASSERT_TRUE(first->is_controller());

  kcpc::ControlReply reply;
  ASSERT_TRUE(second->acquire_control(false, reply).ok());
  EXPECT_EQ(reply.role, kcpc::Role::kObserver);
  EXPECT_EQ(reply.controller, first->client_id());
  ASSERT_TRUE(second->acquire_control(true, reply).ok());
  EXPECT_EQ(reply.role, kcpc::Role::kController);
  EXPECT_TRUE(second->is_controller());

  kcpc::ClientEvent event;
  ASSERT_TRUE(wait_control(*first, kcpc::kControlTaken, event));
  EXPECT_EQ(event.event.controller, second->client_id());
  EXPECT_EQ(event.event.previous, first->client_id());
  EXPECT_FALSE(first->is_controller());
  EXPECT_EQ(first->controller(), second->client_id());

  kcpc::Stamp stamp;
  kcpc::Result const refused = first->pause(stamp);
  EXPECT_EQ(refused.outcome, Outcome::kReplied);
  EXPECT_EQ(refused.status, Status::kNotController);
  EXPECT_EQ(run_.pauses.load(), 0);
  EXPECT_TRUE(second->pause(stamp).ok());
  EXPECT_EQ(run_.pauses.load(), 1);
}


TEST_F(KcpcClientTest, ATimedOutCallLeavesTheConnectionUsable) {
  std::atomic<bool> release{false};
  start_with([&](kcpc::ServerConfig& c) {
    c.reset = [&](const kcpc::Cause&, bool, kcpc::Stamp& stamp) {
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      stamp = {1, false};
      return Status::kOk;
    };
  });
  kcpc::ClientOptions options;
  options.role = kcpc::Role::kController;
  options.call_timeout_ms = 100;
  kcpc::Client client;
  kcpc::Result result;
  ASSERT_TRUE(client.connect(server_.port(), options, result));
  kcpc::Stamp stamp;
  EXPECT_EQ(client.reset(false, stamp).outcome, Outcome::kTimeout);
  release = true;
  // The late reply is dropped; the next request gets its own.
  kcpc::Regs regs;
  kcpc::Result const r = client.regs_get(stamp, regs);
  EXPECT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(regs.pc, FakeReads::regs().pc);
}

// Every typed request reaches the machine with what was asked and reads back
// what the server answered: a request or reply laid out differently from the
// server's codec fails here.
TEST_F(KcpcClientTest, EveryTypedRequestRoundTrips) {
  auto client = join(kcpc::Role::kController);
  kcpc::Stamp stamp;

  EXPECT_TRUE(client->ping().ok());
  kcpc::MachineInfo info;
  ASSERT_TRUE(client->machine_info(info).ok());
  EXPECT_EQ(info.rom_slots, kcpc_fake::fake_machine().rom_slots);
  kcpc::RunState state;
  ASSERT_TRUE(client->run_state(state).ok());
  EXPECT_EQ(state.stamp.frame, 55U);
  EXPECT_EQ(state.controller, client->client_id());
  ASSERT_TRUE(client->pause(stamp).ok());
  EXPECT_EQ(stamp.frame, 56U);
  ASSERT_TRUE(client->run(stamp).ok());
  EXPECT_EQ(stamp.frame, 57U);
  ASSERT_TRUE(client->reset(true, stamp).ok());
  EXPECT_TRUE(run_.last_stay_paused);
  kcpc::StepRequest step;
  step.mode = static_cast<std::uint8_t>(kcpc::StepMode::kTo);
  step.addr = 0x4321;
  std::uint32_t step_id = 0;
  ASSERT_TRUE(client->step(step, step_id).ok());
  EXPECT_EQ(run_.last_step.addr, 0x4321);
  EXPECT_EQ(run_.last_cause.request, step_id);

  kcpc::RegsSetRequest set;
  set.mask = 1U << 3;  // HL
  set.regs.hl = 0xBEEF;
  kcpc::Regs after;
  ASSERT_TRUE(client->regs_set(set, stamp, after).ok());
  EXPECT_EQ(writes_.last_mask, set.mask);
  EXPECT_EQ(after.hl, 0xBEEF);
  kcpc::MemInfo mem;
  ASSERT_TRUE(client->mem_info(mem).ok());
  EXPECT_EQ(mem.upper_rom, 7);
  kcpc::MemWriteRequest write;
  write.space = static_cast<std::uint8_t>(kcpc::MemSpace::kCpuRam);
  write.addr = 0x4000;
  std::uint8_t const bytes[] = {1, 2, 3};
  ASSERT_TRUE(client->mem_write(write, bytes, sizeof bytes, stamp).ok());
  EXPECT_EQ(writes_.written, (std::vector<std::uint8_t>{1, 2, 3}));
  EXPECT_EQ(stamp.frame, 91U);

  std::uint64_t generation = 0;
  kcpc::BpEntry bp;
  bp.addr = 0x1234;
  bp.condition = "a == 1";
  ASSERT_TRUE(client->bp_add(bp, generation).ok());
  EXPECT_EQ(generation, 41U);
  std::vector<kcpc::BpEntry> entries;
  ASSERT_TRUE(client->bp_list(generation, entries).ok());
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].condition, "a == 1");
  kcpc::BpDelRequest del;
  del.addr = 0x1234;
  ASSERT_TRUE(client->bp_del(del, generation).ok());
  EXPECT_EQ(breakpoints_.last_del.addr, 0x1234);
  ASSERT_TRUE(client->bp_clear(kcpc::kBpKindAll, generation).ok());
  EXPECT_EQ(breakpoints_.last_clear_kind, kcpc::kBpKindAll);

  ASSERT_TRUE(client->key(kcpc::KeyRequest{8, 5, 1}).ok());
  kcpc::KeyMatrix rows{};
  ASSERT_TRUE(client->input_state(rows).ok());
  EXPECT_EQ(rows[8], 0xFF & ~(1U << 5));
  ASSERT_TRUE(client->keys_release().ok());
  EXPECT_EQ(input_.row(8), 0xFF);
  std::uint32_t pending = 0;
  ASSERT_TRUE(client->text("run\"", pending).ok());
  EXPECT_EQ(pending, 4U);
  kcpc::MouseRequest mouse;
  mouse.dx = -3;
  mouse.dy = 4;
  mouse.buttons = kcpc::kMouseButtonLeft;
  mouse.flags = kcpc::kMouseFlagButtons;
  ASSERT_TRUE(client->mouse(mouse).ok());
  EXPECT_EQ(input_.buttons(), kcpc::kMouseButtonLeft);
  kcpc::GunRequest gun;
  gun.trigger = 1;
  gun.flags = kcpc::kGunFlagTrigger;
  ASSERT_TRUE(client->gun(gun).ok());
  EXPECT_TRUE(input_.pulled());

  kcpc::ChipState chip;
  std::uint16_t layout = 0;
  ASSERT_TRUE(client->chip_get(kcpc::Chip::kAsic, chip, layout).ok());
  EXPECT_EQ(layout, kcpc::kChipLayoutAsic);
  EXPECT_EQ(chip.stamp.frame, 77U);
  ASSERT_TRUE(client->sym_add(kcpc::Symbol{0x4000, "main"}, generation).ok());
  EXPECT_EQ(generation, 11U);
  std::uint32_t loaded = 0;
  ASSERT_TRUE(client->sym_load("game.sym", generation, loaded).ok());
  EXPECT_EQ(loaded, 2U);
  std::vector<kcpc::Symbol> symbols;
  ASSERT_TRUE(client->sym_list(generation, symbols).ok());
  ASSERT_EQ(symbols.size(), 3U);
  EXPECT_EQ(symbols[0].name, "main");
  ASSERT_TRUE(client->sym_del("main", generation).ok());
  EXPECT_EQ(client->sym_del("main", generation).status, Status::kNotFound);
  kcpc::FbInfo fb;
  ASSERT_TRUE(client->fb_info(fb).ok());
  EXPECT_EQ(fb.stride, 2304U);
  EXPECT_EQ(fb.shm_name, "/kcpc-fb-4242");

  kcpc::MediaInsertRequest insert;
  insert.slot = static_cast<std::uint8_t>(kcpc::MediaSlot::kTape);
  insert.path = "game.cdt";
  ASSERT_TRUE(client->media_insert(insert, generation).ok());
  EXPECT_EQ(generation, 41U);
  kcpc::MediaInfo media;
  ASSERT_TRUE(client->media_info(kcpc::MediaSlot::kTape, media).ok());
  EXPECT_EQ(media.path, "game.cdt");
  EXPECT_EQ(media.tape.blocks, 3U);
  kcpc::TapeState tape;
  ASSERT_TRUE(client
                  ->tape_control(
                      kcpc::TapeControlRequest{
                          static_cast<std::uint8_t>(kcpc::TapeAction::kSeek),
                          2},
                      tape)
                  .ok());
  EXPECT_EQ(tape.block, 2U);
  ASSERT_TRUE(client->media_eject(kcpc::MediaSlot::kTape, generation).ok());
  EXPECT_EQ(generation, 42U);
  ASSERT_TRUE(client->snapshot_save("state.sna", stamp).ok());
  EXPECT_EQ(stamp.frame, 500U);
  kcpc::Result const missing = client->snapshot_save("/nowhere/state.sna",
                                                     stamp);
  EXPECT_EQ(missing.status, Status::kNotFound);
  EXPECT_EQ(missing.message, "no such directory");
  ASSERT_TRUE(client
                  ->snapshot_load(
                      kcpc::SnapshotLoadRequest{
                          kcpc::kSnapshotFlagStayPaused, "state.sna"},
                      stamp)
                  .ok());
  EXPECT_TRUE(stamp.paused);
  kcpc::ConfigEffect effect = kcpc::ConfigEffect::kNow;
  ASSERT_TRUE(client->config_set(kcpc::ConfigSetRequest{"model", "3"}, effect)
                  .ok());
  EXPECT_EQ(effect, kcpc::ConfigEffect::kStaged);
  kcpc::ConfigValue value;
  ASSERT_TRUE(client->config_get("model", value).ok());
  EXPECT_EQ(value.value, "2");
  EXPECT_TRUE(value.has_pending);
  EXPECT_EQ(value.pending, "3");
  ASSERT_TRUE(client->config_apply(stamp).ok());
  EXPECT_EQ(stamp.frame, 900U);

  kcpc::ControlReply control;
  ASSERT_TRUE(client->release_control(control).ok());
  EXPECT_EQ(control.controller, 0U);
  EXPECT_FALSE(client->is_controller());
}

TEST_F(KcpcClientTest, AQuietControllerIsKeptAliveByPing) {
  start_with([](kcpc::ServerConfig& c) { c.controller_idle_timeout_ms = 600; });
  kcpc::ClientOptions options;
  options.role = kcpc::Role::kController;
  options.keepalive_ms = 150;
  kcpc::Client client;
  kcpc::Result result;
  ASSERT_TRUE(client.connect(server_.port(), options, result));
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_TRUE(client.connected());
  EXPECT_TRUE(client.is_controller());
  EXPECT_TRUE(client.ping().ok());

  // Without it the same silence ends the connection.
  kcpc::ControlReply released;
  ASSERT_TRUE(client.release_control(released).ok());
  options.keepalive_ms = 0;
  kcpc::Client quiet;
  ASSERT_TRUE(quiet.connect(server_.port(), options, result));
  ASSERT_TRUE(quiet.is_controller());
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (quiet.connected() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_FALSE(quiet.connected());
  EXPECT_EQ(quiet.close_status(), Status::kIdleTimeout);
}

// -- a peer that never answers ------------------------------------------------

namespace {

// One loopback connection the test speaks for the server: it answers HELLO,
// then reads requests and answers none, so they stay pending for as long as
// the test likes. kcpc::Server answers everything it has read before it
// shuts down; this is how a connection ends with requests still waiting.
class SilentPeer {
 public:
  SilentPeer() {
    net::startup();
    for (int i = 0; i < kTestProbeCount && listener_ == net::kInvalidSocket;
         ++i) {
      port_ = kTestBasePort + kTestProbeCount + i;
      listener_ = net::listen_loopback(port_, 1);
    }
  }
  ~SilentPeer() {
    if (peer_ != net::kInvalidSocket) net::close_socket(peer_);
    if (listener_ != net::kInvalidSocket) net::close_socket(listener_);
    net::cleanup();
  }
  SilentPeer(const SilentPeer&) = delete;
  SilentPeer& operator=(const SilentPeer&) = delete;

  int port() const { return listener_ != net::kInvalidSocket ? port_ : 0; }

  // Accepts the client and answers its HELLO, granting control.
  bool greet() {
    auto const deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (peer_ == net::kInvalidSocket &&
           std::chrono::steady_clock::now() < deadline) {
      if (net::wait_readable(listener_, 100)) {
        peer_ = net::accept_client(listener_);
      }
    }
    kcpc::FrameHeader header;
    std::vector<std::uint8_t> payload;
    if (!read_frame(header, payload) ||
        header.opcode != static_cast<std::uint16_t>(Opcode::kHello)) {
      return false;
    }
    kcpc::HelloReply hello;
    hello.role = kcpc::Role::kController;
    hello.client_id = 5;
    kcpc::Writer w;
    kcpc::write_status(w, Status::kOk);
    kcpc::write_hello_reply(w, hello);
    return write(kcpc::encode_frame(kcpc::Kind::kResponse, header.opcode,
                                    header.id, w.data()));
  }

  // Reads one frame and answers nothing.
  bool read_frame(kcpc::FrameHeader& header,
                  std::vector<std::uint8_t>& payload) {
    auto const deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      if (buffer_.size() >= kcpc::kHeaderSize &&
          kcpc::parse_header(buffer_.data(), kcpc::kMaxRequestPayload,
                             header) == kcpc::HeaderError::kOk &&
          buffer_.size() >= kcpc::kHeaderSize + header.length) {
        auto const begin = buffer_.begin() + kcpc::kHeaderSize;
        payload.assign(begin, begin + header.length);
        buffer_.erase(buffer_.begin(), begin + header.length);
        return true;
      }
      if (!net::wait_readable(peer_, 100)) continue;
      std::uint8_t chunk[4096];
      net::IoResult const r = net::recv_some(peer_, chunk, sizeof chunk);
      if (r.status == net::IoStatus::kOk) {
        buffer_.insert(buffer_.end(), chunk, chunk + r.bytes);
      } else if (r.status != net::IoStatus::kWouldBlock) {
        return false;
      }
    }
    return false;
  }

  bool write(const std::vector<std::uint8_t>& frame) {
    std::size_t sent = 0;
    while (sent < frame.size()) {
      net::IoResult const r =
          net::send_some(peer_, frame.data() + sent, frame.size() - sent);
      if (r.status == net::IoStatus::kOk) {
        sent += r.bytes;
      } else if (r.status == net::IoStatus::kWouldBlock) {
        net::wait_writable(peer_, 100);
      } else {
        return false;
      }
    }
    return true;
  }

  // What a core that quits sends: EVT_SHUTDOWN, then ERROR, then the end.
  void shut_down() {
    write(kcpc::encode_frame(kcpc::Kind::kEvent,
                             static_cast<std::uint16_t>(Opcode::kEvtShutdown),
                             1, {}));
    write(kcpc::encode_frame(
        kcpc::Kind::kResponse, static_cast<std::uint16_t>(Opcode::kError), 0,
        kcpc::error_payload(Status::kShuttingDown, "the core is quitting")));
    net::shutdown_send(peer_);
  }

  void drop() {
    net::close_socket(peer_);
    peer_ = net::kInvalidSocket;
  }

 private:
  net::Socket listener_ = net::kInvalidSocket;
  net::Socket peer_ = net::kInvalidSocket;
  int port_ = 0;
  std::vector<std::uint8_t> buffer_;
};

// Two blocking calls and two asynchronous requests the peer has read and
// will never answer, then `end` ends the connection.
void expect_pending_calls_end(SilentPeer& peer,
                              const std::function<void()>& end,
                              Status close_status) {
  ASSERT_NE(peer.port(), 0);
  std::atomic<int> shutdowns{0};
  kcpc::ClientOptions options;
  options.role = kcpc::Role::kController;
  options.call_timeout_ms = 20000;
  options.on_event = [&](const kcpc::ClientEvent& e) {
    if (e.event.opcode == Opcode::kEvtShutdown) shutdowns.fetch_add(1);
  };
  kcpc::Client client;
  kcpc::Result result;
  std::thread greeter([&] { EXPECT_TRUE(peer.greet()); });
  ASSERT_TRUE(client.connect(peer.port(), options, result)) << result.message;
  greeter.join();
  EXPECT_TRUE(client.is_controller());

  std::atomic<int> ended{0};
  std::vector<std::thread> callers;
  for (int i = 0; i < 2; ++i) {
    callers.emplace_back([&] {
      kcpc::Stamp stamp;
      kcpc::Result const r = client.pause(stamp);
      if (r.outcome == Outcome::kClosed) ended.fetch_add(1);
    });
    client.send(Opcode::kPing, {}, [&](const kcpc::Reply& reply) {
      if (reply.outcome == Outcome::kClosed) ended.fetch_add(1);
    });
  }
  for (int i = 0; i < 4; ++i) {
    kcpc::FrameHeader header;
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(peer.read_frame(header, payload));
  }
  end();
  for (std::thread& t : callers) t.join();
  EXPECT_EQ(ended.load(), 4);
  EXPECT_FALSE(client.connected());
  EXPECT_EQ(client.close_status(), close_status);
  EXPECT_EQ(shutdowns.load(), close_status == Status::kOk ? 0 : 1);
  // Nothing more is sent, and asking again fails at once.
  kcpc::Stamp stamp;
  EXPECT_EQ(client.pause(stamp).outcome, Outcome::kClosed);
  EXPECT_EQ(client.send(Opcode::kPing, {}, nullptr), 0U);
}

}  // namespace

TEST(KcpcClientClose, ShutdownWakesEveryPendingCallWithAnError) {
  SilentPeer peer;
  expect_pending_calls_end(
      peer, [&] { peer.shut_down(); }, Status::kShuttingDown);
}

TEST(KcpcClientClose, AConnectionThatJustEndsWakesThemToo) {
  SilentPeer peer;
  expect_pending_calls_end(peer, [&] { peer.drop(); }, Status::kOk);
}

// -- the frame ring ----------------------------------------------------------

namespace {

constexpr std::uint32_t kRingWidth = 64;
constexpr std::uint32_t kRingHeight = 16;
constexpr std::uint32_t kRingStride = kRingWidth * 3;

std::uint32_t crc32(const std::vector<std::uint8_t>& data) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::uint8_t const byte : data) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

std::vector<std::uint8_t> frame_pixels(std::uint64_t n) {
  std::vector<std::uint8_t> pixels(std::size_t{kRingStride} * kRingHeight);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<std::uint8_t>(i * 13 + n * 7);
  }
  return pixels;
}

// A ring of the test's own, under a name no core uses.
class FbRingReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_ = "kcpc-fbr-" + std::to_string(startup_manifest_self_pid());
    std::string error;
    ASSERT_TRUE(region_.create(
        base_, kcpc::fb_ring_bytes(kRingStride, kRingHeight), error))
        << error;
    ASSERT_TRUE(writer_.init(region_.data(), region_.size(), kRingWidth,
                             kRingHeight, kRingStride, 99));
  }

  kcpc::FbRingHeader& header() {
    return *static_cast<kcpc::FbRingHeader*>(region_.data());
  }

  std::string base_;
  shm::Region region_;
  kcpc::FbRingWriter writer_;
};

}  // namespace

TEST_F(FbRingReaderTest, ReadsTheLatestFrameWhole) {
  kcpc::FbRingReader reader;
  std::string error;
  ASSERT_TRUE(reader.open(shm::os_name(base_), error)) << error;
  EXPECT_EQ(reader.width(), kRingWidth);
  EXPECT_EQ(reader.height(), kRingHeight);
  EXPECT_EQ(reader.stride(), kRingStride);
  EXPECT_EQ(reader.producer_pid(), 99U);
  std::vector<std::uint8_t> got;
  std::uint64_t seq = 0;
  EXPECT_EQ(reader.read_latest(got, seq), kcpc::FbReadStatus::kEmpty);

  for (std::uint64_t n = 1; n <= 6; ++n) {
    std::vector<std::uint8_t> const sent = frame_pixels(n);
    ASSERT_EQ(writer_.publish(sent.data()), n);
    ASSERT_EQ(reader.read_latest(got, seq), kcpc::FbReadStatus::kOk);
    EXPECT_EQ(seq, n);
    EXPECT_EQ(reader.latest_seq(), n);
    EXPECT_EQ(crc32(got), crc32(sent)) << "frame " << n;
  }
}

TEST_F(FbRingReaderTest, ATornSlotIsRetriedAndNeverAccepted) {
  kcpc::FbRingReader reader;
  std::string error;
  ASSERT_TRUE(reader.open(shm::os_name(base_), error)) << error;
  std::vector<std::uint8_t> const sent = frame_pixels(1);
  writer_.publish(sent.data());
  // As if the producer had started rewriting the latest slot: odd.
  auto const slot = header().latest_slot.load();
  std::uint64_t const whole = header().slot_seq[slot].load();
  header().slot_seq[slot].store(whole + 1);
  std::vector<std::uint8_t> got;
  std::uint64_t seq = 0;
  EXPECT_EQ(reader.read_latest(got, seq), kcpc::FbReadStatus::kTorn);
  header().slot_seq[slot].store(whole);
  ASSERT_EQ(reader.read_latest(got, seq), kcpc::FbReadStatus::kOk);
  EXPECT_EQ(crc32(got), crc32(sent));
}

TEST_F(FbRingReaderTest, AReadBesideTheProducerIsAlwaysOneWholeFrame) {
  kcpc::FbRingReader reader;
  std::string error;
  ASSERT_TRUE(reader.open(shm::os_name(base_), error)) << error;
  std::atomic<bool> done{false};
  std::thread producer([&] {
    std::vector<std::uint8_t> pixels(std::size_t{kRingStride} * kRingHeight);
    for (std::uint64_t n = 1; n <= 20000; ++n) {
      std::fill(pixels.begin(), pixels.end(), static_cast<std::uint8_t>(n));
      writer_.publish(pixels.data());
    }
    done = true;
  });
  std::vector<std::uint8_t> got;
  int whole = 0;
  while (!done.load()) {
    std::uint64_t seq = 0;
    if (reader.read_latest(got, seq) != kcpc::FbReadStatus::kOk) continue;
    auto const want = static_cast<std::uint8_t>(seq);
    ASSERT_EQ(std::count(got.begin(), got.end(), want),
              static_cast<std::ptrdiff_t>(got.size()))
        << "frame " << seq << " was torn";
    ++whole;
  }
  producer.join();
  EXPECT_GT(whole, 0);
}

TEST(FbRingReader, NoRegionOrNoRingFailsToOpen) {
  kcpc::FbRingReader reader;
  std::string error;
  EXPECT_FALSE(reader.open("", error));
  EXPECT_FALSE(reader.open(shm::os_name("kcpc-fbr-none-" +
                                        std::to_string(startup_manifest_self_pid())),
                           error));
  EXPECT_FALSE(reader.is_open());
  // A region whose magic was never stored is no ring (yet).
  shm::Region blank;
  std::string const base =
      "kcpc-fbz-" + std::to_string(startup_manifest_self_pid());
  ASSERT_TRUE(blank.create(base, 4096, error)) << error;
  EXPECT_FALSE(reader.open(shm::os_name(base), error));
  std::vector<std::uint8_t> got;
  std::uint64_t seq = 0;
  EXPECT_EQ(reader.read_latest(got, seq), kcpc::FbReadStatus::kBadHeader);
}
