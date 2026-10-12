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

// A machine that can be written: it records what the server handed it.
struct FakeWrites {
  void attach(kcpc::ServerConfig& c) {
    c.regs_set = [this](std::uint32_t mask, const kcpc::Regs& values,
                        kcpc::Stamp& stamp, kcpc::Regs& after) {
      regs_sets.fetch_add(1);
      if (refuse != Status::kOk) return refuse.load();
      last_mask = mask;
      last_regs = values;
      stamp = {90, true};
      after = FakeReads::regs();
      after.hl = values.hl;
      return Status::kOk;
    };
    c.mem_write = [this](const kcpc::MemWriteRequest& request,
                         const std::uint8_t* data, std::size_t len,
                         kcpc::Stamp& stamp) {
      mem_writes.fetch_add(1);
      if (refuse != Status::kOk) return refuse.load();
      if (!kcpc::mem_range_ok(request.addr, static_cast<std::uint32_t>(len),
                              FakeReads::kSize)) {
        return Status::kOutOfRange;
      }
      last_write = request;
      written.assign(data, data + len);
      stamp = {91, false};
      return Status::kOk;
    };
  }

  std::atomic<Status> refuse{Status::kOk};
  std::atomic<int> regs_sets{0};
  std::atomic<int> mem_writes{0};
  // Written on the controller's reader thread, read after its reply.
  std::uint32_t last_mask = 0;
  kcpc::Regs last_regs;
  kcpc::MemWriteRequest last_write;
  std::vector<std::uint8_t> written;
};

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
  EXPECT_EQ(observer_hello.caps, kcpc::kServerCaps & ~kcpc::kCapRunControl)
      << "everything but run control, which this machine does not offer";
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

// Run control that records what the server handed it. A request can be made
// to post an event that names it before it returns, as the real machine does
// under the pause mutex.
struct FakeRun {
  void attach(kcpc::ServerConfig& c, kcpc::Server& server) {
    c.run_state = [this](kcpc::Stamp& stamp, bool& walk_active) {
      stamp = {55, paused.load()};
      walk_active = walking.load();
      return Status::kOk;
    };
    c.pause = [this, &server](const kcpc::Cause& cause, kcpc::Stamp& stamp) {
      pauses.fetch_add(1);
      last_cause = cause;
      if (refuse != Status::kOk) return refuse.load();
      paused = true;
      if (post_from_request) {
        kcpc::Event e;
        e.opcode = Opcode::kEvtStopped;
        e.frame = 56;
        e.reason = static_cast<std::uint8_t>(kcpc::StopReason::kPaused);
        e.pc = 0x1234;
        e.cause_client = cause.client;
        e.cause_request = cause.request;
        server.post(e);
        // Long enough for the fan-out thread to have the event at the
        // connection before the response exists.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      stamp = {56, true};
      return Status::kOk;
    };
    c.run = [this](const kcpc::Cause& cause, kcpc::Stamp& stamp) {
      runs.fetch_add(1);
      last_cause = cause;
      if (refuse != Status::kOk) return refuse.load();
      paused = false;
      stamp = {57, false};
      return Status::kOk;
    };
    c.reset = [this](const kcpc::Cause& cause, bool stay_paused,
                     kcpc::Stamp& stamp) {
      resets.fetch_add(1);
      last_cause = cause;
      last_stay_paused = stay_paused;
      if (refuse != Status::kOk) return refuse.load();
      stamp = {58, stay_paused};
      return Status::kOk;
    };
    c.step = [this](const kcpc::Cause& cause, const kcpc::StepRequest& step) {
      steps.fetch_add(1);
      last_cause = cause;
      last_step = step;
      return refuse.load();
    };
  }

  std::atomic<Status> refuse{Status::kOk};
  std::atomic<bool> paused{false};
  std::atomic<bool> walking{false};
  std::atomic<bool> post_from_request{false};
  std::atomic<int> pauses{0};
  std::atomic<int> runs{0};
  std::atomic<int> resets{0};
  std::atomic<int> steps{0};
  // Written on the controller's reader thread, read after its reply.
  kcpc::Cause last_cause;
  bool last_stay_paused = false;
  kcpc::StepRequest last_step;
};

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
  EXPECT_EQ(hello.caps, kcpc::kServerCaps);
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
  auto watcher = join(kcpc::Role::kObserver, hello);
  subscribe(*controller, 2, kcpc::kEventClassRun);
  subscribe(*watcher, 2, kcpc::kEventClassRun);
  Frame frame;
  expect_event(*controller, Opcode::kEvtControlChanged, frame);

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
  auto other = join(kcpc::Role::kObserver, other_hello);
  Frame frame;
  expect_event(*stepper, Opcode::kEvtControlChanged, frame);

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
