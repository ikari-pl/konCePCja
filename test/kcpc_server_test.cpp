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
#include <string>
#include <thread>
#include <vector>

#include "kcpc/kcpc_proto.h"
#include "net_socket.h"

namespace {

using Clock = std::chrono::steady_clock;
using kcpc::Opcode;
using kcpc::Status;

// Away from 6545 so a running emulator is never in the way, with a wide probe
// range so two test_runners at once (another worktree) do not collide.
constexpr int kTestBasePort = 47545;
constexpr int kTestProbeCount = 40;
constexpr int kReplyTimeoutMs = 5000;

kcpc::MachineInfo fake_machine() {
  kcpc::MachineInfo m;
  m.model = 2;
  m.crtc_type = 1;
  m.effective_tier = 0;
  m.flags = kcpc::kMachineFlagHeadless;
  m.ram_bytes = 128U * 1024U;
  m.rom_slots = 0x81;
  m.stamp = {1234, false};
  return m;
}

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

// A machine with something to read: 64 KiB in which no two neighbouring bytes
// are alike, registers that are all different, and a record of what the
// server asked for.
struct FakeReads {
  static constexpr std::uint32_t kSize = 0x10000;

  FakeReads() : memory(kSize) {
    for (std::uint32_t a = 0; a < kSize; ++a) {
      memory[a] = static_cast<std::uint8_t>((a * 7) ^ (a >> 8));
    }
  }

  static kcpc::Regs regs() {
    kcpc::Regs g;
    g.af = 0x0102;
    g.bc = 0x0304;
    g.de = 0x0506;
    g.hl = 0x0708;
    g.ix = 0x090A;
    g.iy = 0x0B0C;
    g.sp = 0x0D0E;
    g.pc = 0x0F10;
    g.af_alt = 0x1112;
    g.bc_alt = 0x1314;
    g.de_alt = 0x1516;
    g.hl_alt = 0x1718;
    g.i = 0x19;
    g.r = 0x1A;
    g.iff1 = 1;
    g.iff2 = 0;
    g.im = 2;
    g.halt = 1;
    return g;
  }

  void attach(kcpc::ServerConfig& c) {
    c.regs_get = [this](kcpc::Stamp& stamp, kcpc::Regs& out) {
      if (refuse != Status::kOk) return refuse.load();
      stamp = {77, true};
      out = regs();
      return Status::kOk;
    };
    c.mem_info = [this](kcpc::MemInfo& info) {
      if (refuse != Status::kOk) return refuse.load();
      info.ram_bytes = kSize;
      info.ram_config = 0xC4;
      info.rom_config = 0x89;
      info.upper_rom = 7;
      info.rom_slots = 0x80;
      info.stamp = {78, false};
      return Status::kOk;
    };
    c.mem_read = [this](const kcpc::MemReadRequest& request, kcpc::Stamp& stamp,
                        std::uint8_t* dst) {
      mem_reads.fetch_add(1);
      last = request;
      if (refuse != Status::kOk) return refuse.load();
      if (!kcpc::mem_range_ok(request.addr, request.len, kSize)) {
        return Status::kOutOfRange;
      }
      std::copy_n(memory.begin() + request.addr, request.len, dst);
      stamp = {79, false};
      return Status::kOk;
    };
  }

  std::vector<std::uint8_t> memory;
  std::atomic<Status> refuse{Status::kOk};
  std::atomic<int> mem_reads{0};
  kcpc::MemReadRequest last;  // written on the one reader thread of a test
};

struct Frame {
  kcpc::FrameHeader header;
  std::vector<std::uint8_t> payload;

  Status status() const {
    kcpc::Reader r(payload);
    std::uint16_t s = 0xFFFF;
    r.u16(s);
    return static_cast<Status>(s);
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

  // One whole frame, or false on timeout, close or a malformed header.
  bool receive(Frame& out, int timeout_ms = kReplyTimeoutMs) {
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
  EXPECT_EQ(reply.caps, 0U) << "this machine offers no reads";
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

// Roles arrive with CONTROL in a later slice: asking for control in HELLO is
// valid and is answered with the role actually granted.
TEST_F(KcpcServerTest, EveryClientStartsAsAnObserver) {
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
  EXPECT_EQ(hello_.caps, kcpc::kCapMachineRead | kcpc::kCapMemRom);
  EXPECT_EQ(hello_.caps, kcpc::kServerCaps);
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
