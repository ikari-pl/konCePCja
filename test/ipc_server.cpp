#include <gtest/gtest.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "autotype.h"
#include "cpc_key_tables.h"
#include "imgui_state.h"
#include "koncepcja.h"
#include "koncepcja_ipc_server.h"
#include "symfile.h"
#include "video_host.h"
#include "z80_view.h"

extern t_z80regs z80;
extern t_CPC CPC;
extern t_GateArray GateArray;
extern t_CRTC CRTC;
extern SDL_Surface* back_surface;
extern byte* membank_read[4];
extern byte* membank_write[4];
extern video_plugin* vid_plugin;

namespace {

constexpr size_t kBankSize = 16 * 1024;

// Debug-surface commands now return "OK [context]\n" instead of bare "OK\n".
// This helper checks that a response starts with "OK " or is exactly "OK\n".
// Bind the command's result to a variable so it is evaluated ONCE — evaluating
// the argument inline would re-run the command when the reply is a bare "OK\n"
// (the `.substr(0,3) == "OK "` branch fails, so `||` re-evaluates), which
// double-dispatches non-idempotent commands (e.g. 'input type' enqueues twice).
#define EXPECT_OK(resp)                                               \
  do {                                                                \
    const std::string ok_resp_ = (resp);                              \
    EXPECT_TRUE(ok_resp_.substr(0, 3) == "OK " || ok_resp_ == "OK\n") \
        << "Expected OK response, got: " << ok_resp_;                 \
  } while (0)

// Forward-declare the server so send_command can query its actual port
static KoncepcjaIpcServer* g_test_server = nullptr;

std::string send_command(const std::string& command) {
  // Always the test server's own port, never a literal.  The server scans
  // 6543..6552 and publishes whichever it got, so a default of 6543 would
  // send test traffic to a koncepcja instance the developer happens to be
  // running -- which answers, so the test misbehaves instead of failing.
  if (g_test_server == nullptr) {
    ADD_FAILURE() << "send_command called with no test server running";
    return "";
  }
  int const port = g_test_server->port();
  if (port <= 0) {
    ADD_FAILURE() << "test IPC server has not bound a port";
    return "";
  }
#ifdef _WIN32
  SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  EXPECT_NE(fd, INVALID_SOCKET);
  if (fd == INVALID_SOCKET) return "";
#else
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);
  if (fd < 0) return "";
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  bool connected = false;
  for (int attempt = 0; attempt < 100 && !connected; attempt++) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(connected);
  if (!connected) {
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
    return "";
  }

  std::string line = command + "\n";
#ifdef _WIN32
  int written = send(fd, line.data(), static_cast<int>(line.size()), 0);
  EXPECT_EQ(written, static_cast<int>(line.size()));
  // Half-close: signal no more data so the persistent server closes its end
  shutdown(fd, SD_SEND);
#else
  ssize_t written = ::write(fd, line.data(), line.size());
  EXPECT_EQ(written, static_cast<ssize_t>(line.size()));
  // Half-close: signal no more data so the persistent server closes its end
  shutdown(fd, SHUT_WR);
#endif

  std::string response;
  char buffer[256];
#ifdef _WIN32
  int n = 0;
  while ((n = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
    response.append(buffer, buffer + n);
  }
  closesocket(fd);
#else
  ssize_t n = 0;
  while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
    response.append(buffer, buffer + n);
  }
  ::close(fd);
#endif
  return response;
}

class IpcServerTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    CPC.snd_enabled = 0;
    g_test_server = &server;
    server.start();
    // Wait for the listener to publish the port it actually bound, rather
    // than assuming a fixed delay is enough.  With a koncepcja instance
    // already holding 6543/6544 the server scans further up the range, so
    // the bind takes longer than it does on an idle machine.
    int port = 0;
    for (int i = 0; i < 200 && port <= 0; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      port = server.port();
    }
    ASSERT_GT(port, 0) << "test IPC server never bound a port";
  }

  static void TearDownTestSuite() {
    server.stop();
    g_test_server = nullptr;
#ifdef _WIN32
    WSACleanup();
#endif
  }

  void SetUp() override {
    z80 = t_z80regs();
    z80_clear_breakpoints();
    z80_clear_watchpoints();
    g_symfile.clear();
    for (int i = 0; i < 4; i++) {
      std::memset(memory[i], 0, kBankSize);
      membank_read[i] = memory[i];
      membank_write[i] = memory[i];
    }
  }

  static KoncepcjaIpcServer server;
  static byte memory[4][kBankSize];
};

KoncepcjaIpcServer IpcServerTest::server;
byte IpcServerTest::memory[4][kBankSize];

TEST_F(IpcServerTest, RegSetUpdatesRegisters) {
  auto resp = send_command("reg set A 0x42");
  EXPECT_OK(resp);
  EXPECT_EQ(z80.AF.b.h, 0x42);

  resp = send_command("reg set PC 0x1234");
  EXPECT_OK(resp);
  EXPECT_EQ(z80.PC.w.l, 0x1234);
}

TEST_F(IpcServerTest, RegGetReturnsValues) {
  z80.AF.b.h = 0x77;
  z80.PC.w.l = 0x3456;

  auto resp = send_command("reg get A");
  EXPECT_EQ(resp, "OK 77\n");

  resp = send_command("reg get PC");
  EXPECT_EQ(resp, "OK 3456\n");
}

TEST_F(IpcServerTest, HelpRegistryMatchesImplementedCommands) {
  auto help = send_command("help");
  EXPECT_OK(help);
  EXPECT_NE(help.find("tier [get|status]"), std::string::npos) << help;

  EXPECT_OK(send_command("help reg"));
  EXPECT_OK(send_command("help serial"));
  EXPECT_OK(send_command("help telnet"));
}

TEST_F(IpcServerTest, RegisteredStatusCommandsHaveProtocolResponses) {
  auto const tier = send_command("tier");
  EXPECT_EQ(tier, "ERR 503 no-board\n");

  auto const telnet = send_command("telnet status");
  EXPECT_OK(telnet);
  EXPECT_NE(telnet.find("port="), std::string::npos);
  EXPECT_NE(telnet.find("client="), std::string::npos);
}

TEST_F(IpcServerTest, DevtoolsOnOffUsesRequestedState) {
  imgui_state.show_devtools = true;
  EXPECT_OK(send_command("devtools off"));
  EXPECT_FALSE(imgui_state.show_devtools);

  EXPECT_OK(send_command("devtools on"));
  EXPECT_TRUE(imgui_state.show_devtools);
}

// R52 in the crtc dump is the Gate Array's HSYNC line counter, the reference a
// raster effect is timed against.  It reported CRTC.reg5 instead -- a value
// the same line already prints as R5 -- so the field read plausibly while
// describing a different register.  Give the two sources distinct values so
// the wrong one cannot pass.
TEST_F(IpcServerTest, RegsCrtcReportsGateArrayR52NotCrtcReg5) {
  GateArray.sl_count = 0x2A;
  CRTC.registers[5] = 0x13;
  CRTC.reg5 = 0x13;

  auto const resp = send_command("regs crtc");
  ASSERT_NE(resp.find("R52="), std::string::npos) << resp;
  EXPECT_NE(resp.find("R52=2A"), std::string::npos)
      << "R52 must report GateArray.sl_count; got: " << resp;
  EXPECT_EQ(resp.find("R52=13"), std::string::npos)
      << "R52 is reporting CRTC register 5: " << resp;
  // R5 keeps reporting the CRTC register, so the two stay distinguishable.
  EXPECT_NE(resp.find("R5=13"), std::string::npos) << resp;
}

TEST_F(IpcServerTest, BreakpointListAddDelClear) {
  auto resp = send_command("bp clear");
  EXPECT_OK(resp);

  resp = send_command("bp add 0x1234");
  EXPECT_OK(resp);

  resp = send_command("bp add 0x4000");
  EXPECT_OK(resp);

  resp = send_command("bp list");
  EXPECT_EQ(resp, "OK count=2 1234 4000\n");

  resp = send_command("bp del 0x1234");
  EXPECT_OK(resp);

  resp = send_command("bp list");
  EXPECT_EQ(resp, "OK count=1 4000\n");

  resp = send_command("bp clear");
  EXPECT_OK(resp);

  resp = send_command("bp list");
  EXPECT_EQ(resp, "OK count=0\n");
}

TEST_F(IpcServerTest, WaitPcReturnsImmediatelyWhenMatched) {
  z80.PC.w.l = 0x2000;
  auto resp = send_command("wait pc 0x2000 50");
  EXPECT_OK(resp);
}

TEST_F(IpcServerTest, WaitMemHonorsMask) {
  z80_write_mem(0x1000, 0xA5);
  auto resp = send_command("wait mem 0x1000 0xA0 mask=0xF0 50");
  EXPECT_OK(resp);
}

TEST_F(IpcServerTest, WaitVblCompletes) {
  auto resp = send_command("wait vbl 1 100");
  EXPECT_OK(resp);
}

TEST_F(IpcServerTest, StaleBreakpointStopCannotOvertakeResume) {
  uint16_t hit_pc = 0;
  bool watch = false;
  server.consume_breakpoint_hit(hit_pc, watch);  // discard any earlier hit

  cpc_resume();
  uint64_t const stale_epoch = cpc_resume_epoch();
  // A genuine pause->run transition is what invalidates a staged stop --
  // cpc_resume() is a no-op (does not bump the epoch) when the machine is
  // already running, so a real intervening pause is needed here to make
  // this "the user's later Run", not a redundant no-op resume.
  cpc_pause();
  cpc_resume();

  uint64_t const generation = z80_breakpoint_generation();
  EXPECT_FALSE(
      cpc_commit_breakpoint_stop(stale_epoch, generation, 0x1234, false));
  EXPECT_FALSE(CPC.paused);
  EXPECT_FALSE(server.consume_breakpoint_hit(hit_pc, watch));

  uint64_t const current_epoch = cpc_resume_epoch();
  EXPECT_TRUE(
      cpc_commit_breakpoint_stop(current_epoch, generation, 0x5678, false));
  EXPECT_TRUE(CPC.paused);
  EXPECT_TRUE(server.consume_breakpoint_hit(hit_pc, watch));
  EXPECT_EQ(hit_pc, 0x5678);
  EXPECT_FALSE(watch);
}

TEST_F(IpcServerTest, RedundantResumeCannotDiscardAPendingStop) {
  // The bug this guards: a resume issued while the machine is ALREADY
  // running used to still bump the epoch, so a client (or another thread)
  // sending an idempotent `run` in the narrow window between a real
  // breakpoint being classified and debug_sync committing it would
  // silently discard that hit -- the machine never actually paused, and
  // the caller who armed the breakpoint got no report at all. cpc_resume()
  // must be a true no-op when CPC.paused is already false.
  uint16_t hit_pc = 0;
  bool watch = false;
  server.consume_breakpoint_hit(hit_pc, watch);

  cpc_pause();
  uint64_t const epoch = cpc_resume();  // real transition: paused -> running
  uint64_t const generation = z80_breakpoint_generation();

  // A second, redundant `run` arrives while the machine is already running
  // (e.g. a client that isn't tracking pause state, or two racing callers).
  uint64_t const redundant_epoch = cpc_resume();
  EXPECT_EQ(redundant_epoch, epoch)
      << "a resume on an already-running machine must not advance the epoch";

  // The hit staged against the FIRST (and only real) epoch must still commit.
  EXPECT_TRUE(cpc_commit_breakpoint_stop(epoch, generation, 0x1234, false));
  EXPECT_TRUE(CPC.paused);
  EXPECT_TRUE(server.consume_breakpoint_hit(hit_pc, watch));
  EXPECT_EQ(hit_pc, 0x1234);
}

TEST_F(IpcServerTest, BreakpointMutationInvalidatesClassifiedStop) {
  uint16_t hit_pc = 0;
  bool watch = false;
  server.consume_breakpoint_hit(hit_pc, watch);

  uint64_t const epoch = cpc_resume();
  uint64_t const old_generation = z80_breakpoint_generation();
  z80_add_breakpoint(0x1234);

  EXPECT_FALSE(
      cpc_commit_breakpoint_stop(epoch, old_generation, 0x1234, false));
  EXPECT_FALSE(CPC.paused);
  EXPECT_FALSE(server.consume_breakpoint_hit(hit_pc, watch));
  z80_clear_breakpoints();
}

TEST_F(IpcServerTest, ResumeCannotInterleaveWithExecutionEpochStamp) {
  // Establish a known paused state so the background thread's cpc_resume()
  // below is a real pause->run transition and therefore does bump the
  // epoch -- cpc_resume() is a no-op on an already-running machine.
  cpc_pause();
  std::atomic<bool> resume_started{false};
  std::atomic<bool> resume_finished{false};
  std::thread resume_thread;
  uint64_t stamped_epoch = 0;
  uint64_t stamped_generation = 0;

  {
    CpcStopCoordinationGuard const coordination;
    stamped_epoch = coordination.resume_epoch();
    stamped_generation = coordination.breakpoint_generation();
    resume_thread = std::thread([&]() {
      resume_started.store(true, std::memory_order_release);
      cpc_resume();
      resume_finished.store(true, std::memory_order_release);
    });
    while (!resume_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(resume_finished.load(std::memory_order_acquire));
  }

  resume_thread.join();
  EXPECT_TRUE(resume_finished.load(std::memory_order_acquire));
  EXPECT_FALSE(cpc_commit_breakpoint_stop(stamped_epoch, stamped_generation,
                                          0x1234, false));
}

TEST_F(IpcServerTest, BreakpointMutationCannotRaceStopCommit) {
  z80_clear_breakpoints();
  std::atomic<bool> mutation_started{false};
  std::atomic<bool> mutation_finished{false};
  std::thread mutation_thread;
  uint64_t const old_generation = z80_breakpoint_generation();

  {
    CpcStopCoordinationGuard const coordination;
    mutation_thread = std::thread([&]() {
      mutation_started.store(true, std::memory_order_release);
      z80_add_breakpoint(0x1234);
      mutation_finished.store(true, std::memory_order_release);
    });
    while (!mutation_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(mutation_finished.load(std::memory_order_acquire));
    EXPECT_EQ(coordination.breakpoint_generation(), old_generation);
  }

  mutation_thread.join();
  EXPECT_TRUE(mutation_finished.load(std::memory_order_acquire));
  EXPECT_GT(z80_breakpoint_generation(), old_generation);
  z80_clear_breakpoints();
}

TEST_F(IpcServerTest, PauseLeaseBlocksConcurrentResume) {
  // Destructive callers hold CpcPauseLease across their critical section.
  // A concurrent IPC/UI Run must not clear pause or bump the resume epoch
  // while that lease is alive — otherwise quiescence waits can hang and
  // teardown can race a restarted Z80 thread.
  cpc_resume();
  uint64_t const epoch_before = cpc_resume_epoch();

  std::atomic<bool> resume_started{false};
  std::atomic<bool> resume_finished{false};
  std::thread resume_thread;
  {
    CpcPauseLease lease;
    EXPECT_TRUE(CPC.paused);
    EXPECT_FALSE(lease.was_paused());

    resume_thread = std::thread([&]() {
      resume_started.store(true, std::memory_order_release);
      uint64_t const epoch = cpc_resume();
      EXPECT_EQ(epoch, epoch_before);
      resume_finished.store(true, std::memory_order_release);
    });
    while (!resume_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // Resume returns promptly (deferred no-op) but must not unpause.
    EXPECT_TRUE(resume_finished.load(std::memory_order_acquire));
    EXPECT_TRUE(CPC.paused);
    EXPECT_EQ(cpc_resume_epoch(), epoch_before);
  }

  resume_thread.join();
  EXPECT_TRUE(CPC.paused);
  EXPECT_EQ(cpc_resume_epoch(), epoch_before);

  uint64_t const epoch_after = cpc_resume();
  EXPECT_FALSE(CPC.paused);
  EXPECT_EQ(epoch_after, epoch_before + 1);
}

TEST_F(IpcServerTest, PauseLeaseProtectsQuiescenceWaitFromConcurrentResume) {
  // Simulate a non-quiescent Z80 thread while a lease waits. Concurrent
  // Resume must not clear pause — otherwise the wait would never observe
  // the paused branch and could spin forever.
  cpc_resume();
  g_z80_quiescent.store(false, std::memory_order_release);

  std::atomic<bool> lease_held{false};
  std::atomic<bool> allow_wait{false};
  std::atomic<bool> waiter_done{false};
  std::thread waiter([&]() {
    CpcPauseLease lease(CpcPauseLeaseMode::PauseOnly);
    lease_held.store(true, std::memory_order_release);
    while (!allow_wait.load(std::memory_order_acquire))
      std::this_thread::yield();
    lease.wait();
    waiter_done.store(true, std::memory_order_release);
  });

  while (!lease_held.load(std::memory_order_acquire)) std::this_thread::yield();
  EXPECT_TRUE(CPC.paused);

  uint64_t const epoch = cpc_resume_epoch();
  EXPECT_EQ(cpc_resume(), epoch);  // deferred while lease is held
  EXPECT_TRUE(CPC.paused);

  allow_wait.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_FALSE(waiter_done.load(std::memory_order_acquire));

  g_z80_quiescent.store(true, std::memory_order_release);
  waiter.join();
  EXPECT_TRUE(waiter_done.load(std::memory_order_acquire));
  EXPECT_TRUE(CPC.paused);
  EXPECT_EQ(cpc_resume_epoch(), epoch);
}

TEST_F(IpcServerTest, NestedPauseLeasesKeepResumeDeferred) {
  cpc_resume();
  uint64_t const epoch = cpc_resume_epoch();
  {
    CpcPauseLease outer;
    {
      CpcPauseLease inner;
      EXPECT_TRUE(inner.was_paused());
      EXPECT_EQ(cpc_resume(), epoch);
      EXPECT_TRUE(CPC.paused);
    }
    // Outer lease still held — Resume remains deferred.
    EXPECT_EQ(cpc_resume(), epoch);
    EXPECT_TRUE(CPC.paused);
  }
  EXPECT_EQ(cpc_resume(), epoch + 1);
  EXPECT_FALSE(CPC.paused);
}

TEST_F(IpcServerTest, ScreenshotReturnsErrorWithoutSurface) {
  back_surface = nullptr;
  auto screenshotPath =
      (std::filesystem::temp_directory_path() / "kaprys_test.png").string();
  auto resp = send_command("screenshot " + screenshotPath);
  EXPECT_EQ(resp, "ERR 503 no-surface\n");
}

TEST_F(IpcServerTest, DiskNewCanCreateFluxBacking) {
  auto const path =
      std::filesystem::temp_directory_path() / "koncepcja-ipc-flux.scp";
  std::filesystem::remove(path);

  auto const response =
      send_command("disk new " + path.string() + " data flux");
  EXPECT_OK(response);

  std::ifstream file(path, std::ios::binary);
  char signature[3] = {};
  file.read(signature, sizeof(signature));
  EXPECT_EQ(std::string(signature, sizeof(signature)), "SCP");
  file.close();
  std::filesystem::remove(path);
}

TEST_F(IpcServerTest, WatchpointAddListDelClear) {
  auto resp = send_command("wp clear");
  EXPECT_OK(resp);

  resp = send_command("wp add 0x4000 256 w");
  EXPECT_OK(resp);

  resp = send_command("wp add 0xC000 1 rw");
  EXPECT_OK(resp);

  resp = send_command("wp list");
  EXPECT_TRUE(resp.find("count=2") != std::string::npos);
  EXPECT_TRUE(resp.find("4000+256/w") != std::string::npos);
  EXPECT_TRUE(resp.find("C000+1/rw") != std::string::npos);

  resp = send_command("wp del 0");
  EXPECT_OK(resp);

  resp = send_command("wp list");
  EXPECT_TRUE(resp.find("count=1") != std::string::npos);

  resp = send_command("wp clear");
  EXPECT_OK(resp);

  resp = send_command("wp list");
  EXPECT_TRUE(resp.find("count=0") != std::string::npos);
}

TEST_F(IpcServerTest, WatchpointConditional) {
  auto resp = send_command("wp add 0x4000 1 w if value > 128");
  EXPECT_OK(resp);

  resp = send_command("wp list");
  EXPECT_TRUE(resp.find("if value > 128") != std::string::npos);

  send_command("wp clear");
}

TEST_F(IpcServerTest, SymbolAddLookupDel) {
  auto resp = send_command("sym add 0x0038 interrupt_handler");
  EXPECT_OK(resp);

  resp = send_command("sym lookup 0x0038");
  EXPECT_EQ(resp, "OK interrupt_handler\n");

  resp = send_command("sym lookup interrupt_handler");
  EXPECT_EQ(resp, "OK 0038\n");

  resp = send_command("sym list");
  EXPECT_TRUE(resp.find("count=1") != std::string::npos);
  EXPECT_TRUE(resp.find("0038 interrupt_handler") != std::string::npos);

  resp = send_command("sym del interrupt_handler");
  EXPECT_OK(resp);

  resp = send_command("sym lookup interrupt_handler");
  EXPECT_EQ(resp, "ERR 404 not-found\n");
}

TEST_F(IpcServerTest, DisasmWithSymbols) {
  // Add a symbol, then disassemble with --symbols
  send_command("sym add 0x0000 entry_point");
  auto resp = send_command("disasm 0x0000 1 --symbols");
  EXPECT_TRUE(resp.find("OK") != std::string::npos);
  EXPECT_TRUE(resp.find("entry_point") != std::string::npos);
}

TEST_F(IpcServerTest, MemFindHex) {
  // Write a known pattern at 0x1000
  send_command("mem write 0x1000 DEADBEEF");
  auto resp = send_command("mem find hex 0x0000 0xFFFF DEADBEEF");
  EXPECT_TRUE(resp.find("OK") != std::string::npos);
  EXPECT_TRUE(resp.find("1000") != std::string::npos);
}

TEST_F(IpcServerTest, MemFindText) {
  // Write ASCII text at 0x2000
  send_command("mem write 0x2000 48454C4C4F");  // "HELLO"
  auto resp = send_command("mem find text 0x0000 0xFFFF HELLO");
  EXPECT_TRUE(resp.find("OK") != std::string::npos);
  EXPECT_TRUE(resp.find("2000") != std::string::npos);
}

TEST_F(IpcServerTest, StackCommand) {
  z80.SP.w.l = 0xBFFA;
  // Write some values on the stack
  z80_write_mem(0xBFFA, 0x34);
  z80_write_mem(0xBFFB, 0x12);
  auto resp = send_command("stack 4");
  EXPECT_TRUE(resp.find("OK") != std::string::npos);
  EXPECT_TRUE(resp.find("depth=4") != std::string::npos);
  EXPECT_TRUE(resp.find("1234") != std::string::npos);
}

TEST_F(IpcServerTest, StepOverDoesNotDescendIntoCall) {
  // This is a basic check that the command is accepted
  // (full behavioral test requires a running emulator)
  z80.PC.w.l = 0x0000;
  // Write NOP (0x00) at address 0
  z80_write_mem(0x0000, 0x00);
  auto resp = send_command("step over");
  EXPECT_OK(resp);
}

TEST_F(IpcServerTest, StepToCommand) {
  // Write NOP at 0x0000, step to 0x0001 should work immediately via ephemeral
  // bp
  z80.PC.w.l = 0x0000;
  z80_write_mem(0x0000, 0x00);
  // step to on a paused emulator won't actually run; check command is accepted
  // In test environment without main loop, this will timeout
  // Just verify the command doesn't crash
  auto resp = send_command("step to 0x0001");
  // Either timeout or OK is acceptable in test harness
  EXPECT_TRUE(resp.find("OK") != std::string::npos ||
              resp.find("ERR 408") != std::string::npos);
}

TEST_F(IpcServerTest, WatchpointRange) {
  send_command("wp clear");

  // Add a range watchpoint covering 16 bytes
  auto resp = send_command("wp add 0x4000 16 rw");
  EXPECT_OK(resp);

  resp = send_command("wp list");
  EXPECT_TRUE(resp.find("count=1") != std::string::npos);
  EXPECT_TRUE(resp.find("4000+16/rw") != std::string::npos);

  send_command("wp clear");
}

TEST_F(IpcServerTest, StepOutCommand) {
  // Without a live Machine, z80_step_instruction is a no-op, so step out hits
  // the 5s deadline. This only checks the command is wired and does not crash;
  // SP-climb / CALL-skip behaviour is covered by the IPC harness on a running
  // emulator (see PR #37).
  z80.PC.w.l = 0x0000;
  z80_write_mem(0x0000, 0xC9);  // RET

  auto resp = send_command("step out");
  EXPECT_TRUE(resp.find("OK") != std::string::npos ||
              resp.find("ERR 408") != std::string::npos);
}

TEST_F(IpcServerTest, SymbolLoad) {
  // Create a minimal .sym file in the platform temp directory
  auto sympath = std::filesystem::temp_directory_path() / "koncepcja_test.sym";
  std::string symfile = sympath.string();
  {
    std::ofstream ofs(symfile);
    ofs << "; test symbols\n"
        << "al $0038 .interrupt_handler\n"
        << "al $0000 .reset_vector\n"
        << "al $FC00 .screen_base\n";
  }

  auto resp = send_command("sym load " + symfile);
  EXPECT_TRUE(resp.find("OK loaded=3") != std::string::npos);

  // Verify loaded symbols are queryable
  resp = send_command("sym lookup 0x0038");
  EXPECT_EQ(resp, "OK interrupt_handler\n");

  resp = send_command("sym lookup screen_base");
  EXPECT_EQ(resp, "OK FC00\n");

  std::filesystem::remove(sympath);
}

TEST_F(IpcServerTest, MemFindWildcard) {
  // Write a pattern at a known address: DE ?? BE EF
  z80_write_mem(0x3000, 0xDE);
  z80_write_mem(0x3001, 0x42);  // any value
  z80_write_mem(0x3002, 0xBE);
  z80_write_mem(0x3003, 0xEF);

  auto resp = send_command("mem find hex 0x2F00 0x3100 DE??BEEF");
  EXPECT_TRUE(resp.find("OK") != std::string::npos);
  EXPECT_TRUE(resp.find("3000") != std::string::npos);
}

// Bank 0 READ aims at a ROM overlay while WRITE stays on RAM — the asymmetry
// that makes --view=ram distinguishable from the default CPU view. Mirrors
// DevToolsRenderTest::CpuViewAndRamViewDivergeUnderARomOverlay (&1AF1).
TEST_F(IpcServerTest, MemRamViewIgnoresRomOverlay) {
  static byte rom[kBankSize];
  std::memset(rom, 0, sizeof(rom));
  constexpr word kAddr = 0x1AF1;
  constexpr byte kRomByte = 0x3E;
  constexpr byte kRamByte = 0x03;
  rom[kAddr] = kRomByte;
  memory[0][kAddr] = kRamByte;
  membank_read[0] = rom;

  auto resp = send_command("mem read 0x1AF1 1");
  EXPECT_EQ(resp, "OK 3E\n") << resp;

  resp = send_command("mem read 0x1AF1 1 --view=ram");
  EXPECT_EQ(resp, "OK 03\n") << resp;

  resp = send_command("mem read 0x1AF1 1 --view=write");
  EXPECT_EQ(resp, "OK 03\n") << resp;

  resp = send_command("mem read 0x1AF1 1 --view=bogus");
  EXPECT_EQ(resp, "ERR 400 bad-view (read|ram)\n") << resp;

  // Reference byte at 0x4000 matches RAM under the overlay.
  memory[1][0x0000] = kRamByte;  // 0x4000
  resp = send_command("mem compare 0x1AF1 0x4000 1");
  EXPECT_TRUE(resp.find("OK diffs=1") != std::string::npos) << resp;

  resp = send_command("mem compare 0x1AF1 0x4000 1 --view=ram");
  EXPECT_TRUE(resp.find("OK diffs=0") != std::string::npos) << resp;

  resp = send_command("mem compare 0x1AF1 0x4000 1 --view=bogus");
  EXPECT_EQ(resp, "ERR 400 bad-view (read|ram)\n") << resp;

  // CPU view cannot find the RAM byte under ROM; RAM view can.
  resp = send_command("mem find hex 0x1AF0 0x1AF2 03");
  EXPECT_EQ(resp, "OK\n") << resp;

  resp = send_command("mem find hex 0x1AF0 0x1AF2 03 --view=ram");
  EXPECT_TRUE(resp.find("1AF1") != std::string::npos) << resp;

  resp = send_command("search hex 03 --view=ram");
  EXPECT_TRUE(resp.find("1AF1") != std::string::npos) << resp;

  resp = send_command("search hex 03 --view=bogus");
  EXPECT_EQ(resp, "ERR 400 bad-view (read|ram)\n") << resp;
}

// ─────────────────────────────────────────────────
// Error message quality tests
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, UnknownCommandReturns404WithSuggestion) {
  auto resp = send_command("totype hello");
  EXPECT_TRUE(resp.find("ERR 404") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("autotype") != std::string::npos)
      << resp;  // "Did you mean..."
}

TEST_F(IpcServerTest, UnknownCommandReturns404NoSuggestionForGarbage) {
  auto resp = send_command("xyzzyplugh");
  EXPECT_TRUE(resp.find("ERR 404") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("not recognized") != std::string::npos) << resp;
}

TEST_F(IpcServerTest, BareHexAddressAccepted) {
  // "C000" should be parsed as hex, not rejected
  auto resp = send_command("bp add C000");
  EXPECT_OK(resp);
  auto list = send_command("bp list");
  EXPECT_TRUE(list.find("C000") != std::string::npos) << list;
}

TEST_F(IpcServerTest, BpWithNoArgsReturnsUsage) {
  auto resp = send_command("bp");
  EXPECT_TRUE(resp.find("ERR 400") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("usage:") != std::string::npos) << resp;
}

TEST_F(IpcServerTest, BpAddWithNoAddrReturnsUsage) {
  auto resp = send_command("bp add");
  EXPECT_TRUE(resp.find("ERR 400") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("usage:") != std::string::npos) << resp;
}

TEST_F(IpcServerTest, WpWithNoArgsReturnsUsage) {
  auto resp = send_command("wp");
  EXPECT_TRUE(resp.find("ERR 400") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("usage:") != std::string::npos) << resp;
}

TEST_F(IpcServerTest, MemWithNoArgsReturnsUsage) {
  auto resp = send_command("mem");
  EXPECT_TRUE(resp.find("ERR 400") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("usage:") != std::string::npos) << resp;
}

TEST_F(IpcServerTest, BadNumberIncludesValueInError) {
  auto resp = send_command("reg set A notanumber");
  EXPECT_TRUE(resp.find("ERR 400") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("notanumber") != std::string::npos) << resp;
}

// ─────────────────────────────────────────────────
// Key modifiers (IPC Phase 3, beads-nz0n): the chord parser in cpc_key_tables.h
// tokenizes "CTRL+SHIFT+ESC" and maps modifiers to scancode high-byte flags.
// (The atomic tap itself runs only in the live emulator — the frame-stepped
// hold blocks here, so the contract is covered by ipc_harness instead.)
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, ChordParsingHelpers) {
  EXPECT_EQ(cpc_chord_tokens("CTRL+SHIFT+ESC"),
            (std::vector<std::string>{"CTRL", "SHIFT", "ESC"}));
  EXPECT_EQ(cpc_chord_tokens("ESC"), (std::vector<std::string>{"ESC"}));
  EXPECT_EQ(cpc_chord_tokens("SHIFT+A"),
            (std::vector<std::string>{"SHIFT", "A"}));

  // Modifier mapping is case-insensitive; non-modifiers map to 0.
  EXPECT_EQ(cpc_modifier_flag("CTRL"), MOD_CPC_CTRL);
  EXPECT_EQ(cpc_modifier_flag("control"), MOD_CPC_CTRL);
  EXPECT_EQ(cpc_modifier_flag("SHIFT"), MOD_CPC_SHIFT);
  EXPECT_EQ(cpc_modifier_flag("lshift"), MOD_CPC_SHIFT);
  EXPECT_EQ(cpc_modifier_flag("RSHIFT"), MOD_CPC_SHIFT);
  EXPECT_EQ(cpc_modifier_flag("ESC"), 0);
  EXPECT_EQ(cpc_modifier_flag("FOO"), 0);
}

// ─────────────────────────────────────────────────
// Type unification (IPC Phase 4, beads-c8fn): 'input type' routes through
// g_autotype_queue, so a WinAPE ~KEY~ token parses to ONE key action — not
// the literal characters of its name (the old per-char path).
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, TypeRoutesThroughAutotypeQueue) {
  // ~RETURN~ must become ONE RETURN-key action, not the letters R,E,T,U,R,N.
  g_autotype_queue.clear();
  EXPECT_OK(send_command("input type \"~RETURN~\""));
  auto acts = g_autotype_queue.actions();
  ASSERT_EQ(acts.size(), 1u) << "a ~RETURN~ token must parse to one key action";
  EXPECT_EQ(acts[0].type, AutoTypeAction::CHAR_PRESS_RELEASE);
  EXPECT_EQ(acts[0].cpc_key, static_cast<uint16_t>(CPC_RETURN))
      << "~RETURN~ parses to the RETURN key, not its literal characters";

  // Plain text still types one action per mappable character.
  g_autotype_queue.clear();
  EXPECT_OK(send_command("input type \"hi\""));
  auto acts2 = g_autotype_queue.actions();
  EXPECT_EQ(acts2.size(), 2u) << "plain text types one action per character";

  // Surrounding SINGLE quotes are stripped too: `input type '~RETURN~'` must
  // parse to the same one key action.
  g_autotype_queue.clear();
  EXPECT_OK(send_command("input type '~RETURN~'"));
  auto acts3 = g_autotype_queue.actions();
  ASSERT_EQ(acts3.size(), 1u) << "surrounding single quotes must be stripped";
  EXPECT_EQ(acts3[0].cpc_key, static_cast<uint16_t>(CPC_RETURN));
  g_autotype_queue.clear();
}

// ─────────────────────────────────────────────────
// Light-gun (IPC Phase 2, beads-vrsr): the staged 'input gun' commands are
// flushed by ipc_drain_input() into CPC.phazer_* exactly as the SDL path does.
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, GunDrainAppliesAimAndTrigger) {
  // Stub the video plugin so the drain's window->CPC mapping is known:
  // phazer_x = (x - x_offset) * x_scale, phazer_y = (y - y_offset) * y_scale.
  static video_plugin stub{};
  stub.x_offset = 10;
  stub.y_offset = 20;
  stub.x_scale = 2.0f;
  stub.y_scale = 4.0f;
  video_plugin* const saved = vid_plugin;
  vid_plugin = &stub;

  CPC.phazer_emulation = PhazerType::AmstradMagnumPhaser;
  CPC.phazer_x = 0;
  CPC.phazer_y = 0;
  CPC.phazer_pressed = false;

  // Publish the device gate (the no-gun 409 would otherwise fire), then stage.
  ipc_drain_input();
  EXPECT_OK(send_command("input gun move 30 40"));
  EXPECT_OK(send_command("input gun trigger down"));
  ipc_drain_input();

  EXPECT_EQ(CPC.phazer_x, static_cast<unsigned int>((30 - 10) * 2.0f));
  EXPECT_EQ(CPC.phazer_y, static_cast<unsigned int>((40 - 20) * 4.0f));
  EXPECT_TRUE(CPC.phazer_pressed);

  EXPECT_OK(send_command("input gun trigger up"));
  ipc_drain_input();
  EXPECT_FALSE(CPC.phazer_pressed);

  vid_plugin = saved;
  CPC.phazer_emulation = PhazerType::None;
}

// ─────────────────────────────────────────────────
// Status-bracket environment hint (beads-7hqp): the bracket decodes the
// CP/M-vs-BASIC discriminator (screen mode + RAM bank) into a readable 'env'
// field, so a reader doesn't have to remember the encoding (a bare 'reg get
// PC' carries no such context). Heuristic only — ambiguous combinations must
// report '?', not a guess.
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, StatusBracketInfersEnvironment) {
  const unsigned int saved_mode = GateArray.scr_mode;
  const unsigned char saved_ram = GateArray.RAM_config;

  // CP/M Plus boots 80-col (mode 2) on RAM bank 1.
  GateArray.scr_mode = 2;
  GateArray.RAM_config = 1;
  auto resp = send_command("pause");
  EXPECT_NE(resp.find("env:CP/M"), std::string::npos) << resp;

  // BASIC/AMSDOS sits in mode 1 on bank 0.
  GateArray.scr_mode = 1;
  GateArray.RAM_config = 0;
  resp = send_command("pause");
  EXPECT_NE(resp.find("env:BASIC"), std::string::npos) << resp;

  // A mixed state (mode 2 but bank 0) is unknown — reported, not guessed.
  GateArray.scr_mode = 2;
  GateArray.RAM_config = 0;
  resp = send_command("pause");
  EXPECT_NE(resp.find("env:?"), std::string::npos) << resp;

  GateArray.scr_mode = saved_mode;
  GateArray.RAM_config = saved_ram;
}

// ─────────────────────────────────────────────────
// Null-InputMapper guard (beads-p95s): the IPC server accepts connections
// before koncpc_main constructs CPC.InputMapper, so key/joy injection arriving
// in that window must return 503 — not dereference a null InputMapper (the
// SIGSEGV-at-0x48 this guard prevents). IpcServerTest runs with no InputMapper,
// which is exactly that window.
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, KeyJoyInjectionRequiresInputMapper) {
  InputMapper* const saved = CPC.InputMapper;
  CPC.InputMapper = nullptr;  // the early-boot window

  // Every key/joy sub-command resolves names through CPC.InputMapper; with it
  // null they must be rejected, not crash.
  for (const char* cmd :
       {"input key RETURN", "input keydown A", "input keyup A",
        "input chord SHIFT+A", "input joy 0 U", "input joy 1 F1"}) {
    EXPECT_EQ(send_command(cmd), "ERR 503 emulator-not-ready\n") << cmd;
  }

  // The guard is scoped to key/joy injection: 'type' only queues text (OK)
  // and 'state' has its own InputMapper guard (503) — neither over-blocked.
  EXPECT_OK(send_command("input type \"hi\""));
  g_autotype_queue.clear();
  EXPECT_EQ(send_command("input state"), "ERR 503 emulator-not-ready\n");

  CPC.InputMapper = saved;
}

}  // namespace
