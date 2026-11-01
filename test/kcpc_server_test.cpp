// konCePCja — tests for the KCPC binary server over real loopback sockets
// (beads-3o1.1).
//
// The server gets its machine through a callback, so these tests run the real
// acceptor, reader and writer threads against a fake machine. The same
// behaviour is checked end to end against a `-H` core by
// test/integrated/kcpc/test_kcpc.py; what only a unit test can do is make the
// machine refuse (BUSY), shrink the limits, and time stop().

#include "kcpc/kcpc_server.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kcpc/kcpc_proto.h"
#include "kcpc_fake_machine.h"
#include "net_socket.h"

namespace {

using Clock = std::chrono::steady_clock;
using kcpc::Opcode;
using kcpc::Status;
using kcpc_fake::fake_machine;
using kcpc_fake::FakeBreakpoints;
using kcpc_fake::FakeDevtools;
using kcpc_fake::FakeInput;
using kcpc_fake::FakeMedia;
using kcpc_fake::FakeReads;
using kcpc_fake::FakeRun;
using kcpc_fake::FakeWrites;

// Away from 6545 so a running emulator is never in the way, with a wide probe
// range so two test_runners at once (another worktree) do not collide.
constexpr int kTestBasePort = 47545;
constexpr int kTestProbeCount = 40;
constexpr int kReplyTimeoutMs = 5000;

// The groups of slice 7, which a fixture without them does not announce.
constexpr std::uint64_t kDevtoolsCaps =
    kcpc::kCapChips | kcpc::kCapSymbols | kcpc::kCapFbInfo | kcpc::kCapFbShm;
// And those of slice 9.
constexpr std::uint64_t kMediaCaps =
    kcpc::kCapMedia | kcpc::kCapSnapshot | kcpc::kCapConfig;

kcpc::ServerConfig test_config() {
  kcpc::ServerConfig c;
  c.base_port = kTestBasePort;
  c.probe_count = kTestProbeCount;
  c.server_version = "v0.0.0-test";
  c.pid = 4242;
  c.machine_info = [](kcpc::MachineInfo& info) {
    info = fake_machine();
    return Status::kOk;
  };
  return c;
}

struct Frame {
  kcpc::FrameHeader header;
  std::vector<std::uint8_t> payload;

  Status status() const {
    kcpc::Reader r(payload);
    std::uint16_t s = 0xFFFF;
    r.u16(s);
    return static_cast<Status>(s);
  }
  // The words of a refusal: the `str` after the status header.
  std::string message() const {
    kcpc::Reader r(payload);
    std::string text;
    r.skip(4);
    r.str(text);
    return text;
  }
};

// A blocking-with-deadline client on the same socket shim the server uses.
class TestClient {
 public:
  explicit TestClient(int port)
      : socket_(net::connect_loopback(port, kReplyTimeoutMs)) {}
  ~TestClient() { close(); }
  TestClient(const TestClient&) = delete;
  TestClient& operator=(const TestClient&) = delete;

  bool connected() const { return socket_ != net::kInvalidSocket; }
  void close() {
    net::close_socket(socket_);
    socket_ = net::kInvalidSocket;
  }

  bool send(const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    auto const deadline =
        Clock::now() + std::chrono::milliseconds(kReplyTimeoutMs);
    while (sent < bytes.size()) {
      net::IoResult const r =
          net::send_some(socket_, bytes.data() + sent, bytes.size() - sent);
      if (r.status == net::IoStatus::kOk) {
        sent += r.bytes;
      } else if (r.status == net::IoStatus::kWouldBlock) {
        if (Clock::now() >= deadline) return false;
        net::wait_writable(socket_, 20);
      } else {
        return false;
      }
    }
    return true;
  }

  bool request(Opcode opcode, std::uint32_t id,
               const std::vector<std::uint8_t>& payload = {}) {
    return send(kcpc::encode_frame(
        kcpc::Kind::kRequest, static_cast<std::uint16_t>(opcode), id, payload));
  }

  bool hello(std::uint32_t id, const std::string& name = "test",
             std::uint32_t max_frame = 0, std::uint8_t role = 0) {
    kcpc::HelloRequest h;
    h.role = role;
    h.max_frame = max_frame;
    h.client_name = name;
    kcpc::Writer w;
    kcpc::write_hello_request(w, h);
    return request(Opcode::kHello, id, w.take());
  }

  // The next response (or ERROR). Events that arrive first are kept in
  // `events`: a response is matched by id, never by its place in the stream.
  bool receive(Frame& out, int timeout_ms = kReplyTimeoutMs) {
    for (;;) {
      if (!receive_any(out, timeout_ms)) return false;
      if (out.header.kind != static_cast<std::uint8_t>(kcpc::Kind::kEvent)) {
        return true;
      }
      events.push_back(out);
    }
  }

  // The next event, in the order the server sent them. False when a
  // response arrives instead, or nothing does in time.
  bool next_event(Frame& out, int timeout_ms = kReplyTimeoutMs) {
    if (!events.empty()) {
      out = events.front();
      events.erase(events.begin());
      return true;
    }
    return receive_any(out, timeout_ms) &&
           out.header.kind == static_cast<std::uint8_t>(kcpc::Kind::kEvent);
  }

  std::vector<Frame> events;

  // One whole frame of any kind, or false on timeout, close or a malformed
  // header.
  bool receive_any(Frame& out, int timeout_ms = kReplyTimeoutMs) {
    std::uint8_t raw[kcpc::kHeaderSize];
    if (!read_exact(raw, sizeof(raw), timeout_ms)) return false;
    if (kcpc::parse_header(raw, kcpc::kMaxServerFrame, out.header) !=
        kcpc::HeaderError::kOk) {
      return false;
    }
    out.payload.resize(out.header.length);
    return read_exact(out.payload.data(), out.payload.size(), timeout_ms);
  }

  // True when the server closed the connection and sent nothing more.
  bool closed_by_peer(int timeout_ms = kReplyTimeoutMs) {
    auto const deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::uint8_t byte = 0;
    while (Clock::now() < deadline) {
      if (!net::wait_readable(socket_, 20)) continue;
      net::IoResult const r = net::recv_some(socket_, &byte, 1);
      if (r.status == net::IoStatus::kClosed) return true;
      if (r.status == net::IoStatus::kOk) return false;  // stray data
      if (r.status == net::IoStatus::kError) return true;
    }
    return false;
  }

 private:
  bool read_exact(std::uint8_t* buf, std::size_t size, int timeout_ms) {
    std::size_t got = 0;
    auto const deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (got < size) {
      if (Clock::now() >= deadline) return false;
      if (!net::wait_readable(socket_, 20)) continue;
      net::IoResult const r = net::recv_some(socket_, buf + got, size - got);
      if (r.status == net::IoStatus::kOk) {
        got += r.bytes;
      } else if (r.status != net::IoStatus::kWouldBlock) {
        return false;
      }
    }
    return true;
  }

  net::Socket socket_;
};

// The server's last word on a connection it will not serve: exactly one ERROR
// frame (a response, opcode 0, id 0) with `expected`, then end of stream.
void expect_error_then_close(TestClient& client, Status expected) {
  Frame frame;
  ASSERT_TRUE(client.receive(frame)) << "no ERROR frame arrived";
  EXPECT_EQ(frame.header.kind,
            static_cast<std::uint8_t>(kcpc::Kind::kResponse));
  EXPECT_EQ(frame.header.opcode, static_cast<std::uint16_t>(Opcode::kError));
  EXPECT_EQ(frame.header.id, 0U);
  EXPECT_EQ(frame.status(), expected);
  kcpc::Reader r(frame.payload);
  std::string message;
  r.skip(4);
  EXPECT_TRUE(r.str(message));
  EXPECT_FALSE(message.empty()) << "an ERROR carries a message for the log";
  EXPECT_TRUE(client.closed_by_peer()) << "the connection must be closed";
}

void expect_hello_ok(TestClient& client, std::uint32_t id,
                     kcpc::HelloReply& reply) {
  ASSERT_TRUE(client.hello(id));
  Frame frame;
  ASSERT_TRUE(client.receive(frame));
  ASSERT_EQ(frame.header.opcode, static_cast<std::uint16_t>(Opcode::kHello));
  ASSERT_EQ(frame.header.id, id);
  ASSERT_EQ(frame.status(), Status::kOk);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  ASSERT_TRUE(kcpc::read_hello_reply(r, reply));
}

void expect_ping_ok(TestClient& client, std::uint32_t id) {
  ASSERT_TRUE(client.request(Opcode::kPing, id));
  Frame frame;
  ASSERT_TRUE(client.receive(frame));
  EXPECT_EQ(frame.header.kind,
            static_cast<std::uint8_t>(kcpc::Kind::kResponse));
  EXPECT_EQ(frame.header.opcode, static_cast<std::uint16_t>(Opcode::kPing));
  EXPECT_EQ(frame.header.id, id);
  EXPECT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U) << "PING replies with the status alone";
}

std::vector<std::uint8_t> raw_header(std::uint8_t version, std::uint8_t kind,
                                     std::uint16_t opcode, std::uint32_t id,
                                     std::uint32_t length) {
  kcpc::Writer w;
  w.bytes(kcpc::kMagic, sizeof(kcpc::kMagic));
  w.u8(version);
  w.u8(kind);
  w.u16(opcode);
  w.u32(id);
  w.u32(length);
  return w.take();
}

class KcpcServerTest : public ::testing::Test {
 protected:
  void start(const kcpc::ServerConfig& config) {
    ASSERT_TRUE(server_.start(config));
    ASSERT_GT(server_.port(), 0);
  }
  // The fake machines are members of the fixtures derived from this one, so
  // they go before server_ does. A connection that ends may still call one
  // (the keys a controller held are released when it leaves): stop first.
  void TearDown() override { server_.stop(); }
  kcpc::Server server_;
};

}  // namespace

TEST_F(KcpcServerTest, HelloReportsTheServerAndTheMachine) {
  start(test_config());
  TestClient client(server_.port());
  ASSERT_TRUE(client.connected());

  kcpc::HelloReply reply;
  expect_hello_ok(client, 1, reply);
  EXPECT_EQ(reply.server_minor, 0);
  EXPECT_EQ(reply.role, kcpc::Role::kObserver);
  EXPECT_EQ(reply.max_frame, kcpc::kMaxServerFrame);
  EXPECT_EQ(reply.caps, kcpc::kCapEvents)
      << "this machine offers no reads; events are the server's own";
  EXPECT_NE(reply.client_id, 0U);
  EXPECT_EQ(reply.pid, 4242U);
  EXPECT_EQ(reply.server_version, "v0.0.0-test");
  EXPECT_EQ(reply.machine.model, 2);
  EXPECT_EQ(reply.machine.crtc_type, 1);
  EXPECT_EQ(reply.machine.ram_bytes, 128U * 1024U);
  EXPECT_EQ(reply.machine.rom_slots, 0x81U);
  EXPECT_EQ(reply.machine.stamp.frame, 1234U);

  expect_ping_ok(client, 2);
  EXPECT_EQ(server_.client_count(), 1U);
}

// The control role comes with the machine writes (CAP_MACHINE_WRITE). A
// server without them has no controller: asking for control in HELLO is valid
// and is answered with the role actually granted.
TEST_F(KcpcServerTest, WithoutMachineWritesEveryClientIsAnObserver) {
  start(test_config());
  TestClient client(server_.port());
  ASSERT_TRUE(client.hello(1, "wants-control", 0, 1));
  Frame frame;
  ASSERT_TRUE(client.receive(frame));
  ASSERT_EQ(frame.status(), Status::kOk);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::HelloReply reply;
  ASSERT_TRUE(kcpc::read_hello_reply(r, reply));
  EXPECT_EQ(reply.role, kcpc::Role::kObserver);
  EXPECT_EQ(server_.controller(), 0U);
  EXPECT_EQ(reply.caps & kcpc::kCapMachineWrite, 0U);

  // And the opcodes of the group do not exist.
  for (Opcode const opcode :
       {Opcode::kControl, Opcode::kRegsSet, Opcode::kMemWrite}) {
    ASSERT_TRUE(client.request(opcode, 2, std::vector<std::uint8_t>(64)));
    ASSERT_TRUE(client.receive(frame));
    EXPECT_EQ(frame.status(), Status::kUnknownOpcode);
  }
}

TEST_F(KcpcServerTest, MaxFrameIsTheSmallerOfTheTwoLimits) {
  start(test_config());
  {
    TestClient client(server_.port());
    ASSERT_TRUE(client.hello(1, "small", 1U << 20));
    Frame frame;
    ASSERT_TRUE(client.receive(frame));
    kcpc::Reader r(frame.payload);
    r.skip(4);
    kcpc::HelloReply reply;
    ASSERT_TRUE(kcpc::read_hello_reply(r, reply));
    EXPECT_EQ(reply.max_frame, 1U << 20);
  }
  {
    TestClient client(server_.port());
    ASSERT_TRUE(client.hello(1, "huge", 0xFFFFFFFFU));
    Frame frame;
    ASSERT_TRUE(client.receive(frame));
    kcpc::Reader r(frame.payload);
    r.skip(4);
    kcpc::HelloReply reply;
    ASSERT_TRUE(kcpc::read_hello_reply(r, reply));
    EXPECT_EQ(reply.max_frame, kcpc::kMaxServerFrame);
  }
}

TEST_F(KcpcServerTest, MachineInfoMatchesHello) {
  start(test_config());
  TestClient client(server_.port());
  kcpc::HelloReply reply;
  expect_hello_ok(client, 1, reply);

  ASSERT_TRUE(client.request(Opcode::kMachineInfo, 9));
  Frame frame;
  ASSERT_TRUE(client.receive(frame));
  EXPECT_EQ(frame.header.id, 9U);
  ASSERT_EQ(frame.status(), Status::kOk);
  // 4 status + 12 fields + 12 stamp.
  EXPECT_EQ(frame.payload.size(), 28U);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::MachineInfo info;
  ASSERT_TRUE(kcpc::read_machine_info(r, info));
  EXPECT_EQ(info.model, reply.machine.model);
  EXPECT_EQ(info.flags, reply.machine.flags);
  EXPECT_EQ(info.ram_bytes, reply.machine.ram_bytes);
}

// Acceptance 2: pipelined requests are answered in the order they arrived.
TEST_F(KcpcServerTest, PipelinedPingsAreAnsweredInOrder) {
  start(test_config());
  TestClient client(server_.port());
  kcpc::HelloReply reply;
  expect_hello_ok(client, 1, reply);

  constexpr std::uint32_t kCount = 500;
  std::vector<std::uint8_t> burst;
  for (std::uint32_t i = 0; i < kCount; ++i) {
    std::vector<std::uint8_t> const frame = kcpc::encode_frame(
        kcpc::Kind::kRequest, static_cast<std::uint16_t>(Opcode::kPing),
        1000 + i, {});
    burst.insert(burst.end(), frame.begin(), frame.end());
  }
  ASSERT_TRUE(client.send(burst));
  for (std::uint32_t i = 0; i < kCount; ++i) {
    Frame frame;
    ASSERT_TRUE(client.receive(frame)) << "reply " << i << " never arrived";
    ASSERT_EQ(frame.header.id, 1000 + i);
    ASSERT_EQ(frame.status(), Status::kOk);
  }
}

// Acceptance 2: two clients at once, each answered with its own ids.
TEST_F(KcpcServerTest, TwoClientsAreServedConcurrently) {
  start(test_config());
  std::atomic<int> failures{0};
  std::atomic<std::uint32_t> ids[2] = {};
  auto worker = [&](int index) {
    TestClient client(server_.port());
    std::uint32_t const base = index == 0 ? 0x10000U : 0x20000U;
    Frame frame;
    if (!client.hello(base) || !client.receive(frame) ||
        frame.header.id != base || frame.status() != Status::kOk) {
      failures.fetch_add(1);
      return;
    }
    kcpc::Reader r(frame.payload);
    r.skip(4);
    kcpc::HelloReply reply;
    if (!kcpc::read_hello_reply(r, reply)) {
      failures.fetch_add(1);
      return;
    }
    ids[index].store(reply.client_id);
    for (std::uint32_t i = 1; i <= 200; ++i) {
      if (!client.request(Opcode::kPing, base + i) || !client.receive(frame) ||
          frame.header.id != base + i || frame.status() != Status::kOk) {
        failures.fetch_add(1);
        return;
      }
    }
  };
  std::thread a(worker, 0);
  std::thread b(worker, 1);
  a.join();
  b.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_NE(ids[0].load(), 0U);
  EXPECT_NE(ids[1].load(), 0U);
  EXPECT_NE(ids[0].load(), ids[1].load()) << "client ids must be unique";
}

// Acceptance 3, the frame faults: one ERROR frame, then the socket closes.
TEST_F(KcpcServerTest, BadMagicClosesWithBadFrame) {
  start(test_config());
  TestClient client(server_.port());
  // A text IPC client that found the wrong port.
  ASSERT_TRUE(client.send({'p', 'i', 'n', 'g', '\n', 'p', 'i', 'n', 'g', '\n',
                           'p', 'i', 'n', 'g', '\n', '\n'}));
  expect_error_then_close(client, Status::kBadFrame);
}

TEST_F(KcpcServerTest, WrongMajorClosesWithUnsupportedVersion) {
  start(test_config());
  TestClient client(server_.port());
  ASSERT_TRUE(client.send(raw_header(1, 1, 1, 1, 0)));
  expect_error_then_close(client, Status::kUnsupportedVersion);
}

TEST_F(KcpcServerTest, UnknownKindClosesWithBadFrame) {
  start(test_config());
  TestClient client(server_.port());
  ASSERT_TRUE(client.send(raw_header(0, 7, 1, 1, 0)));
  expect_error_then_close(client, Status::kBadFrame);
}

// Responses and events only ever travel from server to client.
TEST_F(KcpcServerTest, AResponseFromAClientClosesWithBadFrame) {
  start(test_config());
  TestClient client(server_.port());
  ASSERT_TRUE(client.send(raw_header(0, 2, 1, 1, 0)));
  expect_error_then_close(client, Status::kBadFrame);
}

// The length is refused from the header: the server must not wait for (or
// allocate) the 4 GiB the client claims will follow.
TEST_F(KcpcServerTest, OversizedLengthClosesWithBadFrame) {
  start(test_config());
  TestClient client(server_.port());
  ASSERT_TRUE(client.send(raw_header(0, 1, 2, 1, 0xFFFFFFFFU)));
  expect_error_then_close(client, Status::kBadFrame);
}

TEST_F(KcpcServerTest, RequestIdZeroClosesWithBadFrame) {
  start(test_config());
  TestClient client(server_.port());
  ASSERT_TRUE(client.hello(0));
  expect_error_then_close(client, Status::kBadFrame);
}

TEST_F(KcpcServerTest, FirstFrameNotHelloClosesWithHelloRequired) {
  start(test_config());
  TestClient client(server_.port());
  ASSERT_TRUE(client.request(Opcode::kPing, 1));
  expect_error_then_close(client, Status::kHelloRequired);
}

TEST_F(KcpcServerTest, SecondHelloClosesWithHelloRequired) {
  start(test_config());
  TestClient client(server_.port());
  kcpc::HelloReply reply;
  expect_hello_ok(client, 1, reply);
  ASSERT_TRUE(client.hello(2));
  expect_error_then_close(client, Status::kHelloRequired);
}

// The ERROR must survive requests the client had already pipelined behind the
// bad one: closing a socket with unread input resets the connection, and a
// reset can destroy the frame before the client reads it.
TEST_F(KcpcServerTest, ErrorIsDeliveredDespitePipelinedInput) {
  start(test_config());
  TestClient client(server_.port());
  std::vector<std::uint8_t> burst = raw_header(0, 1, 2, 1, 0);  // PING first
  for (int i = 0; i < 2000; ++i) {
    std::vector<std::uint8_t> const more = raw_header(0, 1, 2, 2 + i, 0);
    burst.insert(burst.end(), more.begin(), more.end());
  }
  ASSERT_TRUE(client.send(burst));
  expect_error_then_close(client, Status::kHelloRequired);
}

// A connection that never says HELLO must not hold a client place for ever.
TEST_F(KcpcServerTest, SilentConnectionIsClosedAfterTheHelloTimeout) {
  kcpc::ServerConfig config = test_config();
  config.hello_timeout_ms = 150;
  start(config);
  TestClient client(server_.port());
  ASSERT_TRUE(client.connected());
  expect_error_then_close(client, Status::kHelloRequired);
}

// A client that has completed HELLO is never timed out for being quiet.
TEST_F(KcpcServerTest, AnIdleClientIsNotTimedOut) {
  kcpc::ServerConfig config = test_config();
  config.hello_timeout_ms = 150;
  start(config);
  TestClient client(server_.port());
  kcpc::HelloReply reply;
  expect_hello_ok(client, 1, reply);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  expect_ping_ok(client, 2);
}

// Faults that leave the connection open.
TEST_F(KcpcServerTest, UnknownOpcodeKeepsTheConnection) {
  start(test_config());
  TestClient client(server_.port());
  kcpc::HelloReply reply;
  expect_hello_ok(client, 1, reply);

  // MEM_READ belongs to a capability this server was not given: an opcode
  // from a group whose bit is clear answers UNKNOWN_OPCODE.
  EXPECT_EQ(reply.caps & kcpc::kCapMachineRead, 0U);
  ASSERT_TRUE(client.request(Opcode::kMemRead, 2, {0, 0, 0, 0}));
  Frame frame;
  ASSERT_TRUE(client.receive(frame));
  EXPECT_EQ(frame.header.opcode, static_cast<std::uint16_t>(Opcode::kMemRead));
  EXPECT_EQ(frame.header.id, 2U);
  EXPECT_EQ(frame.status(), Status::kUnknownOpcode);

  ASSERT_TRUE(client.request(static_cast<Opcode>(0x7FFF), 3));
  ASSERT_TRUE(client.receive(frame));
  EXPECT_EQ(frame.header.id, 3U);
  EXPECT_EQ(frame.status(), Status::kUnknownOpcode);

  expect_ping_ok(client, 4);
}

TEST_F(KcpcServerTest, ARefusedHelloMayBeRetried) {
  start(test_config());
  TestClient client(server_.port());
  Frame frame;

  // Too short.
  ASSERT_TRUE(client.request(Opcode::kHello, 1, {0, 0, 0}));
  ASSERT_TRUE(client.receive(frame));
  EXPECT_EQ(frame.header.id, 1U);
  EXPECT_EQ(frame.status(), Status::kBadPayload);

  // A role that does not exist.
  ASSERT_TRUE(client.hello(2, "bad-role", 0, 9));
  ASSERT_TRUE(client.receive(frame));
  EXPECT_EQ(frame.status(), Status::kBadPayload);

  // A max_frame too small to carry a 64 KiB memory read.
  ASSERT_TRUE(client.hello(3, "tiny", 1024));
  ASSERT_TRUE(client.receive(frame));
  EXPECT_EQ(frame.status(), Status::kBadPayload);

  // Not a client yet: PING is still refused... by closing, so do not send
  // one. A good HELLO completes the handshake on the same connection.
  kcpc::HelloReply reply;
  expect_hello_ok(client, 4, reply);
  expect_ping_ok(client, 5);
}

// The machine did not go idle: the request changed nothing and says BUSY.
TEST_F(KcpcServerTest, BusyMachineAnswersBusy) {
  std::atomic<bool> busy{true};
  kcpc::ServerConfig config = test_config();
  config.machine_info = [&busy](kcpc::MachineInfo& info) {
    if (busy.load()) return Status::kBusy;
    info = fake_machine();
    return Status::kOk;
  };
  start(config);
  TestClient client(server_.port());
  Frame frame;
  ASSERT_TRUE(client.hello(1));
  ASSERT_TRUE(client.receive(frame));
  EXPECT_EQ(frame.status(), Status::kBusy);

  busy.store(false);
  kcpc::HelloReply reply;
  expect_hello_ok(client, 2, reply);

  busy.store(true);
  ASSERT_TRUE(client.request(Opcode::kMachineInfo, 3));
  ASSERT_TRUE(client.receive(frame));
  EXPECT_EQ(frame.header.id, 3U);
  EXPECT_EQ(frame.status(), Status::kBusy);
  expect_ping_ok(client, 4);
}

// Acceptance 3: the client beyond the limit gets TOO_MANY_CLIENTS at once,
// without having to say HELLO first, and the place it wanted frees up when a
// client leaves.
TEST_F(KcpcServerTest, ClientBeyondTheLimitIsRefused) {
  start(test_config());
  std::vector<std::unique_ptr<TestClient>> clients;
  for (std::size_t i = 0; i < kcpc::kMaxClients; ++i) {
    clients.push_back(std::make_unique<TestClient>(server_.port()));
    kcpc::HelloReply reply;
    expect_hello_ok(*clients.back(), 1, reply);
  }
  EXPECT_EQ(server_.client_count(), kcpc::kMaxClients);

  {
    TestClient ninth(server_.port());
    ASSERT_TRUE(ninth.connected());
    expect_error_then_close(ninth, Status::kTooManyClients);
  }
  // The eight are untouched.
  for (auto& client : clients) expect_ping_ok(*client, 2);

  // One leaves; its place is free as soon as the server notices.
  clients.pop_back();
  auto const deadline = Clock::now() + std::chrono::seconds(5);
  while (server_.client_count() >= kcpc::kMaxClients &&
         Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_LT(server_.client_count(), kcpc::kMaxClients);
  TestClient replacement(server_.port());
  kcpc::HelloReply reply;
  expect_hello_ok(replacement, 1, reply);
}

// Acceptance 4: the port is probed forward, like the text IPC's.
TEST_F(KcpcServerTest, SecondServerLandsOnTheNextFreePort) {
  start(test_config());
  kcpc::Server second;
  ASSERT_TRUE(second.start(test_config()));
  EXPECT_GT(second.port(), server_.port());
  EXPECT_LT(second.port(), kTestBasePort + kTestProbeCount);

  // Each port reaches its own server.
  TestClient a(server_.port());
  TestClient b(second.port());
  kcpc::HelloReply ra;
  kcpc::HelloReply rb;
  expect_hello_ok(a, 1, ra);
  expect_hello_ok(b, 1, rb);
  EXPECT_NE(ra.client_id, rb.client_id);
  second.stop();
  EXPECT_EQ(second.port(), 0);
}

TEST_F(KcpcServerTest, NoFreePortFailsCleanly) {
  kcpc::ServerConfig config = test_config();
  start(config);
  config.base_port = server_.port();
  config.probe_count = 1;
  kcpc::Server second;
  EXPECT_FALSE(second.start(config));
  EXPECT_FALSE(second.running());
  EXPECT_EQ(second.port(), 0);
  second.stop();  // harmless on a server that never started
}

// Acceptance 5: stop() returns promptly whatever the clients are doing, and a
// client that completed HELLO is told why it is being dropped.
TEST_F(KcpcServerTest, StopJoinsEveryThreadWithClientsConnected) {
  start(test_config());
  int const port = server_.port();

  TestClient said_hello(port);
  kcpc::HelloReply reply;
  expect_hello_ok(said_hello, 1, reply);
  TestClient silent(port);
  ASSERT_TRUE(silent.connected());
  TestClient mid_header(port);
  ASSERT_TRUE(mid_header.send({0x4B, 0x43, 0x50, 0x43, 0x00}));
  TestClient mid_payload(port);
  ASSERT_TRUE(mid_payload.send(raw_header(0, 1, 1, 1, 4096)));

  auto const begin = Clock::now();
  server_.stop();
  auto const took = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - begin);
  EXPECT_LT(took.count(), 2000) << "stop() took " << took.count() << " ms";
  EXPECT_FALSE(server_.running());
  EXPECT_EQ(server_.port(), 0);
  EXPECT_EQ(server_.client_count(), 0U);

  expect_error_then_close(said_hello, Status::kShuttingDown);
  expect_error_then_close(silent, Status::kShuttingDown);

  // Nothing listens there any more.
  TestClient late(port);
  EXPECT_FALSE(late.connected() && late.hello(1) && [&late] {
    Frame frame;
    return late.receive(frame, 500);
  }());
}

TEST_F(KcpcServerTest, RestartsAndKeepsClientIdsUnique) {
  start(test_config());
  std::uint32_t first_id = 0;
  {
    TestClient client(server_.port());
    kcpc::HelloReply reply;
    expect_hello_ok(client, 1, reply);
    first_id = reply.client_id;
  }
  server_.stop();
  server_.stop();  // twice is fine
  start(test_config());
  TestClient client(server_.port());
  kcpc::HelloReply reply;
  expect_hello_ok(client, 1, reply);
  EXPECT_NE(reply.client_id, first_id)
      << "a client id is unique for the life of the core";
}

// ---- Slice 2: the machine reads (beads-3o1.2) ----------------------------

namespace {

std::vector<std::uint8_t> mem_read_payload(kcpc::MemSpace space,
                                           std::uint16_t bank,
                                           std::uint32_t addr,
                                           std::uint32_t len) {
  kcpc::MemReadRequest request;
  request.space = static_cast<std::uint8_t>(space);
  request.bank = bank;
  request.addr = addr;
  request.len = len;
  kcpc::Writer w;
  kcpc::write_mem_read_request(w, request);
  return w.take();
}

class KcpcReadsTest : public KcpcServerTest {
 protected:
  // Starts the server with the fake reads and completes HELLO on client_.
  void connect(std::uint32_t max_frame = 0) {
    kcpc::ServerConfig config = test_config();
    reads_.attach(config);
    start(config);
    client_ = std::make_unique<TestClient>(server_.port());
    ASSERT_TRUE(client_->hello(1, "reads", max_frame));
    Frame frame;
    ASSERT_TRUE(client_->receive(frame));
    ASSERT_EQ(frame.status(), Status::kOk);
    kcpc::Reader r(frame.payload);
    r.skip(4);
    ASSERT_TRUE(kcpc::read_hello_reply(r, hello_));
  }

  // One request and its response, which must carry the request's opcode and
  // id.
  void exchange(Opcode opcode, std::uint32_t id,
                const std::vector<std::uint8_t>& payload, Frame& frame) {
    ASSERT_TRUE(client_->request(opcode, id, payload));
    ASSERT_TRUE(client_->receive(frame));
    ASSERT_EQ(frame.header.kind,
              static_cast<std::uint8_t>(kcpc::Kind::kResponse));
    ASSERT_EQ(frame.header.opcode, static_cast<std::uint16_t>(opcode));
    ASSERT_EQ(frame.header.id, id);
  }

  FakeReads reads_;
  std::unique_ptr<TestClient> client_;
  kcpc::HelloReply hello_;
};

}  // namespace

TEST_F(KcpcReadsTest, HelloAnnouncesTheReadCapabilities) {
  connect();
  EXPECT_EQ(hello_.caps,
            kcpc::kCapMachineRead | kcpc::kCapMemRom | kcpc::kCapEvents);
}

TEST_F(KcpcReadsTest, RegsGetRepliesWithTheStampAndTheBlock) {
  connect();
  Frame frame;
  exchange(Opcode::kRegsGet, 2, {}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U + 12U + kcpc::kRegsSize);

  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::Stamp stamp;
  kcpc::Regs regs;
  ASSERT_TRUE(kcpc::read_stamp(r, stamp));
  ASSERT_TRUE(kcpc::read_regs(r, regs));
  EXPECT_EQ(stamp.frame, 77U);
  EXPECT_TRUE(stamp.paused);
  EXPECT_EQ(regs.af, 0x0102);
  EXPECT_EQ(regs.pc, 0x0F10);
  EXPECT_EQ(regs.hl_alt, 0x1718);
  EXPECT_EQ(regs.i, 0x19);
  EXPECT_EQ(regs.im, 2);
  EXPECT_EQ(regs.halt, 1);
  // On the wire: PC is the eighth 16-bit field, low byte first.
  EXPECT_EQ(frame.payload[16 + 14], 0x10);
  EXPECT_EQ(frame.payload[16 + 15], 0x0F);
}

TEST_F(KcpcReadsTest, MemInfoRepliesWithTheBankingState) {
  connect();
  Frame frame;
  exchange(Opcode::kMemInfo, 2, {}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U + 12U + 12U);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::MemInfo info;
  ASSERT_TRUE(kcpc::read_mem_info(r, info));
  EXPECT_EQ(info.ram_bytes, FakeReads::kSize);
  EXPECT_EQ(info.ram_config, 0xC4);
  EXPECT_EQ(info.rom_config, 0x89);
  EXPECT_EQ(info.upper_rom, 7);
  EXPECT_EQ(info.rom_slots, 0x80U);
  EXPECT_EQ(info.stamp.frame, 78U);
}

// The point of the port: 64 KiB of memory is one reply.
TEST_F(KcpcReadsTest, MemReadOf64KiBIsOneFrame) {
  connect();
  Frame frame;
  exchange(Opcode::kMemRead, 2,
           mem_read_payload(kcpc::MemSpace::kCpu, 0, 0, 0x10000), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), kcpc::kMemReadReplyOverhead + 0x10000U);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::Stamp stamp;
  ASSERT_TRUE(kcpc::read_stamp(r, stamp));
  EXPECT_EQ(stamp.frame, 79U);
  EXPECT_FALSE(stamp.paused);
  std::vector<std::uint8_t> bytes;
  ASSERT_TRUE(r.bytes(bytes));
  EXPECT_EQ(bytes, reads_.memory);
  expect_ping_ok(*client_, 3);
}

TEST_F(KcpcReadsTest, MemReadPassesTheRequestThrough) {
  connect();
  Frame frame;
  exchange(Opcode::kMemRead, 2,
           mem_read_payload(kcpc::MemSpace::kRomUpper, 7, 0x1234, 0x20), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(reads_.last.space,
            static_cast<std::uint8_t>(kcpc::MemSpace::kRomUpper));
  EXPECT_EQ(reads_.last.bank, 7);
  EXPECT_EQ(reads_.last.addr, 0x1234U);
  EXPECT_EQ(reads_.last.len, 0x20U);
  ASSERT_EQ(frame.payload.size(), kcpc::kMemReadReplyOverhead + 0x20U);
  for (std::uint32_t i = 0; i < 0x20; ++i) {
    ASSERT_EQ(frame.payload[kcpc::kMemReadReplyOverhead + i],
              reads_.memory[0x1234 + i])
        << i;
  }

  // An empty read is a stamp and no bytes.
  exchange(Opcode::kMemRead, 3,
           mem_read_payload(kcpc::MemSpace::kCpu, 0, 0x10000, 0), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), kcpc::kMemReadReplyOverhead);
}

// A read whose reply would not fit the frame limit HELLO agreed on is refused
// before the machine is asked and before anything is allocated.
TEST_F(KcpcReadsTest, MemReadOverTheFrameLimitIsTooLarge) {
  connect(kcpc::kMinClientMaxFrame);
  ASSERT_EQ(hello_.max_frame, kcpc::kMinClientMaxFrame);
  std::uint32_t const fits =
      kcpc::kMinClientMaxFrame - kcpc::kMemReadReplyOverhead;

  Frame frame;
  for (std::uint32_t const len : {fits + 1, 0x7FFFFFFFU, 0xFFFFFFFFU}) {
    exchange(Opcode::kMemRead, 2,
             mem_read_payload(kcpc::MemSpace::kPhysRam, 0, 0, len), frame);
    EXPECT_EQ(frame.status(), Status::kTooLarge) << len;
  }
  EXPECT_EQ(reads_.mem_reads.load(), 0) << "the machine was asked anyway";

  // Exactly at the limit the request goes through to the machine (which
  // here says the space is not that large).
  exchange(Opcode::kMemRead, 3,
           mem_read_payload(kcpc::MemSpace::kPhysRam, 0, 0, fits), frame);
  EXPECT_EQ(frame.status(), Status::kOutOfRange);
  EXPECT_EQ(reads_.mem_reads.load(), 1);
  expect_ping_ok(*client_, 4);
}

TEST_F(KcpcReadsTest, AShortMemReadRequestIsABadPayload) {
  connect();
  Frame frame;
  exchange(Opcode::kMemRead, 2, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  EXPECT_EQ(reads_.mem_reads.load(), 0);
  // Bytes after the fields are ignored: a later minor may append some.
  std::vector<std::uint8_t> longer =
      mem_read_payload(kcpc::MemSpace::kCpu, 0, 0, 4);
  longer.push_back(0xAA);
  exchange(Opcode::kMemRead, 3, longer, frame);
  EXPECT_EQ(frame.status(), Status::kOk);
}

// Whatever the machine answers is the status of the response, the message is
// there for the log, and the connection stays open.
TEST_F(KcpcReadsTest, TheMachinesRefusalIsTheStatus) {
  connect();
  std::uint32_t id = 2;
  for (Status const refusal :
       {Status::kBusy, Status::kOutOfRange, Status::kBadPayload,
        Status::kNotPresent, Status::kInternal}) {
    reads_.refuse.store(refusal);
    for (Opcode const opcode :
         {Opcode::kRegsGet, Opcode::kMemInfo, Opcode::kMemRead}) {
      Frame frame;
      exchange(opcode, id++,
               opcode == Opcode::kMemRead
                   ? mem_read_payload(kcpc::MemSpace::kCpu, 0, 0, 16)
                   : std::vector<std::uint8_t>{},
               frame);
      EXPECT_EQ(frame.status(), refusal);
      kcpc::Reader r(frame.payload);
      std::string message;
      r.skip(4);
      EXPECT_TRUE(r.str(message));
      EXPECT_FALSE(message.empty());
    }
  }
  reads_.refuse.store(Status::kOk);
  expect_ping_ok(*client_, id);
}

// Two clients read at once and each gets its own bytes under its own id.
TEST_F(KcpcReadsTest, TwoClientsReadConcurrently) {
  connect();
  std::atomic<int> failures{0};
  auto const reader = [&](std::uint32_t addr) {
    TestClient client(server_.port());
    Frame frame;
    if (!client.hello(1) || !client.receive(frame)) {
      failures.fetch_add(1);
      return;
    }
    for (std::uint32_t id = 2; id < 202; ++id) {
      if (!client.request(
              Opcode::kMemRead, id,
              mem_read_payload(kcpc::MemSpace::kCpu, 0, addr, 0x4000)) ||
          !client.receive(frame) || frame.header.id != id ||
          frame.status() != Status::kOk ||
          frame.payload.size() != kcpc::kMemReadReplyOverhead + 0x4000U ||
          !std::equal(frame.payload.begin() + kcpc::kMemReadReplyOverhead,
                      frame.payload.end(), reads_.memory.begin() + addr)) {
        failures.fetch_add(1);
        return;
      }
    }
  };
  std::thread a(reader, 0x0000U);
  std::thread b(reader, 0x8000U);
  a.join();
  b.join();
  EXPECT_EQ(failures.load(), 0);
}

// ---------------------------------------------------------------------------
// The control role and the machine writes (beads-3o1.3)
// ---------------------------------------------------------------------------

namespace {

std::vector<std::uint8_t> control_payload(std::uint8_t action,
                                          std::uint8_t flags = 0) {
  kcpc::Writer w;
  kcpc::write_control_request(w, {action, flags});
  return w.take();
}

std::vector<std::uint8_t> regs_set_payload(std::uint32_t mask,
                                           const kcpc::Regs& regs) {
  kcpc::Writer w;
  kcpc::write_regs_set_request(w, {mask, regs});
  return w.take();
}

std::vector<std::uint8_t> mem_write_payload(
    kcpc::MemSpace space, std::uint32_t addr,
    const std::vector<std::uint8_t>& data, std::uint16_t bank = 0) {
  kcpc::Writer w;
  kcpc::MemWriteRequest request;
  request.space = static_cast<std::uint8_t>(space);
  request.bank = bank;
  request.addr = addr;
  kcpc::write_mem_write_request(w, request);
  w.bytes(data.data(), data.size());
  return w.take();
}

class KcpcControlTest : public KcpcServerTest {
 protected:
  void start_writable(int idle_timeout_ms = 30000) {
    kcpc::ServerConfig config = test_config();
    reads_.attach(config);
    writes_.attach(config);
    config.controller_idle_timeout_ms = idle_timeout_ms;
    start(config);
  }

  // Connects and completes HELLO asking for `role`.
  std::unique_ptr<TestClient> join(kcpc::Role role, kcpc::HelloReply& reply) {
    auto client = std::make_unique<TestClient>(server_.port());
    EXPECT_TRUE(
        client->hello(1, "control", 0, static_cast<std::uint8_t>(role)));
    Frame frame;
    EXPECT_TRUE(client->receive(frame));
    EXPECT_EQ(frame.status(), Status::kOk);
    kcpc::Reader r(frame.payload);
    r.skip(4);
    EXPECT_TRUE(kcpc::read_hello_reply(r, reply));
    return client;
  }

  static void exchange(TestClient& client, Opcode opcode, std::uint32_t id,
                       const std::vector<std::uint8_t>& payload, Frame& frame) {
    ASSERT_TRUE(client.request(opcode, id, payload));
    ASSERT_TRUE(client.receive(frame));
    ASSERT_EQ(frame.header.opcode, static_cast<std::uint16_t>(opcode));
    ASSERT_EQ(frame.header.id, id);
  }

  static void control(TestClient& client, std::uint32_t id, std::uint8_t action,
                      std::uint8_t flags, kcpc::ControlReply& reply) {
    Frame frame;
    exchange(client, Opcode::kControl, id, control_payload(action, flags),
             frame);
    ASSERT_EQ(frame.status(), Status::kOk);
    EXPECT_EQ(frame.payload.size(), 4U + 8U);
    kcpc::Reader r(frame.payload);
    r.skip(4);
    ASSERT_TRUE(kcpc::read_control_reply(r, reply));
  }

  static Status mem_write_status(TestClient& client, std::uint32_t id) {
    Frame frame;
    exchange(client, Opcode::kMemWrite, id,
             mem_write_payload(kcpc::MemSpace::kCpuRam, 0x4000, {1, 2, 3}),
             frame);
    return frame.status();
  }

  FakeReads reads_;
  FakeWrites writes_;
};

}  // namespace

TEST_F(KcpcControlTest, HelloGrantsControlToTheFirstWhoAsks) {
  start_writable();
  kcpc::HelloReply observer_hello;
  auto observer = join(kcpc::Role::kObserver, observer_hello);
  EXPECT_EQ(observer_hello.role, kcpc::Role::kObserver);
  EXPECT_EQ(observer_hello.caps, kcpc::kServerCaps & ~kcpc::kCapRunControl &
                                     ~kcpc::kCapBreakpoints & ~kcpc::kCapInput &
                                     ~kcpc::kCapGun & ~kDevtoolsCaps &
                                     ~kMediaCaps)
      << "everything but run control, breakpoints, input and the DevTools "
         "data, which this machine does not offer";
  EXPECT_NE(observer_hello.caps & kcpc::kCapMachineWrite, 0U);
  EXPECT_EQ(server_.controller(), 0U) << "nobody asked for control yet";

  kcpc::HelloReply first_hello;
  auto first = join(kcpc::Role::kController, first_hello);
  EXPECT_EQ(first_hello.role, kcpc::Role::kController);
  EXPECT_EQ(server_.controller(), first_hello.client_id);

  // HELLO never takes control away: the second asker observes.
  kcpc::HelloReply second_hello;
  auto second = join(kcpc::Role::kController, second_hello);
  EXPECT_EQ(second_hello.role, kcpc::Role::kObserver);
  EXPECT_EQ(server_.controller(), first_hello.client_id);
}

TEST_F(KcpcControlTest, ObserversWritesAnswerNotControllerAndReachNothing) {
  start_writable();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  auto observer = join(kcpc::Role::kController, hello);
  ASSERT_EQ(hello.role, kcpc::Role::kObserver);

  Frame frame;
  exchange(*observer, Opcode::kRegsSet, 2,
           regs_set_payload(1, FakeReads::regs()), frame);
  EXPECT_EQ(frame.status(), Status::kNotController);
  EXPECT_EQ(mem_write_status(*observer, 3), Status::kNotController);
  // The role is judged before the payload: an observer learns nothing about
  // a request it may not make, and the machine is never asked.
  exchange(*observer, Opcode::kMemWrite, 4, {}, frame);
  EXPECT_EQ(frame.status(), Status::kNotController);
  EXPECT_EQ(writes_.regs_sets.load(), 0);
  EXPECT_EQ(writes_.mem_writes.load(), 0);

  // It stays connected and may still read.
  exchange(*observer, Opcode::kRegsGet, 5, {}, frame);
  EXPECT_EQ(frame.status(), Status::kOk);
  // The controller's own write goes through.
  EXPECT_EQ(mem_write_status(*controller, 2), Status::kOk);
  EXPECT_EQ(writes_.mem_writes.load(), 1);
}

// Writes the controller sends together are made together, in one call of
// `writes` (one frame boundary or one pause lease in the core), and each is
// answered in the order it came: a refused one among them included, and the
// request that follows the run after all of them (beads-dws9).
TEST_F(KcpcControlTest, WritesSentTogetherAreMadeTogetherAndAnsweredInOrder) {
  kcpc::ServerConfig config = test_config();
  reads_.attach(config);
  writes_.attach(config);
  std::mutex mutex;
  std::vector<std::vector<std::uint16_t>> runs;  // the HL of each REGS_SET
  config.writes = [&](std::vector<kcpc::WriteOp>& ops) {
    std::vector<std::uint16_t> run;
    for (kcpc::WriteOp& op : ops) {
      op.status = Status::kOk;
      op.stamp = {77, false};
      if (!op.mem) {
        op.after = FakeReads::regs();
        op.after.hl = op.regs.regs.hl;
        run.push_back(op.regs.regs.hl);
      } else {
        run.push_back(static_cast<std::uint16_t>(op.len));
      }
    }
    std::scoped_lock const lock(mutex);
    runs.push_back(run);
  };
  start(config);
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  ASSERT_EQ(hello.role, kcpc::Role::kController);

  constexpr std::uint32_t kHl = 1U << 3;  // AF, BC, DE, HL, ...
  kcpc::Regs one = FakeReads::regs();
  one.hl = 0x1111;
  kcpc::Regs two = FakeReads::regs();
  two.hl = 0x2222;
  auto frame_of = [](Opcode opcode, std::uint32_t id,
                     const std::vector<std::uint8_t>& payload) {
    return kcpc::encode_frame(kcpc::Kind::kRequest,
                              static_cast<std::uint16_t>(opcode), id, payload);
  };
  std::vector<std::uint8_t> together;
  for (const auto& f :
       {frame_of(Opcode::kRegsSet, 2, regs_set_payload(kHl, one)),
        frame_of(Opcode::kMemWrite, 3,
                 mem_write_payload(kcpc::MemSpace::kCpuRam, 0x4000, {1, 2, 3})),
        frame_of(Opcode::kMemWrite, 4,
                 mem_write_payload(kcpc::MemSpace::kCpu, 0x4000, {9})),
        frame_of(Opcode::kRegsSet, 5, regs_set_payload(kHl, two)),
        frame_of(Opcode::kPing, 6, {})}) {
    together.insert(together.end(), f.begin(), f.end());
  }
  ASSERT_TRUE(controller->send(together));

  Status const expected[] = {Status::kOk, Status::kOk, Status::kBadPayload,
                             Status::kOk, Status::kOk};
  for (std::uint32_t id = 2; id <= 6; ++id) {
    Frame frame;
    ASSERT_TRUE(controller->receive(frame)) << "reply " << id;
    EXPECT_EQ(frame.header.id, id) << "the replies come in request order";
    EXPECT_EQ(frame.status(), expected[id - 2]) << "reply " << id;
    if (id == 5) {
      kcpc::Reader r(frame.payload);
      r.skip(4);
      kcpc::Stamp stamp;
      kcpc::Regs after;
      ASSERT_TRUE(kcpc::read_stamp(r, stamp));
      ASSERT_TRUE(kcpc::read_regs(r, after));
      EXPECT_EQ(stamp.frame, 77U);
      EXPECT_EQ(after.hl, 0x2222);
    }
  }
  // The request read behind the run was answered once.
  ASSERT_TRUE(controller->request(Opcode::kPing, 7));
  Frame frame;
  ASSERT_TRUE(controller->receive(frame));
  EXPECT_EQ(frame.header.id, 7U);
  std::scoped_lock const lock(mutex);
  ASSERT_EQ(runs.size(), 1U) << "the three writes that pass are one change";
  EXPECT_EQ(runs[0], (std::vector<std::uint16_t>{0x1111, 3, 0x2222}));
  EXPECT_EQ(writes_.regs_sets.load(), 0) << "not one by one";
  EXPECT_EQ(writes_.mem_writes.load(), 0);
}

TEST_F(KcpcControlTest, ControlIsAcquiredOnlyWhenFreeUnlessForced) {
  start_writable();
  kcpc::HelloReply a_hello;
  kcpc::HelloReply b_hello;
  auto a = join(kcpc::Role::kController, a_hello);
  auto b = join(kcpc::Role::kObserver, b_hello);

  // Asking politely while another client has control is not an error.
  kcpc::ControlReply reply;
  control(*b, 2, kcpc::kControlAcquire, 0, reply);
  EXPECT_EQ(reply.role, kcpc::Role::kObserver);
  EXPECT_EQ(reply.controller, a_hello.client_id);
  // Releasing what one does not hold changes nothing.
  control(*b, 3, kcpc::kControlRelease, 0, reply);
  EXPECT_EQ(reply.role, kcpc::Role::kObserver);
  EXPECT_EQ(reply.controller, a_hello.client_id);

  // Acquiring what one already holds is a no-op that says controller.
  control(*a, 2, kcpc::kControlAcquire, 0, reply);
  EXPECT_EQ(reply.role, kcpc::Role::kController);

  // Released, it is free, and then the polite request succeeds.
  control(*a, 3, kcpc::kControlRelease, 0, reply);
  EXPECT_EQ(reply.role, kcpc::Role::kObserver);
  EXPECT_EQ(reply.controller, 0U);
  EXPECT_EQ(server_.controller(), 0U);
  EXPECT_EQ(mem_write_status(*a, 4), Status::kNotController);
  control(*b, 4, kcpc::kControlAcquire, 0, reply);
  EXPECT_EQ(reply.role, kcpc::Role::kController);
  EXPECT_EQ(reply.controller, b_hello.client_id);
  EXPECT_EQ(mem_write_status(*b, 5), Status::kOk);
}

// User decision 2026-10-11: takeover is a force flag and the newest wins.
TEST_F(KcpcControlTest, ForcedTakeoverDemotesThePreviousController) {
  start_writable();
  kcpc::HelloReply a_hello;
  kcpc::HelloReply b_hello;
  auto a = join(kcpc::Role::kController, a_hello);
  auto b = join(kcpc::Role::kObserver, b_hello);
  ASSERT_EQ(mem_write_status(*a, 2), Status::kOk);

  kcpc::ControlReply reply;
  control(*b, 2, kcpc::kControlAcquire, kcpc::kControlFlagForce, reply);
  EXPECT_EQ(reply.role, kcpc::Role::kController);
  EXPECT_EQ(reply.controller, b_hello.client_id);
  EXPECT_EQ(server_.controller(), b_hello.client_id);

  // The first is still connected, as an observer: its writes are refused and
  // change nothing, its reads work.
  int const writes_before = writes_.mem_writes.load();
  EXPECT_EQ(mem_write_status(*a, 3), Status::kNotController);
  EXPECT_EQ(writes_.mem_writes.load(), writes_before);
  expect_ping_ok(*a, 4);
  EXPECT_EQ(mem_write_status(*b, 3), Status::kOk);

  // No client is special: the first may force it back.
  control(*a, 5, kcpc::kControlAcquire, kcpc::kControlFlagForce, reply);
  EXPECT_EQ(reply.role, kcpc::Role::kController);
  EXPECT_EQ(reply.controller, a_hello.client_id);
  EXPECT_EQ(mem_write_status(*b, 4), Status::kNotController);
  EXPECT_EQ(mem_write_status(*a, 6), Status::kOk);
}

TEST_F(KcpcControlTest, ControlIsFreeOnceTheControllerDisconnects) {
  start_writable();
  kcpc::HelloReply a_hello;
  kcpc::HelloReply b_hello;
  auto a = join(kcpc::Role::kController, a_hello);
  auto b = join(kcpc::Role::kObserver, b_hello);
  ASSERT_EQ(server_.controller(), a_hello.client_id);

  a->close();
  auto const deadline = Clock::now() + std::chrono::seconds(5);
  while (server_.controller() != 0 && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(server_.controller(), 0U) << "a closed connection kept control";

  kcpc::ControlReply reply;
  control(*b, 2, kcpc::kControlAcquire, 0, reply);
  EXPECT_EQ(reply.role, kcpc::Role::kController);
  EXPECT_EQ(mem_write_status(*b, 3), Status::kOk);
}

TEST_F(KcpcControlTest, ASilentControllerIsDroppedAndControlBecomesFree) {
  // Twelve pings fit in one timeout, so a loaded host that oversleeps one of
  // them several times over still does not look like silence.
  constexpr int kIdleMs = 600;
  constexpr int kPingEveryMs = 50;
  start_writable(kIdleMs);
  kcpc::HelloReply a_hello;
  kcpc::HelloReply b_hello;
  auto a = join(kcpc::Role::kController, a_hello);
  auto b = join(kcpc::Role::kObserver, b_hello);

  // PING keeps it: twice the timeout passes and it still has control.
  auto const begin = Clock::now();
  auto last_ping = begin;
  std::uint32_t id = 2;
  while (Clock::now() - begin < std::chrono::milliseconds(2 * kIdleMs)) {
    expect_ping_ok(*a, id++);
    last_ping = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(kPingEveryMs));
  }
  ASSERT_EQ(server_.controller(), a_hello.client_id);

  // Silence: one ERROR saying why, then the connection is closed. Not before
  // the timeout has passed since its last frame.
  expect_error_then_close(*a, Status::kIdleTimeout);
  EXPECT_GE(Clock::now() - last_ping, std::chrono::milliseconds(kIdleMs - 20));
  EXPECT_EQ(server_.controller(), 0U);

  // The observer was silent all along and is still there: observers are
  // never timed out. It takes the free control.
  kcpc::ControlReply reply;
  control(*b, 2, kcpc::kControlAcquire, 0, reply);
  EXPECT_EQ(reply.role, kcpc::Role::kController);
}

TEST_F(KcpcControlTest, AnObserverThatTakesControlIsTimedFromThatMoment) {
  constexpr int kIdleMs = 600;
  start_writable(kIdleMs);
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kObserver, hello);
  // Longer than the timeout as an observer...
  std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs + 200));
  kcpc::ControlReply reply;
  control(*client, 2, kcpc::kControlAcquire, 0, reply);
  ASSERT_EQ(reply.role, kcpc::Role::kController);
  // ...does not count against the controller it has just become.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  expect_ping_ok(*client, 3);
  EXPECT_EQ(server_.controller(), hello.client_id);
  // Giving control up ends the clock.
  control(*client, 4, kcpc::kControlRelease, 0, reply);
  std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs + 200));
  expect_ping_ok(*client, 5);
}

TEST_F(KcpcControlTest, BadControlRequestsAreBadPayloads) {
  start_writable();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kObserver, hello);
  Frame frame;
  exchange(*client, Opcode::kControl, 2, {1, 0}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "cut short";
  exchange(*client, Opcode::kControl, 3, control_payload(2), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "unknown action";
  EXPECT_EQ(server_.controller(), 0U);
  expect_ping_ok(*client, 4);
}

TEST_F(KcpcControlTest, RegsSetPassesTheMaskAndRepliesWithTheBlock) {
  start_writable();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kController, hello);

  kcpc::Regs values;
  values.hl = 0xBEEF;
  values.pc = 0x1234;
  Frame frame;
  exchange(*client, Opcode::kRegsSet, 2, regs_set_payload(1U << 3, values),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U + 12U + kcpc::kRegsSize);
  EXPECT_EQ(writes_.last_mask, 1U << 3);
  EXPECT_EQ(writes_.last_regs.hl, 0xBEEF);
  EXPECT_EQ(writes_.last_regs.pc, 0x1234);

  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::Stamp stamp;
  kcpc::Regs after;
  ASSERT_TRUE(kcpc::read_stamp(r, stamp));
  ASSERT_TRUE(kcpc::read_regs(r, after));
  EXPECT_EQ(stamp.frame, 90U);
  EXPECT_TRUE(stamp.paused);
  EXPECT_EQ(after.hl, 0xBEEF);
  EXPECT_EQ(after.bc, FakeReads::regs().bc);
}

TEST_F(KcpcControlTest, RegsSetRefusesWhatCannotBeSet) {
  start_writable();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kController, hello);
  kcpc::Regs const good;
  kcpc::Regs bad_im;
  bad_im.im = 3;
  kcpc::Regs bad_iff;
  bad_iff.iff2 = 2;
  struct Case {
    const char* what;
    std::vector<std::uint8_t> payload;
  };
  std::vector<std::uint8_t> short_payload = regs_set_payload(1, good);
  short_payload.pop_back();
  const Case cases[] = {
      {"cut short", short_payload},
      {"HALT", regs_set_payload(kcpc::kRegBitHalt, good)},
      {"a field past the block", regs_set_payload(1U << 18, good)},
      {"IM 3", regs_set_payload(kcpc::kRegBitIm, bad_im)},
      {"IFF2 2", regs_set_payload(kcpc::kRegBitIff2, bad_iff)},
  };
  std::uint32_t id = 2;
  for (const Case& c : cases) {
    Frame frame;
    exchange(*client, Opcode::kRegsSet, id++, c.payload, frame);
    EXPECT_EQ(frame.status(), Status::kBadPayload) << c.what;
  }
  EXPECT_EQ(writes_.regs_sets.load(), 0) << "a refused write reached the core";

  // A value the mask does not select is not looked at.
  Frame frame;
  exchange(*client, Opcode::kRegsSet, id++, regs_set_payload(1, bad_im), frame);
  EXPECT_EQ(frame.status(), Status::kOk);
  // An empty mask is a valid way to ask for the block.
  exchange(*client, Opcode::kRegsSet, id++, regs_set_payload(0, good), frame);
  EXPECT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(writes_.last_mask, 0U);
}

TEST_F(KcpcControlTest, MemWritePassesTheBytesThrough) {
  start_writable();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kController, hello);

  std::vector<std::uint8_t> const data = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x7F};
  Frame frame;
  exchange(*client, Opcode::kMemWrite, 2,
           mem_write_payload(kcpc::MemSpace::kPhysRam, 0x8123, data), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U + 12U);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::Stamp stamp;
  ASSERT_TRUE(kcpc::read_stamp(r, stamp));
  EXPECT_EQ(stamp.frame, 91U);
  EXPECT_EQ(writes_.last_write.space,
            static_cast<std::uint8_t>(kcpc::MemSpace::kPhysRam));
  EXPECT_EQ(writes_.last_write.addr, 0x8123U);
  EXPECT_EQ(writes_.written, data);

  // No bytes at all is a write of nothing, and still a write.
  exchange(*client, Opcode::kMemWrite, 3,
           mem_write_payload(kcpc::MemSpace::kCpuRam, 0x10000, {}), frame);
  EXPECT_EQ(frame.status(), Status::kOk);
  EXPECT_TRUE(writes_.written.empty());
}

TEST_F(KcpcControlTest, MemWriteRefusesTheSpacesThatAreNotRam) {
  start_writable();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kController, hello);
  std::uint32_t id = 2;
  Frame frame;
  for (std::uint8_t space : {0, 3, 4, 5, 6, 255}) {
    exchange(*client, Opcode::kMemWrite, id++,
             mem_write_payload(static_cast<kcpc::MemSpace>(space), 0, {1}),
             frame);
    EXPECT_EQ(frame.status(), Status::kBadPayload) << "space " << +space;
  }
  exchange(*client, Opcode::kMemWrite, id++,
           mem_write_payload(kcpc::MemSpace::kCpuRam, 0, {1}, 1), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "a bank where none applies";
  exchange(*client, Opcode::kMemWrite, id++, {1, 0, 0, 0, 0, 0, 0}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "cut short";
  EXPECT_EQ(writes_.mem_writes.load(), 0) << "a refused write reached the core";

  // The range is the machine's to judge.
  exchange(*client, Opcode::kMemWrite, id++,
           mem_write_payload(kcpc::MemSpace::kCpuRam, 0xFFFF, {1, 2}), frame);
  EXPECT_EQ(frame.status(), Status::kOutOfRange);
}

TEST_F(KcpcControlTest, ABusyMachineRefusesTheWriteAndKeepsTheController) {
  start_writable();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kController, hello);
  writes_.refuse = Status::kBusy;
  Frame frame;
  exchange(*client, Opcode::kRegsSet, 2, regs_set_payload(1, FakeReads::regs()),
           frame);
  EXPECT_EQ(frame.status(), Status::kBusy);
  EXPECT_EQ(mem_write_status(*client, 3), Status::kBusy);
  writes_.refuse = Status::kOk;
  EXPECT_EQ(mem_write_status(*client, 4), Status::kOk);
  EXPECT_EQ(server_.controller(), hello.client_id);
}

TEST_F(KcpcControlTest, StopLeavesNoController) {
  start_writable();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kController, hello);
  ASSERT_NE(server_.controller(), 0U);
  server_.stop();
  EXPECT_EQ(server_.controller(), 0U);
}

// -- slice 4: run control and pushed events ---------------------------------

namespace {

std::vector<std::uint8_t> subscribe_payload(std::uint32_t classes,
                                            std::uint32_t interval = 0) {
  kcpc::Writer w;
  kcpc::write_subscription(w, {classes, interval});
  return w.take();
}

std::vector<std::uint8_t> step_payload(std::uint8_t mode, std::uint32_t count) {
  kcpc::Writer w;
  kcpc::StepRequest step;
  step.mode = mode;
  step.count = count;
  kcpc::write_step_request(w, step);
  return w.take();
}

kcpc::Event frame_event(std::uint64_t frame) {
  kcpc::Event e;
  e.opcode = Opcode::kEvtFrame;
  e.frame = frame;
  return e;
}

kcpc::Event stopped_event(std::uint64_t frame) {
  kcpc::Event e;
  e.opcode = Opcode::kEvtStopped;
  e.frame = frame;
  e.reason = static_cast<std::uint8_t>(kcpc::StopReason::kPaused);
  return e;
}

// The first eight bytes of the frame-carrying events: `frame`.
std::uint64_t event_frame(const Frame& frame) {
  kcpc::Reader r(frame.payload);
  std::uint64_t value = 0;
  r.u64(value);
  return value;
}

class KcpcEventsTest : public KcpcControlTest {
 protected:
  void start_with_run_control() {
    kcpc::ServerConfig config = test_config();
    reads_.attach(config);
    writes_.attach(config);
    run_.attach(config, server_);
    config.max_event_queue_bytes = event_cap_;
    start(config);
  }

  static void subscribe(TestClient& client, std::uint32_t id,
                        std::uint32_t classes, std::uint32_t interval = 0) {
    Frame frame;
    exchange(client, Opcode::kSubscribe, id,
             subscribe_payload(classes, interval), frame);
    ASSERT_EQ(frame.status(), Status::kOk);
  }

  // Every event posted before this returns has been delivered to `client`
  // or filtered out: events and responses share one queue per connection,
  // and the fan-out thread takes events in the order they were posted.
  void settle(TestClient& client, std::uint32_t id) {
    // A marker that ignores every mask. It reaches the other clients too;
    // their own settle() throws away markers that are not theirs.
    constexpr std::uint32_t kMarker = 0xABCD0000U;
    kcpc::Event marker;
    marker.opcode = Opcode::kEvtOverrun;
    marker.dropped = kMarker + id;
    server_.post(marker);
    for (;;) {
      Frame frame;
      ASSERT_TRUE(client.receive_any(frame)) << "the marker never arrived";
      ASSERT_EQ(frame.header.kind,
                static_cast<std::uint8_t>(kcpc::Kind::kEvent));
      if (frame.header.opcode ==
          static_cast<std::uint16_t>(Opcode::kEvtOverrun)) {
        kcpc::Reader r(frame.payload);
        std::uint32_t dropped = 0;
        r.u32(dropped);
        if (dropped == marker.dropped) return;
        if ((dropped & 0xFFFF0000U) == kMarker) continue;
      }
      client.events.push_back(frame);
    }
  }

  static void expect_event(TestClient& client, Opcode opcode, Frame& frame) {
    ASSERT_TRUE(client.next_event(frame)) << "no event arrived";
    ASSERT_EQ(frame.header.opcode, static_cast<std::uint16_t>(opcode));
  }

  FakeRun run_;
  std::size_t event_cap_ = 4U << 20;
};

}  // namespace

TEST_F(KcpcEventsTest, HelloAnnouncesRunControlAndEvents) {
  start_with_run_control();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kObserver, hello);
  EXPECT_EQ(hello.caps, kcpc::kServerCaps & ~kcpc::kCapBreakpoints &
                            ~kcpc::kCapInput & ~kcpc::kCapGun & ~kDevtoolsCaps &
                            ~kMediaCaps);
  EXPECT_NE(hello.caps & kcpc::kCapRunControl, 0U);
  EXPECT_NE(hello.caps & kcpc::kCapEvents, 0U);
}

TEST_F(KcpcEventsTest, WithoutRunControlItsOpcodesDoNotExist) {
  kcpc::ServerConfig config = test_config();
  start(config);
  TestClient client(server_.port());
  kcpc::HelloReply hello;
  expect_hello_ok(client, 1, hello);
  EXPECT_EQ(hello.caps & kcpc::kCapRunControl, 0U);
  Frame frame;
  for (Opcode const opcode : {Opcode::kPause, Opcode::kRun, Opcode::kStep,
                              Opcode::kReset, Opcode::kRunState}) {
    ASSERT_TRUE(client.request(opcode, 2, std::vector<std::uint8_t>(16)));
    ASSERT_TRUE(client.receive(frame));
    EXPECT_EQ(frame.status(), Status::kUnknownOpcode);
  }
  // Events are the server's own: SUBSCRIBE needs no machine.
  ASSERT_TRUE(client.request(Opcode::kSubscribe, 3,
                             subscribe_payload(kcpc::kEventClassRun)));
  ASSERT_TRUE(client.receive(frame));
  EXPECT_EQ(frame.status(), Status::kOk);
}

TEST_F(KcpcEventsTest, AClientReceivesTheClassesItSubscribedTo) {
  start_with_run_control();
  kcpc::HelloReply hello;
  auto watcher = join(kcpc::Role::kObserver, hello);
  auto deaf = join(kcpc::Role::kObserver, hello);

  // The reply repeats what was accepted; unknown class bits are dropped.
  Frame frame;
  exchange(*watcher, Opcode::kSubscribe, 2,
           subscribe_payload(kcpc::kEventClassRun | 0x80000000U, 9), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), 4U + 8U);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::Subscription accepted;
  ASSERT_TRUE(kcpc::read_subscription(r, accepted));
  EXPECT_EQ(accepted.classes, kcpc::kEventClassRun);
  EXPECT_EQ(accepted.frame_interval, 9U);

  kcpc::Event stopped = stopped_event(300);
  stopped.pc = 0xBEEF;
  stopped.cause_client = 42;
  stopped.cause_request = 43;
  server_.post(stopped);
  server_.post(frame_event(301));  // nobody asked for frames
  kcpc::Event resumed;
  resumed.opcode = Opcode::kEvtResumed;
  resumed.frame = 302;
  server_.post(resumed);

  expect_event(*watcher, Opcode::kEvtStopped, frame);
  EXPECT_EQ(frame.header.id, 1U) << "the first event on a connection";
  ASSERT_EQ(frame.payload.size(), 24U);
  EXPECT_EQ(event_frame(frame), 300U);
  kcpc::Reader e(frame.payload);
  std::uint8_t reason = 0;
  std::uint16_t pc = 0;
  std::uint32_t client = 0, request = 0;
  e.skip(8);
  e.u8(reason);
  e.skip(1);
  e.u16(pc);
  e.skip(4);
  e.u32(client);
  ASSERT_TRUE(e.u32(request));
  EXPECT_EQ(reason, 1);
  EXPECT_EQ(pc, 0xBEEF);
  EXPECT_EQ(client, 42U);
  EXPECT_EQ(request, 43U);
  expect_event(*watcher, Opcode::kEvtResumed, frame);
  EXPECT_EQ(frame.header.id, 2U) << "the frame event was not for this client";

  settle(*deaf, 9);
  EXPECT_TRUE(deaf->events.empty())
      << "a client that subscribed to nothing was sent an event";

  // SUBSCRIBE replaces the subscription.
  subscribe(*watcher, 3, 0);
  watcher->events.clear();  // the other client's marker, read on the way
  server_.post(stopped_event(303));
  settle(*watcher, 4);
  EXPECT_TRUE(watcher->events.empty());
}

TEST_F(KcpcEventsTest, FrameEventsFollowEachClientsInterval) {
  start_with_run_control();
  EXPECT_FALSE(server_.frame_due(5)) << "nobody wants frames yet";
  kcpc::HelloReply hello;
  auto fives = join(kcpc::Role::kObserver, hello);
  auto threes = join(kcpc::Role::kObserver, hello);
  subscribe(*fives, 2, kcpc::kEventClassFrame, 5);
  EXPECT_TRUE(server_.frame_due(10));
  EXPECT_FALSE(server_.frame_due(9)) << "the runner posts every fifth frame";
  subscribe(*threes, 2, kcpc::kEventClassFrame, 3);
  EXPECT_TRUE(server_.frame_due(9)) << "3 and 5 together need every frame";
  EXPECT_TRUE(server_.frame_due(7));

  for (std::uint64_t f = 1; f <= 15; ++f) server_.post(frame_event(f));
  Frame frame;
  std::uint32_t id = 1;
  for (std::uint64_t const expected : {5, 10, 15}) {
    expect_event(*fives, Opcode::kEvtFrame, frame);
    EXPECT_EQ(event_frame(frame), expected);
    EXPECT_EQ(frame.header.id, id++) << "event ids count per connection";
    EXPECT_EQ(frame.payload.size(), 16U);
  }
  id = 1;
  for (std::uint64_t const expected : {3, 6, 9, 12, 15}) {
    expect_event(*threes, Opcode::kEvtFrame, frame);
    EXPECT_EQ(event_frame(frame), expected);
    EXPECT_EQ(frame.header.id, id++);
  }

  // An interval without the class, and the class with interval 0, want
  // nothing.
  subscribe(*threes, 3, kcpc::kEventClassRun, 3);
  EXPECT_FALSE(server_.frame_due(9));
  EXPECT_TRUE(server_.frame_due(10));
  subscribe(*fives, 3, kcpc::kEventClassFrame, 0);
  EXPECT_FALSE(server_.frame_due(10));

  // A client that leaves takes its interval with it.
  subscribe(*fives, 4, kcpc::kEventClassFrame, 2);
  EXPECT_TRUE(server_.frame_due(4));
  fives->close();
  auto const deadline = Clock::now() + std::chrono::seconds(5);
  while (server_.frame_due(4) && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_FALSE(server_.frame_due(4));
}

// The machine settles a PAUSE, and posts its EVT_STOPPED, before the request
// returns. The client that asked still sees its response first.
TEST_F(KcpcEventsTest, AnEventFollowsTheResponseToTheRequestThatCausedIt) {
  start_with_run_control();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  // Before anyone else joins: it is sent to every client connected by then.
  Frame frame;
  expect_event(*controller, Opcode::kEvtControlChanged, frame);
  auto watcher = join(kcpc::Role::kObserver, hello);
  subscribe(*controller, 2, kcpc::kEventClassRun);
  subscribe(*watcher, 2, kcpc::kEventClassRun);

  run_.post_from_request = true;
  ASSERT_TRUE(controller->request(Opcode::kPause, 3));
  // The watcher has the event while the request is still being served.
  expect_event(*watcher, Opcode::kEvtStopped, frame);
  // An unrelated event that happens meanwhile keeps its place behind it.
  server_.post(stopped_event(999));

  ASSERT_TRUE(controller->receive_any(frame));
  EXPECT_EQ(frame.header.kind, static_cast<std::uint8_t>(kcpc::Kind::kResponse))
      << "the event overtook the response";
  EXPECT_EQ(frame.header.opcode, static_cast<std::uint16_t>(Opcode::kPause));
  EXPECT_EQ(frame.header.id, 3U);
  ASSERT_EQ(frame.status(), Status::kOk);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::Stamp stamp;
  ASSERT_TRUE(kcpc::read_stamp(r, stamp));
  EXPECT_EQ(stamp.frame, 56U);
  EXPECT_TRUE(stamp.paused);

  expect_event(*controller, Opcode::kEvtStopped, frame);
  EXPECT_EQ(event_frame(frame), 56U);
  expect_event(*controller, Opcode::kEvtStopped, frame);
  EXPECT_EQ(event_frame(frame), 999U) << "events arrive in the order posted";
  EXPECT_EQ(run_.last_cause.client, hello.client_id - 1)
      << "the machine is told which client asked";
  EXPECT_EQ(run_.last_cause.request, 3U);
}

TEST_F(KcpcEventsTest, RunControlNeedsControlAndRunStateDoesNot) {
  start_with_run_control();
  kcpc::HelloReply controller_hello;
  kcpc::HelloReply observer_hello;
  auto controller = join(kcpc::Role::kController, controller_hello);
  auto observer = join(kcpc::Role::kObserver, observer_hello);

  Frame frame;
  std::uint32_t id = 2;
  for (Opcode const opcode :
       {Opcode::kPause, Opcode::kRun, Opcode::kReset, Opcode::kStep}) {
    // Refused before the payload is looked at: these are all malformed.
    exchange(*observer, opcode, id++, {}, frame);
    EXPECT_EQ(frame.status(), Status::kNotController);
  }
  EXPECT_EQ(run_.pauses + run_.runs + run_.resets + run_.steps, 0);

  run_.paused = true;
  run_.walking = true;
  exchange(*observer, Opcode::kRunState, id++, {}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), 4U + 12U + 8U);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::RunState state;
  ASSERT_TRUE(kcpc::read_run_state(r, state));
  EXPECT_EQ(state.stamp.frame, 55U);
  EXPECT_TRUE(state.stamp.paused);
  EXPECT_TRUE(state.walk_active);
  EXPECT_EQ(state.controller, controller_hello.client_id);

  exchange(*controller, Opcode::kRun, 2, {}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U + 12U);
  EXPECT_EQ(run_.runs, 1);
  EXPECT_EQ(run_.last_cause.client, controller_hello.client_id);
  EXPECT_EQ(run_.last_cause.request, 2U);

  run_.refuse = Status::kBusy;
  exchange(*controller, Opcode::kPause, 3, {}, frame);
  EXPECT_EQ(frame.status(), Status::kBusy);
}

TEST_F(KcpcEventsTest, ResetPassesTheStayPausedFlag) {
  start_with_run_control();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  exchange(*controller, Opcode::kReset, 2, {1, 0, 0, 0}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_TRUE(run_.last_stay_paused);
  exchange(*controller, Opcode::kReset, 3, {0, 0, 0, 0}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_FALSE(run_.last_stay_paused);
  exchange(*controller, Opcode::kReset, 4, {1}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "three reserved bytes";
  EXPECT_EQ(run_.resets, 2);
}

TEST_F(KcpcEventsTest, StepIsCheckedBeforeTheMachineIsAsked) {
  start_with_run_control();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  auto const in = static_cast<std::uint8_t>(kcpc::StepMode::kIn);
  auto const frames = static_cast<std::uint8_t>(kcpc::StepMode::kFrames);

  exchange(*controller, Opcode::kStep, 2, {0, 0, 0}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "too short";
  exchange(*controller, Opcode::kStep, 3, step_payload(5, 1), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "no such mode";
  for (std::uint8_t mode = 1; mode <= 3; ++mode) {
    exchange(*controller, Opcode::kStep, 4, step_payload(mode, 1), frame);
    EXPECT_EQ(frame.status(), Status::kBadPayload)
        << "over, out and to arrive with the breakpoints";
  }
  exchange(*controller, Opcode::kStep, 5,
           step_payload(in, kcpc::kMaxStepInCount + 1), frame);
  EXPECT_EQ(frame.status(), Status::kOutOfRange);
  exchange(*controller, Opcode::kStep, 6,
           step_payload(frames, kcpc::kMaxStepFrames + 1), frame);
  EXPECT_EQ(frame.status(), Status::kOutOfRange);
  EXPECT_EQ(run_.steps, 0) << "a refused step never reached the machine";

  exchange(*controller, Opcode::kStep, 7, step_payload(in, 0), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U) << "the reply is the status alone";
  EXPECT_EQ(run_.last_step.mode, in);
  EXPECT_EQ(run_.last_step.count, 1U) << "count 0 means 1";
  EXPECT_EQ(run_.last_cause.request, 7U);

  exchange(*controller, Opcode::kStep, 8,
           step_payload(frames, kcpc::kMaxStepInCount + 1), frame);
  ASSERT_EQ(frame.status(), Status::kOk) << "the limit is step in's";
  EXPECT_EQ(run_.last_step.count, kcpc::kMaxStepInCount + 1);

  run_.refuse = Status::kBusy;  // a step is already in progress
  exchange(*controller, Opcode::kStep, 9, step_payload(in, 1), frame);
  EXPECT_EQ(frame.status(), Status::kBusy);
}

TEST_F(KcpcEventsTest, EveryClientIsToldWhoHasControl) {
  start_with_run_control();
  kcpc::HelloReply first_hello;
  kcpc::HelloReply second_hello;
  auto first = join(kcpc::Role::kController, first_hello);
  auto second = join(kcpc::Role::kObserver, second_hello);
  std::uint32_t const a = first_hello.client_id;
  std::uint32_t const b = second_hello.client_id;

  auto const expect_change = [](TestClient& client, std::uint32_t controller,
                                std::uint32_t previous, std::uint8_t reason) {
    Frame frame;
    ASSERT_TRUE(client.next_event(frame));
    ASSERT_EQ(frame.header.opcode,
              static_cast<std::uint16_t>(Opcode::kEvtControlChanged));
    ASSERT_EQ(frame.payload.size(), 12U);
    kcpc::Reader r(frame.payload);
    std::uint32_t got_controller = 0, got_previous = 0;
    std::uint8_t got_reason = 0;
    r.u32(got_controller);
    r.u32(got_previous);
    ASSERT_TRUE(r.u8(got_reason));
    EXPECT_EQ(got_controller, controller);
    EXPECT_EQ(got_previous, previous);
    EXPECT_EQ(got_reason, reason);
  };

  // Granted by HELLO: the new controller is told, after its HELLO reply and
  // though it subscribed to nothing.
  expect_change(*first, a, 0, kcpc::kControlAcquired);

  // Taken by force: the demoted controller learns it from the event.
  ASSERT_TRUE(second->request(Opcode::kControl, 2, control_payload(1, 1)));
  Frame frame;
  ASSERT_TRUE(second->receive_any(frame));
  EXPECT_EQ(frame.header.kind, static_cast<std::uint8_t>(kcpc::Kind::kResponse))
      << "the response to CONTROL comes before its event";
  expect_change(*second, b, a, kcpc::kControlTaken);
  expect_change(*first, b, a, kcpc::kControlTaken);

  kcpc::ControlReply reply;
  control(*second, 3, kcpc::kControlRelease, 0, reply);
  expect_change(*second, 0, b, kcpc::kControlReleased);
  expect_change(*first, 0, b, kcpc::kControlReleased);

  control(*first, 2, kcpc::kControlAcquire, 0, reply);
  expect_change(*first, a, 0, kcpc::kControlAcquired);
  expect_change(*second, a, 0, kcpc::kControlAcquired);

  // Asking again, or releasing what one does not hold, changes nothing and
  // announces nothing.
  control(*first, 3, kcpc::kControlAcquire, 0, reply);
  control(*second, 4, kcpc::kControlRelease, 0, reply);
  control(*second, 5, kcpc::kControlAcquire, 0, reply);
  settle(*second, 6);
  EXPECT_TRUE(second->events.empty());

  first->close();
  expect_change(*second, 0, a, kcpc::kControlLeft);
}

// "An EVT_STOPPED that ends a step is always sent to the client that asked
// for the step."
TEST_F(KcpcEventsTest, TheEndOfAStepReachesItsClientWhateverItSubscribedTo) {
  start_with_run_control();
  kcpc::HelloReply stepper_hello;
  kcpc::HelloReply other_hello;
  auto stepper = join(kcpc::Role::kController, stepper_hello);
  // Before anyone else joins: it is sent to every client connected by then.
  Frame frame;
  expect_event(*stepper, Opcode::kEvtControlChanged, frame);
  auto other = join(kcpc::Role::kObserver, other_hello);

  kcpc::Event done = stopped_event(400);
  done.reason = static_cast<std::uint8_t>(kcpc::StopReason::kStepDone);
  done.cause_client = stepper_hello.client_id;
  done.cause_request = 77;
  done.step_client = stepper_hello.client_id;
  server_.post(done);

  expect_event(*stepper, Opcode::kEvtStopped, frame);
  EXPECT_EQ(event_frame(frame), 400U);
  settle(*other, 2);
  EXPECT_TRUE(other->events.empty());
}

// A client that does not read costs nobody else anything: past the event
// cap its events are dropped, and it is told how many once there is room.
TEST_F(KcpcEventsTest, AClientThatStopsReadingIsToldHowManyEventsItMissed) {
  event_cap_ = 64U << 10;
  start_with_run_control();
  kcpc::HelloReply hello;
  auto slow = join(kcpc::Role::kObserver, hello);
  auto steady = join(kcpc::Role::kObserver, hello);
  subscribe(*slow, 2, kcpc::kEventClassFrame, 1);
  subscribe(*steady, 2, kcpc::kEventClassFrame, 1);

  server_.post(frame_event(1));
  Frame frame;
  expect_event(*slow, Opcode::kEvtFrame, frame);
  EXPECT_EQ(frame.header.id, 1U);

  // 23 MiB of replies nobody reads: far more than the socket buffers hold,
  // so most of it waits in the server's queue for this client.
  constexpr int kReads = 360;
  kcpc::Writer w;
  kcpc::MemReadRequest all;
  all.len = FakeReads::kSize;
  kcpc::write_mem_read_request(w, all);
  std::vector<std::uint8_t> const read_all = w.take();
  for (int i = 0; i < kReads; ++i) {
    ASSERT_TRUE(slow->request(Opcode::kMemRead, 100 + i, read_all));
  }
  auto const deadline = Clock::now() + std::chrono::seconds(10);
  while (reads_.mem_reads < kReads && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(reads_.mem_reads, kReads);

  constexpr std::uint64_t kMissed = 10;
  for (std::uint64_t f = 2; f < 2 + kMissed; ++f) server_.post(frame_event(f));
  // The other client has every one of them, at once.
  for (std::uint64_t f = 1; f < 2 + kMissed; ++f) {
    expect_event(*steady, Opcode::kEvtFrame, frame);
    EXPECT_EQ(event_frame(frame), f);
    EXPECT_EQ(frame.header.id, f);
  }

  // The slow client reads again: every reply is there, in order, and then
  // one EVT_OVERRUN that says exactly what was dropped.
  for (int i = 0; i < kReads; ++i) {
    ASSERT_TRUE(slow->receive(frame)) << "reply " << i;
    ASSERT_EQ(frame.header.id, 100U + i);
    ASSERT_EQ(frame.status(), Status::kOk);
  }
  expect_event(*slow, Opcode::kEvtOverrun, frame);
  EXPECT_EQ(frame.header.id, 2U) << "an OVERRUN is an event like any other";
  kcpc::Reader r(frame.payload);
  std::uint32_t dropped = 0;
  ASSERT_TRUE(r.u32(dropped));
  EXPECT_EQ(dropped, kMissed);

  server_.post(frame_event(50));
  expect_event(*slow, Opcode::kEvtFrame, frame);
  EXPECT_EQ(event_frame(frame), 50U);
  EXPECT_EQ(frame.header.id, 3U);
  EXPECT_TRUE(slow->events.empty());
}

TEST_F(KcpcEventsTest, StopSendsShutdownThenTheError) {
  start_with_run_control();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kObserver, hello);
  TestClient silent(server_.port());
  ASSERT_TRUE(silent.connected());
  server_.post(stopped_event(1));  // not subscribed: must not count

  server_.stop();

  Frame frame;
  ASSERT_TRUE(client->receive_any(frame));
  EXPECT_EQ(frame.header.kind, static_cast<std::uint8_t>(kcpc::Kind::kEvent));
  EXPECT_EQ(frame.header.opcode,
            static_cast<std::uint16_t>(Opcode::kEvtShutdown));
  EXPECT_EQ(frame.header.id, 1U);
  EXPECT_TRUE(frame.payload.empty());
  expect_error_then_close(*client, Status::kShuttingDown);

  // A connection that never completed HELLO was promised no events.
  ASSERT_TRUE(silent.receive_any(frame));
  EXPECT_EQ(frame.header.kind,
            static_cast<std::uint8_t>(kcpc::Kind::kResponse));
  EXPECT_EQ(frame.header.opcode, static_cast<std::uint16_t>(Opcode::kError));
  EXPECT_EQ(frame.status(), Status::kShuttingDown);

  // Posting to a stopped server is harmless, and nothing of it is left for
  // the clients of the next start.
  for (int i = 0; i < 10; ++i) server_.post(stopped_event(2));
  start_with_run_control();
  auto later = join(kcpc::Role::kObserver, hello);
  subscribe(*later, 2, kcpc::kEventClassRun);
  server_.post(stopped_event(3));
  expect_event(*later, Opcode::kEvtStopped, frame);
  EXPECT_EQ(event_frame(frame), 3U);
  EXPECT_EQ(frame.header.id, 1U);
}

// -- slice 5: breakpoints and the steps that walk ---------------------------

namespace {

std::vector<std::uint8_t> bp_payload(const kcpc::BpEntry& entry) {
  kcpc::Writer w;
  kcpc::write_bp_entry(w, entry);
  return w.take();
}

kcpc::BpEntry exec_bp(std::uint16_t addr) {
  kcpc::BpEntry entry;
  entry.addr = addr;
  return entry;
}

kcpc::BpEntry watch_bp(std::uint16_t addr, std::uint16_t len,
                       std::uint8_t access) {
  kcpc::BpEntry entry;
  entry.kind = static_cast<std::uint8_t>(kcpc::BpKind::kWatch);
  entry.addr = addr;
  entry.len_or_mask = len;
  entry.access = access;
  return entry;
}

std::vector<std::uint8_t> bp_del_payload(std::uint8_t kind, std::uint16_t addr,
                                         std::uint16_t mask = 0) {
  kcpc::Writer w;
  kcpc::BpDelRequest del;
  del.kind = kind;
  del.addr = addr;
  del.len_or_mask = mask;
  kcpc::write_bp_del_request(w, del);
  return w.take();
}

// The u64 a successful edit replies with.
std::uint64_t reply_generation(const Frame& frame) {
  kcpc::Reader r(frame.payload);
  std::uint64_t generation = 0;
  r.skip(4);
  r.u64(generation);
  return generation;
}

class KcpcBreakpointsTest : public KcpcEventsTest {
 protected:
  void start_with_breakpoints() {
    kcpc::ServerConfig config = test_config();
    reads_.attach(config);
    writes_.attach(config);
    run_.attach(config, server_);
    bps_.attach(config, server_);
    start(config);
  }

  FakeBreakpoints bps_;
};

}  // namespace

TEST_F(KcpcBreakpointsTest, HelloAnnouncesBreakpoints) {
  start_with_breakpoints();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kObserver, hello);
  EXPECT_EQ(hello.caps, kcpc::kServerCaps & ~kcpc::kCapInput & ~kcpc::kCapGun &
                            ~kDevtoolsCaps & ~kMediaCaps);
  EXPECT_NE(hello.caps & kcpc::kCapBreakpoints, 0U);
}

TEST_F(KcpcBreakpointsTest, WithoutTheGroupItsOpcodesDoNotExist) {
  start_with_run_control();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  EXPECT_EQ(hello.caps & kcpc::kCapBreakpoints, 0U);
  Frame frame;
  std::uint32_t id = 2;
  for (Opcode const opcode :
       {Opcode::kBpList, Opcode::kBpAdd, Opcode::kBpDel, Opcode::kBpClear}) {
    exchange(*controller, opcode, id++, bp_payload(exec_bp(0x4000)), frame);
    EXPECT_EQ(frame.status(), Status::kUnknownOpcode);
  }
}

TEST_F(KcpcBreakpointsTest, AnyoneListsAndOnlyTheControllerEdits) {
  start_with_breakpoints();
  kcpc::HelloReply controller_hello;
  kcpc::HelloReply observer_hello;
  auto controller = join(kcpc::Role::kController, controller_hello);
  auto observer = join(kcpc::Role::kObserver, observer_hello);

  Frame frame;
  std::uint32_t id = 2;
  for (Opcode const opcode :
       {Opcode::kBpAdd, Opcode::kBpDel, Opcode::kBpClear}) {
    // Refused before the payload is looked at: these are all malformed.
    exchange(*observer, opcode, id++, {}, frame);
    EXPECT_EQ(frame.status(), Status::kNotController);
  }
  EXPECT_EQ(bps_.adds + bps_.dels + bps_.clears, 0);

  kcpc::BpEntry conditional = exec_bp(0xC000);
  conditional.pass_count = 3;
  conditional.hit_count = 2;
  conditional.condition = "a = 5";
  bps_.entries = {conditional, watch_bp(0x9000, 16, kcpc::kBpAccessBoth)};
  exchange(*observer, Opcode::kBpList, id++, {}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  std::uint64_t generation = 0;
  std::uint32_t count = 0;
  r.u64(generation);
  ASSERT_TRUE(r.u32(count));
  EXPECT_EQ(generation, 40U);
  ASSERT_EQ(count, 2U);
  kcpc::BpEntry first;
  kcpc::BpEntry second;
  ASSERT_TRUE(kcpc::read_bp_entry(r, first));
  ASSERT_TRUE(kcpc::read_bp_entry(r, second));
  EXPECT_EQ(r.remaining(), 0U);
  EXPECT_EQ(first.addr, 0xC000U);
  EXPECT_EQ(first.pass_count, 3U);
  EXPECT_EQ(first.hit_count, 2U);
  EXPECT_EQ(first.condition, "a = 5");
  EXPECT_EQ(second.kind, static_cast<std::uint8_t>(kcpc::BpKind::kWatch));
  EXPECT_EQ(second.len_or_mask, 16U);
  EXPECT_EQ(second.access, kcpc::kBpAccessBoth);
}

TEST_F(KcpcBreakpointsTest, AnEntryIsCheckedBeforeTheMachineIsAsked) {
  start_with_breakpoints();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  std::uint32_t id = 2;

  std::vector<std::uint8_t> cut = bp_payload(exec_bp(0x4000));
  cut.pop_back();
  exchange(*controller, Opcode::kBpAdd, id++, cut, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "too short";

  kcpc::BpEntry bad_kind = exec_bp(0x4000);
  bad_kind.kind = 3;
  kcpc::BpEntry exec_with_access = exec_bp(0x4000);
  exec_with_access.access = kcpc::kBpAccessRead;
  kcpc::BpEntry huge_pass = exec_bp(0x4000);
  huge_pass.pass_count = kcpc::kMaxBpPassCount + 1;
  for (const kcpc::BpEntry& entry :
       {bad_kind, exec_with_access, huge_pass, watch_bp(0x4000, 1, 0),
        watch_bp(0x4000, 1, 4), watch_bp(0x4000, 0, kcpc::kBpAccessWrite)}) {
    exchange(*controller, Opcode::kBpAdd, id++, bp_payload(entry), frame);
    EXPECT_EQ(frame.status(), Status::kBadPayload)
        << "kind " << int{entry.kind} << " access " << int{entry.access};
  }
  exchange(*controller, Opcode::kBpAdd, id++,
           bp_payload(watch_bp(0xFFFF, 2, kcpc::kBpAccessWrite)), frame);
  EXPECT_EQ(frame.status(), Status::kOutOfRange);
  exchange(*controller, Opcode::kBpDel, id++, bp_del_payload(3, 0x4000), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  exchange(*controller, Opcode::kBpDel, id++, {0, 0, 0}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "too short";
  exchange(*controller, Opcode::kBpClear, id++, {3}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  exchange(*controller, Opcode::kBpClear, id++, {}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "too short";
  EXPECT_EQ(bps_.adds + bps_.dels + bps_.clears, 0)
      << "a refused edit reached the machine";

  // A watch that ends exactly at the top of memory is fine.
  exchange(*controller, Opcode::kBpAdd, id,
           bp_payload(watch_bp(0xFFFE, 2, kcpc::kBpAccessRead)), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(reply_generation(frame), 41U);
  EXPECT_EQ(bps_.last_entry.addr, 0xFFFEU);
  EXPECT_EQ(bps_.last_cause.client, hello.client_id);
  EXPECT_EQ(bps_.last_cause.request, id);
  ++id;

  exchange(*controller, Opcode::kBpDel, id++,
           bp_del_payload(static_cast<std::uint8_t>(kcpc::BpKind::kIo), 0x7F00,
                          0xFF00),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(reply_generation(frame), 42U);
  EXPECT_EQ(bps_.last_del.addr, 0x7F00U);
  EXPECT_EQ(bps_.last_del.len_or_mask, 0xFF00U);

  exchange(*controller, Opcode::kBpClear, id++, {kcpc::kBpKindAll}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(bps_.last_clear_kind, kcpc::kBpKindAll);
}

TEST_F(KcpcBreakpointsTest, TheMachinesRefusalsKeepTheirStatusAndWords) {
  start_with_breakpoints();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;

  bps_.refuse = Status::kBadPayload;
  bps_.refusal = "the condition does not parse: unexpected ')'";
  exchange(*controller, Opcode::kBpAdd, 2, bp_payload(exec_bp(0x4000)), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  EXPECT_EQ(frame.message(), bps_.refusal);

  bps_.refuse = Status::kLimitReached;
  bps_.refusal.clear();
  exchange(*controller, Opcode::kBpAdd, 3, bp_payload(exec_bp(0x4000)), frame);
  EXPECT_EQ(frame.status(), Status::kLimitReached);
  EXPECT_NE(frame.message().find("32"), std::string::npos) << frame.message();

  bps_.refuse = Status::kNotFound;
  exchange(*controller, Opcode::kBpDel, 4, bp_del_payload(0, 0x4000), frame);
  EXPECT_EQ(frame.status(), Status::kNotFound);
  EXPECT_FALSE(frame.message().empty());
}

TEST_F(KcpcBreakpointsTest, AnEditIsPushedAfterItsReplyAndToDebugSubscribers) {
  start_with_breakpoints();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  // Before anyone else joins: it is sent to every client connected by then.
  Frame frame;
  expect_event(*controller, Opcode::kEvtControlChanged, frame);
  auto watcher = join(kcpc::Role::kObserver, hello);
  auto deaf = join(kcpc::Role::kObserver, hello);
  subscribe(*controller, 2, kcpc::kEventClassDebug);
  subscribe(*watcher, 2, kcpc::kEventClassDebug);
  subscribe(*deaf, 2, kcpc::kEventClassRun);

  ASSERT_TRUE(
      controller->request(Opcode::kBpAdd, 3, bp_payload(exec_bp(0x4000))));
  ASSERT_TRUE(controller->receive_any(frame));
  EXPECT_EQ(frame.header.kind, static_cast<std::uint8_t>(kcpc::Kind::kResponse))
      << "the event overtook the response";
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(reply_generation(frame), 41U);
  for (TestClient* client : {controller.get(), watcher.get()}) {
    expect_event(*client, Opcode::kEvtBreakpointsChanged, frame);
    ASSERT_EQ(frame.payload.size(), 8U);
    EXPECT_EQ(event_frame(frame), 41U) << "the payload is the generation";
  }
  settle(*deaf, 3);
  EXPECT_TRUE(deaf->events.empty()) << "it did not subscribe to DEBUG";
}

TEST_F(KcpcBreakpointsTest, TheWalkingStepsReachTheMachineWithADeadline) {
  start_with_breakpoints();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  auto const over = static_cast<std::uint8_t>(kcpc::StepMode::kOver);
  auto const to = static_cast<std::uint8_t>(kcpc::StepMode::kTo);

  exchange(*controller, Opcode::kStep, 2, step_payload(over, 0), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U);
  EXPECT_EQ(run_.last_step.mode, over);
  EXPECT_EQ(run_.last_step.count, 1U) << "count 0 means 1";
  EXPECT_EQ(run_.last_step.timeout_ms, kcpc::kDefaultStepTimeoutMs)
      << "timeout 0 means the default";

  kcpc::Writer w;
  kcpc::StepRequest step;
  step.mode = to;
  step.addr = 0xBB5A;
  step.timeout_ms = 250;
  kcpc::write_step_request(w, step);
  exchange(*controller, Opcode::kStep, 3, w.take(), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(run_.last_step.addr, 0xBB5AU);
  EXPECT_EQ(run_.last_step.timeout_ms, 250U);

  // Step over has no instruction limit: its deadline bounds it.
  exchange(*controller, Opcode::kStep, 4,
           step_payload(over, kcpc::kMaxStepInCount + 1), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(run_.last_step.count, kcpc::kMaxStepInCount + 1);

  // A frame step has no deadline, and is not given one.
  exchange(*controller, Opcode::kStep, 5,
           step_payload(static_cast<std::uint8_t>(kcpc::StepMode::kFrames), 2),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(run_.last_step.timeout_ms, 0U);

  run_.refuse = Status::kLimitReached;  // no comparator for the one-shot bp
  exchange(*controller, Opcode::kStep, 6, step_payload(over, 1), frame);
  EXPECT_EQ(frame.status(), Status::kLimitReached);
  EXPECT_FALSE(frame.message().empty());
}

// -- slice 6: input ----------------------------------------------------------

namespace {

std::vector<std::uint8_t> key_payload(std::uint8_t row, std::uint8_t bit,
                                      std::uint8_t down) {
  kcpc::Writer w;
  kcpc::KeyRequest key;
  key.row = row;
  key.bit = bit;
  key.down = down;
  kcpc::write_key_request(w, key);
  return w.take();
}

std::vector<std::uint8_t> text_payload(const std::string& text) {
  kcpc::Writer w;
  w.str(text);
  return w.take();
}

std::vector<std::uint8_t> mouse_payload(std::int16_t dx, std::int16_t dy,
                                        std::uint8_t buttons,
                                        std::uint8_t flags) {
  kcpc::Writer w;
  kcpc::MouseRequest mouse;
  mouse.dx = dx;
  mouse.dy = dy;
  mouse.buttons = buttons;
  mouse.flags = flags;
  kcpc::write_mouse_request(w, mouse);
  return w.take();
}

std::vector<std::uint8_t> gun_payload(std::uint16_t x, std::uint16_t y,
                                      std::uint8_t trigger,
                                      std::uint8_t flags) {
  kcpc::Writer w;
  kcpc::GunRequest gun;
  gun.x = x;
  gun.y = y;
  gun.trigger = trigger;
  gun.flags = flags;
  kcpc::write_gun_request(w, gun);
  return w.take();
}

// The keys the tests press: A, and joystick 0 up.
constexpr std::uint8_t kRowA = 8;
constexpr std::uint8_t kBitA = 5;
constexpr std::uint8_t kRowJoy = 9;
constexpr std::uint8_t kBitUp = 0;

class KcpcInputTest : public KcpcBreakpointsTest {
 protected:
  void start_with_input(int idle_timeout_ms = 30000) {
    kcpc::ServerConfig config = test_config();
    reads_.attach(config);
    writes_.attach(config);
    run_.attach(config, server_);
    bps_.attach(config, server_);
    input_.attach(config);
    config.controller_idle_timeout_ms = idle_timeout_ms;
    start(config);
  }

  static Status key(TestClient& client, std::uint32_t id, std::uint8_t row,
                    std::uint8_t bit, bool down) {
    Frame frame;
    exchange(client, Opcode::kKey, id, key_payload(row, bit, down ? 1 : 0),
             frame);
    return frame.status();
  }

  // Reads events until an EVT_CONTROL_CHANGED says control is free.
  static void await_free_control(TestClient& client) {
    Frame frame;
    for (;;) {
      ASSERT_TRUE(client.next_event(frame)) << "control never became free";
      if (frame.header.opcode !=
          static_cast<std::uint16_t>(Opcode::kEvtControlChanged)) {
        continue;
      }
      kcpc::Reader r(frame.payload);
      std::uint32_t now = 1;
      ASSERT_TRUE(r.u32(now));
      if (now == 0) return;
    }
  }

  FakeInput input_;
};

}  // namespace

TEST_F(KcpcInputTest, HelloAnnouncesInput) {
  start_with_input();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kObserver, hello);
  EXPECT_EQ(hello.caps, kcpc::kServerCaps & ~kDevtoolsCaps & ~kMediaCaps);
  EXPECT_NE(hello.caps & kcpc::kCapInput, 0U);
}

TEST_F(KcpcInputTest, WithoutTheGroupItsOpcodesDoNotExist) {
  start_with_breakpoints();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  EXPECT_EQ(hello.caps & kcpc::kCapInput, 0U);
  Frame frame;
  std::uint32_t id = 2;
  for (Opcode const opcode : {Opcode::kKey, Opcode::kKeysRelease, Opcode::kText,
                              Opcode::kMouse, Opcode::kInputState}) {
    exchange(*controller, opcode, id++, key_payload(kRowA, kBitA, 1), frame);
    EXPECT_EQ(frame.status(), Status::kUnknownOpcode);
  }
}

TEST_F(KcpcInputTest, AKeyReachesTheMatrixAndAnyoneReadsIt) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  auto observer = join(kcpc::Role::kObserver, hello);

  Frame frame;
  exchange(*controller, Opcode::kKey, 2, key_payload(kRowA, kBitA, 1), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U) << "KEY replies with the status alone";
  EXPECT_EQ(input_.row(kRowA), 0xDF);

  // One key, and no other: SHIFT is a key of its own and stays down while
  // another key is pressed and released.
  ASSERT_EQ(key(*controller, 3, 2, 5, true), Status::kOk);
  ASSERT_EQ(key(*controller, 4, kRowJoy, kBitUp, true), Status::kOk);
  ASSERT_EQ(key(*controller, 5, kRowJoy, kBitUp, false), Status::kOk);
  EXPECT_EQ(input_.row(2), 0xDF);
  EXPECT_EQ(input_.row(kRowJoy), 0xFF);

  exchange(*observer, Opcode::kInputState, 2, {}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), 4U + 10U) << "one byte per matrix row";
  for (std::size_t row = 0; row < kcpc::kKeyRows; ++row) {
    EXPECT_EQ(frame.payload[4 + row], row == kRowA || row == 2 ? 0xDF : 0xFF)
        << "row " << row;
  }
}

// "A request that needs control is refused with NOT_CONTROLLER before its
// payload is looked at."
TEST_F(KcpcInputTest, AnObserversInputIsRefusedAndReachesNothing) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  auto observer = join(kcpc::Role::kObserver, hello);
  Frame frame;
  std::uint32_t id = 2;
  for (Opcode const opcode :
       {Opcode::kKey, Opcode::kKeysRelease, Opcode::kText, Opcode::kMouse}) {
    exchange(*observer, opcode, id++, key_payload(kRowA, kBitA, 1), frame);
    EXPECT_EQ(frame.status(), Status::kNotController);
    EXPECT_FALSE(frame.message().empty());
    exchange(*observer, opcode, id++, {}, frame);
    EXPECT_EQ(frame.status(), Status::kNotController) << "malformed, too";
  }
  EXPECT_EQ(input_.count(&FakeInput::presses), 0);
  EXPECT_EQ(input_.count(&FakeInput::release_alls), 0);
  EXPECT_EQ(input_.count(&FakeInput::mouse_calls), 0);
  EXPECT_TRUE(input_.typed.empty());
  EXPECT_EQ(input_.row(kRowA), 0xFF);
}

TEST_F(KcpcInputTest, AKeyIsCheckedBeforeTheMatrixIsTouched) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;

  exchange(*controller, Opcode::kKey, 2, {kRowA, kBitA, 1}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "three bytes of four";
  exchange(*controller, Opcode::kKey, 3, key_payload(kRowA, kBitA, 2), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "down is 0 or 1";
  exchange(*controller, Opcode::kKey, 4, key_payload(10, 0, 1), frame);
  EXPECT_EQ(frame.status(), Status::kOutOfRange) << "rows 0 to 9";
  exchange(*controller, Opcode::kKey, 5, key_payload(0, 8, 1), frame);
  EXPECT_EQ(frame.status(), Status::kOutOfRange) << "bits 0 to 7";
  EXPECT_FALSE(frame.message().empty());
  // The last key of the matrix exists.
  EXPECT_EQ(key(*controller, 6, 9, 7, true), Status::kOk);
  EXPECT_EQ(input_.count(&FakeInput::presses), 1);
}

// Section 7: the keys a controller holds are released when it disconnects,
// gives control up, has it taken, or is dropped for silence. Only those: a
// key another source holds is not this port's to release.
TEST_F(KcpcInputTest, HeldKeysAreReleasedWhenTheControllerDisconnects) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  auto observer = join(kcpc::Role::kObserver, hello);
  input_.press_from_elsewhere(0, 0);

  ASSERT_EQ(key(*controller, 2, kRowA, kBitA, true), Status::kOk);
  ASSERT_EQ(key(*controller, 3, kRowJoy, kBitUp, true), Status::kOk);
  ASSERT_EQ(key(*controller, 4, 2, 5, true), Status::kOk);
  ASSERT_EQ(key(*controller, 5, 2, 5, false), Status::kOk);
  ASSERT_EQ(input_.row(kRowA), 0xDF);
  ASSERT_EQ(input_.row(kRowJoy), 0xFE);

  controller->close();
  // The keys are up by the time anyone is told that control is free.
  await_free_control(*observer);
  EXPECT_EQ(input_.row(kRowA), 0xFF);
  EXPECT_EQ(input_.row(kRowJoy), 0xFF);
  EXPECT_EQ(input_.row(0), 0xFE) << "somebody else's key was released";
  // Two keys were still down; the one it had let go of itself is not
  // released a second time.
  EXPECT_EQ(input_.count(&FakeInput::releases), 1 + 2);
  EXPECT_EQ(input_.count(&FakeInput::release_alls), 0);
}

TEST_F(KcpcInputTest, HeldKeysAreReleasedWhenControlIsGivenUp) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  ASSERT_EQ(key(*controller, 2, kRowA, kBitA, true), Status::kOk);

  kcpc::ControlReply reply;
  control(*controller, 3, kcpc::kControlRelease, 0, reply);
  ASSERT_EQ(reply.role, kcpc::Role::kObserver);
  // Released before the reply: CONTROL is answered by the thread that
  // released them.
  EXPECT_EQ(input_.row(kRowA), 0xFF);

  // Taking control again does not bring the key back, and the keys of the
  // new term are released at its end like the first.
  control(*controller, 4, kcpc::kControlAcquire, 0, reply);
  ASSERT_EQ(reply.role, kcpc::Role::kController);
  EXPECT_EQ(input_.row(kRowA), 0xFF);
  ASSERT_EQ(key(*controller, 5, kRowJoy, kBitUp, true), Status::kOk);
  control(*controller, 6, kcpc::kControlRelease, 0, reply);
  EXPECT_EQ(input_.row(kRowJoy), 0xFF);
  EXPECT_EQ(input_.count(&FakeInput::releases), 2);
}

TEST_F(KcpcInputTest, HeldKeysAreReleasedByAForcedTakeover) {
  start_with_input();
  kcpc::HelloReply hello;
  auto first = join(kcpc::Role::kController, hello);
  auto second = join(kcpc::Role::kObserver, hello);
  ASSERT_EQ(key(*first, 2, kRowA, kBitA, true), Status::kOk);

  kcpc::ControlReply reply;
  control(*second, 2, kcpc::kControlAcquire, kcpc::kControlFlagForce, reply);
  ASSERT_EQ(reply.role, kcpc::Role::kController);
  EXPECT_EQ(input_.row(kRowA), 0xFF) << "the demoted controller's key";

  // The demoted client can press nothing, and the new controller's keys are
  // its own: the first one leaving does not release them.
  EXPECT_EQ(key(*first, 3, kRowA, kBitA, true), Status::kNotController);
  ASSERT_EQ(key(*second, 3, kRowJoy, kBitUp, true), Status::kOk);
  first->close();
  auto const deadline = Clock::now() + std::chrono::seconds(5);
  while (server_.client_count() != 1 && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(server_.client_count(), 1U) << "the first client is still there";
  EXPECT_EQ(input_.row(kRowA), 0xFF);
  EXPECT_EQ(input_.row(kRowJoy), 0xFE);
}

TEST_F(KcpcInputTest, HeldKeysAreReleasedWhenASilentControllerIsDropped) {
  // Long enough that a loaded host still gets the KEY in before the silence
  // counts.
  start_with_input(/*idle_timeout_ms=*/1000);
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  ASSERT_EQ(key(*controller, 2, kRowA, kBitA, true), Status::kOk);
  ASSERT_EQ(input_.row(kRowA), 0xDF);
  expect_error_then_close(*controller, Status::kIdleTimeout);
  EXPECT_EQ(input_.row(kRowA), 0xFF);
}

TEST_F(KcpcInputTest, KeysReleaseLetsGoOfEveryKey) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  input_.press_from_elsewhere(0, 0);
  ASSERT_EQ(key(*controller, 2, kRowA, kBitA, true), Status::kOk);

  Frame frame;
  exchange(*controller, Opcode::kKeysRelease, 3, {}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U);
  EXPECT_EQ(input_.count(&FakeInput::release_alls), 1);
  for (std::size_t row = 0; row < kcpc::kKeyRows; ++row) {
    EXPECT_EQ(input_.row(row), 0xFF) << "row " << row;
  }

  // Nothing is held any more, so nothing is released when it leaves.
  kcpc::ControlReply reply;
  control(*controller, 4, kcpc::kControlRelease, 0, reply);
  EXPECT_EQ(input_.count(&FakeInput::releases), 0);
}

TEST_F(KcpcInputTest, TextIsQueuedAndTheReplySaysHowMuchIsPending) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;

  exchange(*controller, Opcode::kText, 2, text_payload("print 1+1~RETURN~"),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), 4U + 4U);
  kcpc::Reader r(frame.payload);
  std::uint32_t pending = 0;
  r.skip(4);
  ASSERT_TRUE(r.u32(pending));
  EXPECT_EQ(pending, 17U);
  EXPECT_EQ(input_.typed, "print 1+1~RETURN~");

  // Text the queue cannot parse is the client's mistake, in the parser's
  // words; nothing was queued.
  exchange(*controller, Opcode::kText, 3, text_payload("a~NOPE~"), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  EXPECT_EQ(frame.message(), "unknown key: NOPE");
  EXPECT_EQ(input_.typed, "print 1+1~RETURN~");

  // A `str` that says more bytes than arrived.
  exchange(*controller, Opcode::kText, 4, {5, 0, 'a', 'b'}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  exchange(*controller, Opcode::kText, 5, text_payload(""), frame);
  EXPECT_EQ(frame.status(), Status::kOk) << "no text is valid text";
}

TEST_F(KcpcInputTest, MouseMovesAndSetsButtonsOnlyWhenAsked) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  constexpr std::uint8_t kLeft = kcpc::kMouseButtonLeft;

  exchange(*controller, Opcode::kMouse, 2,
           mouse_payload(10, -4, kLeft, kcpc::kMouseFlagButtons), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U);
  EXPECT_EQ(input_.mouse_x, 10);
  EXPECT_EQ(input_.mouse_y, -4);
  EXPECT_EQ(input_.buttons(), kLeft);

  // Without the flag the buttons stay as they are, whatever the field holds.
  exchange(*controller, Opcode::kMouse, 3, mouse_payload(-300, 300, 0, 0),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(input_.mouse_x, -290);
  EXPECT_EQ(input_.mouse_y, 296);
  EXPECT_EQ(input_.buttons(), kLeft);

  // Bits that are no button are dropped.
  exchange(*controller, Opcode::kMouse, 4,
           mouse_payload(0, 0, 0xFC | kcpc::kMouseButtonRight,
                         kcpc::kMouseFlagButtons),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(input_.buttons(), kcpc::kMouseButtonRight);

  exchange(*controller, Opcode::kMouse, 5, {1, 0, 1, 0, 0}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "five bytes of six";

  // A button held when control is lost is let go, like a key.
  kcpc::ControlReply reply;
  control(*controller, 6, kcpc::kControlRelease, 0, reply);
  EXPECT_EQ(input_.buttons(), 0);
  EXPECT_EQ(input_.mouse_x, -290) << "letting go moves nothing";
}

TEST_F(KcpcInputTest, MouseWithNoMouseFittedIsNotPresent) {
  input_.mouse_fitted = false;
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  exchange(*controller, Opcode::kMouse, 2,
           mouse_payload(1, 1, 0, kcpc::kMouseFlagButtons), frame);
  EXPECT_EQ(frame.status(), Status::kNotPresent);
  EXPECT_NE(frame.message().find("mouse"), std::string::npos);
  expect_ping_ok(*controller, 3);
}

// -- the light gun (beads-il6h) ----------------------------------------------

TEST_F(KcpcInputTest, HelloAnnouncesTheGun) {
  start_with_input();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kObserver, hello);
  EXPECT_NE(hello.caps & kcpc::kCapGun, 0U);
}

TEST_F(KcpcInputTest, WithoutAGunCallbackGunDoesNotExist) {
  start_with_breakpoints();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  EXPECT_EQ(hello.caps & kcpc::kCapGun, 0U);
  Frame frame;
  exchange(*controller, Opcode::kGun, 2,
           gun_payload(10, 10, 1, kcpc::kGunFlagAim | kcpc::kGunFlagTrigger),
           frame);
  EXPECT_EQ(frame.status(), Status::kUnknownOpcode);
}

TEST_F(KcpcInputTest, GunAimsAndPullsOnlyWhatItIsAskedTo) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;

  exchange(*controller, Opcode::kGun, 2,
           gun_payload(400, 120, 1, kcpc::kGunFlagAim | kcpc::kGunFlagTrigger),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(frame.payload.size(), 4U) << "GUN replies with the status alone";
  EXPECT_EQ(input_.gun_x, 400);
  EXPECT_EQ(input_.gun_y, 120);
  EXPECT_TRUE(input_.pulled());

  // The aim alone: the trigger stays pulled, whatever its field holds.
  exchange(*controller, Opcode::kGun, 3,
           gun_payload(767, 271, 0, kcpc::kGunFlagAim), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(input_.gun_x, 767);
  EXPECT_EQ(input_.gun_y, 271);
  EXPECT_TRUE(input_.pulled());

  // The trigger alone: the aim stays, whatever its fields hold.
  exchange(*controller, Opcode::kGun, 4,
           gun_payload(9999, 9999, 0, kcpc::kGunFlagTrigger), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(input_.gun_x, 767);
  EXPECT_FALSE(input_.pulled());
}

TEST_F(KcpcInputTest, GunIsCheckedBeforeTheGunIsTouched) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  auto observer = join(kcpc::Role::kObserver, hello);
  Frame frame;

  exchange(*observer, Opcode::kGun, 2,
           gun_payload(1, 1, 1, kcpc::kGunFlagTrigger), frame);
  EXPECT_EQ(frame.status(), Status::kNotController);
  exchange(*observer, Opcode::kGun, 3, {}, frame);
  EXPECT_EQ(frame.status(), Status::kNotController) << "malformed, too";

  exchange(*controller, Opcode::kGun, 2, {1, 0, 1, 0, 1}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "five bytes of six";
  exchange(*controller, Opcode::kGun, 3,
           gun_payload(1, 1, 2, kcpc::kGunFlagTrigger), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "trigger is 0 or 1";
  exchange(*controller, Opcode::kGun, 4,
           gun_payload(768, 0, 0, kcpc::kGunFlagAim), frame);
  EXPECT_EQ(frame.status(), Status::kOutOfRange) << "x 0 to 767";
  exchange(*controller, Opcode::kGun, 5,
           gun_payload(0, 272, 0, kcpc::kGunFlagAim), frame);
  EXPECT_EQ(frame.status(), Status::kOutOfRange) << "y 0 to 271";
  EXPECT_FALSE(frame.message().empty());
  EXPECT_EQ(input_.count(&FakeInput::gun_calls), 0);

  // Fields a flag does not name are not checked: they are not used.
  exchange(*controller, Opcode::kGun, 6, gun_payload(768, 272, 7, 0), frame);
  EXPECT_EQ(frame.status(), Status::kOk);
}

TEST_F(KcpcInputTest, GunWithNoGunEnabledIsNotPresent) {
  input_.gun_fitted = false;
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  exchange(*controller, Opcode::kGun, 2,
           gun_payload(1, 1, 1, kcpc::kGunFlagTrigger), frame);
  EXPECT_EQ(frame.status(), Status::kNotPresent);
  EXPECT_NE(frame.message().find("light gun"), std::string::npos)
      << frame.message();
  expect_ping_ok(*controller, 3);
}

// A trigger held when control is lost is let go, like a key; the aim stays.
TEST_F(KcpcInputTest, AHeldTriggerIsLetGoWithControl) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  exchange(*controller, Opcode::kGun, 2,
           gun_payload(300, 100, 1, kcpc::kGunFlagAim | kcpc::kGunFlagTrigger),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_TRUE(input_.pulled());

  kcpc::ControlReply reply;
  control(*controller, 3, kcpc::kControlRelease, 0, reply);
  EXPECT_FALSE(input_.pulled());
  EXPECT_EQ(input_.gun_x, 300) << "letting go aims nowhere else";
  EXPECT_EQ(input_.gun_y, 100);
  EXPECT_EQ(input_.count(&FakeInput::gun_calls), 2);

  // One that was let go already is not let go again.
  control(*controller, 4, kcpc::kControlAcquire, 0, reply);
  exchange(*controller, Opcode::kGun, 5, gun_payload(0, 0, 1, 0), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  control(*controller, 6, kcpc::kControlRelease, 0, reply);
  EXPECT_EQ(input_.count(&FakeInput::gun_calls), 3);
}

// A controller that disconnects holding the trigger lets go of it too.
TEST_F(KcpcInputTest, ADisconnectedControllerLetsGoOfTheTrigger) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  auto observer = join(kcpc::Role::kObserver, hello);
  Frame frame;
  exchange(*controller, Opcode::kGun, 2,
           gun_payload(0, 0, 1, kcpc::kGunFlagTrigger), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_TRUE(input_.pulled());

  controller->close();
  // Let go by the time anyone is told that control is free.
  await_free_control(*observer);
  EXPECT_FALSE(input_.pulled());
}

// -- slice 7: what the DevTools show -----------------------------------------

namespace {

std::vector<std::uint8_t> chip_payload(std::uint8_t chip) {
  return {chip, 0, 0, 0};
}

std::vector<std::uint8_t> symbol_payload(std::uint16_t addr,
                                         const std::string& name) {
  kcpc::Writer w;
  kcpc::write_symbol(w, kcpc::Symbol{addr, name});
  return w.take();
}

class KcpcDevtoolsTest : public KcpcInputTest {
 protected:
  void start_with_devtools() {
    kcpc::ServerConfig config = test_config();
    reads_.attach(config);
    writes_.attach(config);
    run_.attach(config, server_);
    bps_.attach(config, server_);
    input_.attach(config);
    devtools_.attach(config, server_);
    start(config);
  }

  FakeDevtools devtools_;
};

}  // namespace

TEST_F(KcpcDevtoolsTest, HelloAnnouncesChipsSymbolsAndTheFrame) {
  start_with_devtools();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kObserver, hello);
  EXPECT_EQ(hello.caps, kcpc::kServerCaps & ~kMediaCaps);
  EXPECT_EQ(hello.caps & kDevtoolsCaps, kDevtoolsCaps);
}

TEST_F(KcpcDevtoolsTest, WithoutTheGroupsTheirOpcodesDoNotExist) {
  start_with_input();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  EXPECT_EQ(hello.caps & kDevtoolsCaps, 0U);
  Frame frame;
  std::uint32_t id = 2;
  for (Opcode const opcode :
       {Opcode::kChipGet, Opcode::kSymList, Opcode::kSymAdd, Opcode::kSymDel,
        Opcode::kSymLoad, Opcode::kFbInfo}) {
    exchange(*controller, opcode, id++, chip_payload(1), frame);
    EXPECT_EQ(frame.status(), Status::kUnknownOpcode);
  }
}

TEST_F(KcpcDevtoolsTest, ChipGetRepliesWithTheStampTheLayoutAndTheBlock) {
  devtools_.chips.crtc.type = 3;
  devtools_.chips.crtc.reg[0] = 63;
  devtools_.chips.crtc.ma = 0x3000;
  devtools_.chips.gate_array.ink[16] = 0x14;
  devtools_.chips.psg.reg_select = 7;
  devtools_.chips.fdc.generation[1] = 0x0102030405060708ULL;
  devtools_.chips.asic.unlocked = 1;
  devtools_.chips.asic.sprite[15].mag_y = 4;
  start_with_devtools();
  kcpc::HelloReply hello;
  auto observer = join(kcpc::Role::kObserver, hello);

  // What each block weighs after the 16 bytes of stamp, chip and layout.
  const std::size_t sizes[] = {0, 30, 25, 17, 24, 154};
  Frame frame;
  for (std::uint8_t chip = 1; chip <= 5; ++chip) {
    exchange(*observer, Opcode::kChipGet, 1U + chip, chip_payload(chip), frame);
    ASSERT_EQ(frame.status(), Status::kOk) << "chip " << int{chip};
    EXPECT_EQ(frame.payload.size(), 4U + 16U + sizes[chip])
        << "chip " << int{chip};
    kcpc::Reader r(frame.payload);
    r.skip(4);
    kcpc::ChipState state;
    std::uint16_t layout = 0;
    ASSERT_TRUE(kcpc::read_chip_reply(r, state, layout));
    EXPECT_EQ(state.chip, chip);
    EXPECT_EQ(layout, kcpc::chip_layout(static_cast<kcpc::Chip>(chip)));
    EXPECT_EQ(state.stamp.frame, 77U);
    EXPECT_TRUE(state.stamp.paused);
    EXPECT_EQ(r.remaining(), 0U);
    switch (static_cast<kcpc::Chip>(chip)) {
      case kcpc::Chip::kCrtc:
        EXPECT_EQ(state.crtc.type, 3);
        EXPECT_EQ(state.crtc.reg[0], 63);
        EXPECT_EQ(state.crtc.ma, 0x3000);
        break;
      case kcpc::Chip::kGateArray:
        EXPECT_EQ(state.gate_array.ink[16], 0x14);
        break;
      case kcpc::Chip::kPsg:
        EXPECT_EQ(state.psg.reg_select, 7);
        break;
      case kcpc::Chip::kFdc:
        EXPECT_EQ(state.fdc.generation[1], 0x0102030405060708ULL);
        break;
      case kcpc::Chip::kAsic:
        EXPECT_EQ(state.asic.unlocked, 1);
        EXPECT_EQ(state.asic.sprite[15].mag_y, 4);
        break;
    }
  }
}

TEST_F(KcpcDevtoolsTest, AChipIsCheckedBeforeTheMachineIsAsked) {
  start_with_devtools();
  kcpc::HelloReply hello;
  auto observer = join(kcpc::Role::kObserver, hello);
  Frame frame;
  exchange(*observer, Opcode::kChipGet, 2, chip_payload(0), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  exchange(*observer, Opcode::kChipGet, 3, chip_payload(6), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  exchange(*observer, Opcode::kChipGet, 4, {1, 0, 0}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "three bytes of four";
  EXPECT_EQ(devtools_.chip_calls, 0);

  // What the machine refuses keeps its status.
  devtools_.has_asic = false;
  exchange(*observer, Opcode::kChipGet, 5, chip_payload(5), frame);
  EXPECT_EQ(frame.status(), Status::kNotPresent);
  EXPECT_NE(frame.message().find("ASIC"), std::string::npos);
  devtools_.chip_status = Status::kBusy;
  exchange(*observer, Opcode::kChipGet, 6, chip_payload(1), frame);
  EXPECT_EQ(frame.status(), Status::kBusy);
  expect_ping_ok(*observer, 7);
}

TEST_F(KcpcDevtoolsTest, AnyoneListsSymbolsAndOnlyTheControllerEdits) {
  start_with_devtools();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  auto observer = join(kcpc::Role::kObserver, hello);
  Frame frame;

  exchange(*controller, Opcode::kSymAdd, 2, symbol_payload(0x4000, "start"),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(reply_generation(frame), 11U);

  exchange(*controller, Opcode::kSymLoad, 3, text_payload("game.sym"), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), 4U + 8U + 4U);
  EXPECT_EQ(reply_generation(frame), 12U);
  EXPECT_EQ(frame.payload[12], 2) << "symbols loaded";

  exchange(*observer, Opcode::kSymList, 2, {}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  std::uint64_t generation = 0;
  std::uint32_t count = 0;
  ASSERT_TRUE(r.u64(generation));
  ASSERT_TRUE(r.u32(count));
  EXPECT_EQ(generation, 12U);
  ASSERT_EQ(count, 3U);
  kcpc::Symbol symbol;
  ASSERT_TRUE(kcpc::read_symbol(r, symbol));
  EXPECT_EQ(symbol.addr, 0x4000);
  EXPECT_EQ(symbol.name, "start");

  exchange(*controller, Opcode::kSymDel, 4, text_payload("start"), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(reply_generation(frame), 13U);
  exchange(*controller, Opcode::kSymDel, 5, text_payload("start"), frame);
  EXPECT_EQ(frame.status(), Status::kNotFound);
  EXPECT_NE(frame.message().find("symbol"), std::string::npos);
  exchange(*controller, Opcode::kSymLoad, 6, text_payload("nowhere.sym"),
           frame);
  EXPECT_EQ(frame.status(), Status::kNotFound);
  EXPECT_NE(frame.message().find("file"), std::string::npos);

  // An observer may not edit, whatever it sends.
  std::uint32_t id = 3;
  for (Opcode const opcode :
       {Opcode::kSymAdd, Opcode::kSymDel, Opcode::kSymLoad}) {
    exchange(*observer, opcode, id++, symbol_payload(1, "x"), frame);
    EXPECT_EQ(frame.status(), Status::kNotController);
    exchange(*observer, opcode, id++, {}, frame);
    EXPECT_EQ(frame.status(), Status::kNotController) << "malformed, too";
  }
  EXPECT_EQ(devtools_.sym_generation, 13U);
}

TEST_F(KcpcDevtoolsTest, ASymbolIsCheckedBeforeTheTableIsTouched) {
  start_with_devtools();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  std::uint32_t id = 2;
  for (const std::string& name :
       {std::string(), std::string("two words"), std::string("tab\there"),
        std::string("semi;colon"), std::string(256, 'x')}) {
    exchange(*controller, Opcode::kSymAdd, id++, symbol_payload(0x4000, name),
             frame);
    EXPECT_EQ(frame.status(), Status::kBadPayload) << '"' << name << '"';
    EXPECT_FALSE(frame.message().empty());
  }
  exchange(*controller, Opcode::kSymAdd, id++, {0x00}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "cut short";
  exchange(*controller, Opcode::kSymDel, id++, {0x05, 0x00, 'a'}, frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "a str longer than it is";
  exchange(*controller, Opcode::kSymLoad, id++, text_payload(""), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload) << "no path";
  EXPECT_TRUE(devtools_.symbols.empty());
  EXPECT_EQ(devtools_.sym_generation, 10U);

  // The longest name there is.
  exchange(*controller, Opcode::kSymAdd, id++,
           symbol_payload(0x4000, std::string(255, 'x')), frame);
  EXPECT_EQ(frame.status(), Status::kOk);
}

TEST_F(KcpcDevtoolsTest, ASymbolEditIsPushedAfterItsReplyToDebugSubscribers) {
  start_with_devtools();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  // Before anyone else joins: it is sent to every client connected by then.
  Frame frame;
  expect_event(*controller, Opcode::kEvtControlChanged, frame);
  auto watcher = join(kcpc::Role::kObserver, hello);
  auto deaf = join(kcpc::Role::kObserver, hello);
  subscribe(*controller, 2, kcpc::kEventClassDebug);
  subscribe(*watcher, 2, kcpc::kEventClassDebug);
  subscribe(*deaf, 2, kcpc::kEventClassRun | kcpc::kEventClassMedia);

  ASSERT_TRUE(
      controller->request(Opcode::kSymAdd, 3, symbol_payload(0x4000, "start")));
  ASSERT_TRUE(controller->receive_any(frame));
  EXPECT_EQ(frame.header.kind, static_cast<std::uint8_t>(kcpc::Kind::kResponse))
      << "the event overtook the response";
  ASSERT_EQ(frame.status(), Status::kOk);
  for (TestClient* client : {controller.get(), watcher.get()}) {
    expect_event(*client, Opcode::kEvtSymbolsChanged, frame);
    ASSERT_EQ(frame.payload.size(), 8U);
    EXPECT_EQ(event_frame(frame), 11U) << "the payload is the generation";
  }
  settle(*deaf, 3);
  EXPECT_TRUE(deaf->events.empty()) << "it did not subscribe to DEBUG";
}

TEST_F(KcpcDevtoolsTest, FbInfoDescribesTheFrame) {
  start_with_devtools();
  kcpc::HelloReply hello;
  auto observer = join(kcpc::Role::kObserver, hello);
  devtools_.fb_seq = 41;
  Frame frame;
  exchange(*observer, Opcode::kFbInfo, 2, {}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  // width, height, format, transport, two reserved, stride, fb_seq and the
  // ring's name as a str.
  ASSERT_EQ(frame.payload.size(),
            4U + 2U + 2U + 1U + 1U + 2U + 4U + 8U + 2U + 13U);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::FbInfo info;
  ASSERT_TRUE(kcpc::read_fb_info(r, info));
  EXPECT_EQ(info.width, 768);
  EXPECT_EQ(info.height, 272);
  EXPECT_EQ(info.format, kcpc::kFbFormatRgb24);
  EXPECT_EQ(info.transport, kcpc::kFbTransportShm);
  EXPECT_EQ(info.stride, 2304U);
  EXPECT_EQ(info.fb_seq, 41U);
  EXPECT_EQ(info.shm_name, "/kcpc-fb-4242");

  devtools_.fb_seq = 42;  // live: read at each request
  exchange(*observer, Opcode::kFbInfo, 3, {}, frame);
  kcpc::Reader again(frame.payload);
  again.skip(4);
  ASSERT_TRUE(kcpc::read_fb_info(again, info));
  EXPECT_EQ(info.fb_seq, 42U);
}

TEST_F(KcpcDevtoolsTest, WithoutTheRingFbShmIsNotAnnounced) {
  devtools_.fb_ring = false;  // the core could not create the region
  start_with_devtools();
  kcpc::HelloReply hello;
  auto observer = join(kcpc::Role::kObserver, hello);
  EXPECT_NE(hello.caps & kcpc::kCapFbInfo, 0U);
  EXPECT_EQ(hello.caps & kcpc::kCapFbShm, 0U);
  Frame frame;
  exchange(*observer, Opcode::kFbInfo, 2, {}, frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::FbInfo info;
  ASSERT_TRUE(kcpc::read_fb_info(r, info));
  EXPECT_EQ(info.transport, kcpc::kFbTransportNone);
  EXPECT_TRUE(info.shm_name.empty());
}

TEST_F(KcpcDevtoolsTest, AMediaChangeIsSentWithWhatTheSlotHoldsNow) {
  start_with_devtools();
  kcpc::HelloReply hello;
  auto watcher = join(kcpc::Role::kObserver, hello);
  auto deaf = join(kcpc::Role::kObserver, hello);
  subscribe(*watcher, 2, kcpc::kEventClassMedia);
  subscribe(*deaf, 2, kcpc::kEventClassDebug);

  kcpc::Event event;
  event.opcode = Opcode::kEvtMediaChanged;
  event.slot = 0;
  event.generation = 7;
  server_.post(event);
  event.slot = 2;
  event.generation = 3;
  server_.post(event);

  Frame frame;
  expect_event(*watcher, Opcode::kEvtMediaChanged, frame);
  {
    kcpc::Reader r(frame.payload);
    std::uint8_t slot = 9;
    std::uint8_t present = 9;
    std::uint64_t generation = 0;
    std::string path;
    r.u8(slot);
    r.u8(present);
    r.skip(2);
    r.u64(generation);
    ASSERT_TRUE(r.str(path));
    EXPECT_EQ(slot, 0);
    EXPECT_EQ(present, 1);
    EXPECT_EQ(generation, 7U);
    EXPECT_EQ(path, "/discs/game.dsk");
    EXPECT_EQ(r.remaining(), 0U);
  }
  expect_event(*watcher, Opcode::kEvtMediaChanged, frame);
  ASSERT_EQ(frame.payload.size(), 1U + 1U + 2U + 8U + 2U) << "no path";
  EXPECT_EQ(frame.payload[0], 2) << "the tape";
  EXPECT_EQ(frame.payload[1], 0) << "nothing in it";
  // Asked once an event, not once a client.
  EXPECT_EQ(devtools_.media_asked, (std::vector<std::uint8_t>{0, 2}));
  settle(*deaf, 3);
  EXPECT_TRUE(deaf->events.empty()) << "it did not subscribe to MEDIA";
}

TEST_F(KcpcDevtoolsTest, ARebuiltMachineIsSentAsItsMachineInfo) {
  start_with_devtools();
  kcpc::HelloReply hello;
  auto watcher = join(kcpc::Role::kObserver, hello);
  auto deaf = join(kcpc::Role::kObserver, hello);
  subscribe(*watcher, 2, kcpc::kEventClassMachine);
  subscribe(*deaf, 2, kcpc::kEventClassMedia);

  kcpc::Event event;
  event.opcode = Opcode::kEvtMachineChanged;
  event.machine.model = 3;
  event.machine.crtc_type = 3;
  event.machine.flags = kcpc::kMachineFlagAsic;
  event.machine.ram_bytes = 576U * 1024U;
  event.machine.rom_slots = 0x80;
  event.machine.stamp.frame = 1234;
  event.machine.stamp.paused = true;
  server_.post(event);

  Frame frame;
  expect_event(*watcher, Opcode::kEvtMachineChanged, frame);
  kcpc::Reader r(frame.payload);
  kcpc::MachineInfo info;
  ASSERT_TRUE(kcpc::read_machine_info(r, info));
  EXPECT_EQ(r.remaining(), 0U) << "exactly the MACHINE_INFO reply fields";
  EXPECT_EQ(info.model, 3);
  EXPECT_EQ(info.flags, kcpc::kMachineFlagAsic);
  EXPECT_EQ(info.ram_bytes, 576U * 1024U);
  EXPECT_EQ(info.rom_slots, 0x80U);
  EXPECT_EQ(info.stamp.frame, 1234U);
  EXPECT_TRUE(info.stamp.paused);
  settle(*deaf, 3);
  EXPECT_TRUE(deaf->events.empty()) << "it did not subscribe to MACHINE";
}

// -- slice 9: media, snapshots, configuration -------------------------------

namespace {

std::vector<std::uint8_t> insert_payload(std::uint8_t slot,
                                         const std::string& path,
                                         std::uint8_t flags = 0) {
  kcpc::Writer w;
  kcpc::write_media_insert_request(w,
                                   kcpc::MediaInsertRequest{slot, flags, path});
  return w.take();
}

std::vector<std::uint8_t> slot_payload(std::uint8_t slot) {
  kcpc::Writer w;
  kcpc::write_media_slot_request(w, slot);
  return w.take();
}

std::vector<std::uint8_t> tape_payload(std::uint8_t action,
                                       std::uint32_t block = 0) {
  kcpc::Writer w;
  kcpc::write_tape_control_request(w, kcpc::TapeControlRequest{action, block});
  return w.take();
}

std::vector<std::uint8_t> snapshot_load_payload(const std::string& path,
                                                std::uint8_t flags = 0) {
  kcpc::Writer w;
  kcpc::write_snapshot_load_request(w, kcpc::SnapshotLoadRequest{flags, path});
  return w.take();
}

std::vector<std::uint8_t> config_set_payload(const std::string& key,
                                             const std::string& value) {
  kcpc::Writer w;
  kcpc::write_config_set_request(w, kcpc::ConfigSetRequest{key, value});
  return w.take();
}

class KcpcMediaTest : public KcpcDevtoolsTest {
 protected:
  void start_with_media() {
    kcpc::ServerConfig config = test_config();
    reads_.attach(config);
    writes_.attach(config);
    run_.attach(config, server_);
    bps_.attach(config, server_);
    input_.attach(config);
    devtools_.attach(config, server_);
    media_.attach(config, server_);
    start(config);
  }

  FakeMedia media_;
};

}  // namespace

TEST_F(KcpcMediaTest, HelloAnnouncesMediaSnapshotsAndConfiguration) {
  start_with_media();
  kcpc::HelloReply hello;
  auto client = join(kcpc::Role::kObserver, hello);
  EXPECT_EQ(hello.caps, kcpc::kServerCaps);
  EXPECT_EQ(hello.caps & kMediaCaps, kMediaCaps);
}

TEST_F(KcpcMediaTest, WithoutTheGroupsTheirOpcodesDoNotExist) {
  start_with_devtools();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  EXPECT_EQ(hello.caps & kMediaCaps, 0U);
  Frame frame;
  std::uint32_t id = 2;
  for (Opcode const opcode :
       {Opcode::kMediaInsert, Opcode::kMediaEject, Opcode::kMediaInfo,
        Opcode::kTapeControl, Opcode::kSnapshotSave, Opcode::kSnapshotLoad,
        Opcode::kConfigGet, Opcode::kConfigSet, Opcode::kConfigApply}) {
    exchange(*controller, opcode, id++, slot_payload(0), frame);
    EXPECT_EQ(frame.status(), Status::kUnknownOpcode);
  }
}

TEST_F(KcpcMediaTest, OnlyTheControllerChangesAndAnyoneAsks) {
  start_with_media();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  auto observer = join(kcpc::Role::kObserver, hello);
  Frame frame;

  std::uint32_t id = 2;
  for (Opcode const opcode :
       {Opcode::kMediaInsert, Opcode::kMediaEject, Opcode::kTapeControl,
        Opcode::kSnapshotSave, Opcode::kSnapshotLoad, Opcode::kConfigSet,
        Opcode::kConfigApply}) {
    exchange(*observer, opcode, id++, insert_payload(0, "game.dsk"), frame);
    EXPECT_EQ(frame.status(), Status::kNotController);
    exchange(*observer, opcode, id++, {}, frame);
    EXPECT_EQ(frame.status(), Status::kNotController) << "malformed, too";
  }
  EXPECT_EQ(media_.calls, 0) << "nothing reached the machine";

  exchange(*controller, Opcode::kMediaInsert, 2, insert_payload(1, "game.dsk"),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), 4U + 8U);
  EXPECT_EQ(reply_generation(frame), 31U);

  // MEDIA_INFO and CONFIG_GET are an observer's too.
  exchange(*observer, Opcode::kMediaInfo, id++, slot_payload(1), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::MediaInfo info;
  ASSERT_TRUE(kcpc::read_media_info(r, info));
  EXPECT_EQ(r.remaining(), 0U) << "a drive has no tape fields";
  EXPECT_EQ(info.slot, 1) << "the slot that was asked for";
  EXPECT_EQ(info.present, 1);
  EXPECT_EQ(info.flags, kcpc::kMediaFlagUnsaved);
  EXPECT_EQ(info.generation, 31U);
  EXPECT_EQ(info.path, "game.dsk");

  exchange(*observer, Opcode::kMediaInfo, id++, slot_payload(2), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  kcpc::Reader t(frame.payload);
  t.skip(4);
  ASSERT_TRUE(kcpc::read_media_info(t, info));
  EXPECT_EQ(t.remaining(), 0U);
  EXPECT_EQ(info.present, 0);
  EXPECT_EQ(info.tape.motor, 1) << "the tape slot ends with the deck";
  EXPECT_EQ(info.tape.blocks, 3U);

  exchange(*observer, Opcode::kConfigGet, id++, text_payload("model"), frame);
  ASSERT_EQ(frame.status(), Status::kOk);

  exchange(*controller, Opcode::kMediaEject, 3, slot_payload(1), frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(reply_generation(frame), 32U);
}

TEST_F(KcpcMediaTest, AMediaRequestIsCheckedBeforeTheMachineIsAsked) {
  start_with_media();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  std::uint32_t id = 2;
  auto refused = [&](Opcode opcode, const std::vector<std::uint8_t>& payload,
                     const char* what) {
    exchange(*controller, opcode, id++, payload, frame);
    EXPECT_EQ(frame.status(), Status::kBadPayload) << what;
    EXPECT_FALSE(frame.message().empty()) << what;
  };
  refused(Opcode::kMediaInsert, insert_payload(4, "game.dsk"), "slot 4");
  refused(Opcode::kMediaInsert, insert_payload(0, "game.dsk", 1),
          "a reserved flag");
  refused(Opcode::kMediaInsert, insert_payload(0, ""), "no path");
  refused(Opcode::kMediaInsert, {0, 0}, "too short");
  refused(Opcode::kMediaEject, slot_payload(4), "slot 4");
  refused(Opcode::kMediaEject, {0}, "too short");
  refused(Opcode::kMediaInfo, slot_payload(9), "slot 9");
  refused(Opcode::kMediaInfo, {}, "too short");
  refused(Opcode::kTapeControl, tape_payload(4), "an unknown action");
  refused(Opcode::kTapeControl, {1, 0, 0, 0}, "too short");
  refused(Opcode::kSnapshotSave, text_payload(""), "no path");
  refused(Opcode::kSnapshotSave, {}, "too short");
  refused(Opcode::kSnapshotLoad, snapshot_load_payload(""), "no path");
  refused(Opcode::kSnapshotLoad, {0}, "too short");
  refused(Opcode::kConfigGet, {}, "too short");
  refused(Opcode::kConfigSet, text_payload("model"), "no value field");
  EXPECT_EQ(media_.calls, 0) << "none of them reached the machine";

  // What the machine's side refuses keeps its status and its words.
  exchange(*controller, Opcode::kMediaInsert, id++,
           insert_payload(0, "missing.dsk"), frame);
  EXPECT_EQ(frame.status(), Status::kNotFound);
  EXPECT_NE(frame.message().find("file"), std::string::npos);
  exchange(*controller, Opcode::kMediaInsert, id++,
           insert_payload(0, "junk.dsk"), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  EXPECT_EQ(frame.message(), "this slot takes .dsk.ipf.raw");
}

TEST_F(KcpcMediaTest, AMediaChangeFollowsItsReplyAndReachesSubscribers) {
  start_with_media();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  // Before anyone else joins: it is sent to every client connected by then.
  Frame frame;
  expect_event(*controller, Opcode::kEvtControlChanged, frame);
  auto watcher = join(kcpc::Role::kObserver, hello);
  auto deaf = join(kcpc::Role::kObserver, hello);
  subscribe(*controller, 2, kcpc::kEventClassMedia);
  subscribe(*watcher, 2, kcpc::kEventClassMedia);
  subscribe(*deaf, 2, kcpc::kEventClassRun);

  ASSERT_TRUE(controller->request(Opcode::kMediaInsert, 3,
                                  insert_payload(0, "game.dsk")));
  ASSERT_TRUE(controller->receive_any(frame));
  EXPECT_EQ(frame.header.kind, static_cast<std::uint8_t>(kcpc::Kind::kResponse))
      << "the event overtook the response";
  ASSERT_EQ(frame.status(), Status::kOk);
  for (TestClient* client : {controller.get(), watcher.get()}) {
    expect_event(*client, Opcode::kEvtMediaChanged, frame);
    kcpc::Reader r(frame.payload);
    std::uint8_t slot = 9;
    std::uint64_t generation = 0;
    r.u8(slot);
    r.skip(3);
    ASSERT_TRUE(r.u64(generation));
    EXPECT_EQ(slot, 0);
    EXPECT_EQ(generation, 21U);
  }
  settle(*deaf, 3);
  EXPECT_TRUE(deaf->events.empty()) << "it did not subscribe to MEDIA";
}

TEST_F(KcpcMediaTest, TapeControlRepliesWithTheDeck) {
  start_with_media();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  Frame frame;
  exchange(*controller, Opcode::kTapeControl, 2,
           tape_payload(static_cast<std::uint8_t>(kcpc::TapeAction::kPlay)),
           frame);
  EXPECT_EQ(frame.status(), Status::kNotPresent);
  EXPECT_NE(frame.message().find("tape"), std::string::npos);

  exchange(*controller, Opcode::kMediaInsert, 3, insert_payload(2, "side.cdt"),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  exchange(*controller, Opcode::kTapeControl, 4,
           tape_payload(static_cast<std::uint8_t>(kcpc::TapeAction::kSeek), 2),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), 4U + 12U);
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::TapeState tape;
  ASSERT_TRUE(kcpc::read_tape_state(r, tape));
  EXPECT_EQ(tape.playing, 0);
  EXPECT_EQ(tape.motor, 1);
  EXPECT_EQ(tape.block, 2U);
  EXPECT_EQ(tape.blocks, 3U);

  exchange(*controller, Opcode::kTapeControl, 5,
           tape_payload(static_cast<std::uint8_t>(kcpc::TapeAction::kSeek), 3),
           frame);
  EXPECT_EQ(frame.status(), Status::kOutOfRange);
}

TEST_F(KcpcMediaTest, SnapshotsReplyWithAStampAndALoadIsAnnounced) {
  start_with_media();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  subscribe(*controller, 2, kcpc::kEventClassRun);
  Frame frame;
  expect_event(*controller, Opcode::kEvtControlChanged, frame);

  exchange(*controller, Opcode::kSnapshotSave, 3, text_payload("state.sna"),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), 4U + 12U);
  EXPECT_EQ(media_.saved, "state.sna");
  kcpc::Reader r(frame.payload);
  r.skip(4);
  kcpc::Stamp stamp;
  ASSERT_TRUE(kcpc::read_stamp(r, stamp));
  EXPECT_EQ(stamp.frame, 500U);
  EXPECT_TRUE(stamp.paused);

  exchange(*controller, Opcode::kSnapshotSave, 4,
           text_payload("/nowhere/state.sna"), frame);
  EXPECT_EQ(frame.status(), Status::kNotFound);
  EXPECT_EQ(frame.message(), "no such directory");

  ASSERT_TRUE(controller->request(
      Opcode::kSnapshotLoad, 5,
      snapshot_load_payload("state.sna", kcpc::kSnapshotFlagStayPaused)));
  ASSERT_TRUE(controller->receive_any(frame));
  EXPECT_EQ(frame.header.kind, static_cast<std::uint8_t>(kcpc::Kind::kResponse))
      << "the reset overtook the response";
  ASSERT_EQ(frame.status(), Status::kOk);
  EXPECT_EQ(media_.loaded_flags, kcpc::kSnapshotFlagStayPaused);
  expect_event(*controller, Opcode::kEvtReset, frame);

  exchange(*controller, Opcode::kSnapshotLoad, 6,
           snapshot_load_payload("notes.txt"), frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  EXPECT_NE(frame.message().find("SNA"), std::string::npos);
}

TEST_F(KcpcMediaTest, ConfigurationIsStagedAndApplied) {
  start_with_media();
  kcpc::HelloReply hello;
  auto controller = join(kcpc::Role::kController, hello);
  // Before anyone else joins: it is sent to every client connected by then.
  Frame frame;
  expect_event(*controller, Opcode::kEvtControlChanged, frame);
  auto watcher = join(kcpc::Role::kObserver, hello);
  subscribe(*controller, 2, kcpc::kEventClassMachine);
  subscribe(*watcher, 2, kcpc::kEventClassMachine);

  auto get = [&](TestClient& client, std::uint32_t id) {
    exchange(client, Opcode::kConfigGet, id, text_payload("model"), frame);
    EXPECT_EQ(frame.status(), Status::kOk);
    kcpc::Reader r(frame.payload);
    r.skip(4);
    kcpc::ConfigValue value;
    EXPECT_TRUE(kcpc::read_config_value(r, value));
    EXPECT_EQ(r.remaining(), 0U);
    return value;
  };
  kcpc::ConfigValue value = get(*watcher, 3);
  EXPECT_EQ(value.value, "2");
  EXPECT_FALSE(value.has_pending);
  EXPECT_TRUE(value.pending.empty()) << "empty unless one is staged";

  exchange(*controller, Opcode::kConfigSet, 3, config_set_payload("model", "3"),
           frame);
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), 4U + 4U);
  EXPECT_EQ(frame.payload[4],
            static_cast<std::uint8_t>(kcpc::ConfigEffect::kStaged));
  value = get(*watcher, 4);
  EXPECT_EQ(value.value, "2");
  EXPECT_TRUE(value.has_pending);
  EXPECT_EQ(value.pending, "3");

  exchange(*controller, Opcode::kConfigSet, 4, config_set_payload("model", "9"),
           frame);
  EXPECT_EQ(frame.status(), Status::kBadPayload);
  EXPECT_EQ(frame.message(), "model must be 0-3");
  exchange(*controller, Opcode::kConfigSet, 5,
           config_set_payload("colour", "1"), frame);
  EXPECT_EQ(frame.status(), Status::kNotFound);
  exchange(*watcher, Opcode::kConfigGet, 5, text_payload("colour"), frame);
  EXPECT_EQ(frame.status(), Status::kNotFound);
  exchange(*watcher, Opcode::kConfigGet, 6, text_payload("fullscreen"), frame);
  EXPECT_EQ(frame.status(), Status::kNotPresent);

  ASSERT_TRUE(controller->request(Opcode::kConfigApply, 6, {}));
  ASSERT_TRUE(controller->receive_any(frame));
  EXPECT_EQ(frame.header.kind, static_cast<std::uint8_t>(kcpc::Kind::kResponse))
      << "the event overtook the response";
  ASSERT_EQ(frame.status(), Status::kOk);
  ASSERT_EQ(frame.payload.size(), 4U + 12U) << "a stamp";
  for (TestClient* client : {controller.get(), watcher.get()}) {
    expect_event(*client, Opcode::kEvtMachineChanged, frame);
    EXPECT_EQ(frame.payload[0], 3) << "the model of the rebuilt machine";
  }
  value = get(*watcher, 7);
  EXPECT_EQ(value.value, "3");
  EXPECT_FALSE(value.has_pending);

  media_.apply_status = Status::kInternal;
  exchange(*controller, Opcode::kConfigApply, 7, {}, frame);
  EXPECT_EQ(frame.status(), Status::kInternal);
  EXPECT_EQ(frame.message(), "the machine could not be rebuilt (code 7)");
}
