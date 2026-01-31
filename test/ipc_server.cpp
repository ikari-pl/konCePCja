#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "cap32.h"
#include "koncepcja_ipc_server.h"
#include "z80.h"

extern t_z80regs z80;
extern t_CPC CPC;
extern SDL_Surface* back_surface;
extern byte *membank_read[4];
extern byte *membank_write[4];

namespace {

constexpr int kPort = 6543;
constexpr size_t kBankSize = 16 * 1024;

std::string send_command(const std::string& command) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);
  if (fd < 0) return "";

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  bool connected = false;
  for (int attempt = 0; attempt < 50 && !connected; attempt++) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(connected);
  if (!connected) {
    ::close(fd);
    return "";
  }

  std::string line = command + "\n";
  ssize_t written = ::write(fd, line.data(), line.size());
  EXPECT_EQ(written, static_cast<ssize_t>(line.size()));

  std::string response;
  char buffer[256];
  ssize_t n = 0;
  while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
    response.append(buffer, buffer + n);
  }
  ::close(fd);
  return response;
}

class IpcServerTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    CPC.snd_enabled = 0;
    server.start();
  }

  static void TearDownTestSuite() {
    server.stop();
  }

  void SetUp() override {
    z80 = t_z80regs();
    z80_clear_breakpoints();
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
  EXPECT_EQ(resp, "OK\n");
  EXPECT_EQ(z80.AF.b.h, 0x42);

  resp = send_command("reg set PC 0x1234");
  EXPECT_EQ(resp, "OK\n");
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

TEST_F(IpcServerTest, BreakpointListAddDelClear) {
  auto resp = send_command("bp clear");
  EXPECT_EQ(resp, "OK\n");

  resp = send_command("bp add 0x1234");
  EXPECT_EQ(resp, "OK\n");

  resp = send_command("bp add 0x4000");
  EXPECT_EQ(resp, "OK\n");

  resp = send_command("bp list");
  EXPECT_EQ(resp, "OK count=2 1234 4000\n");

  resp = send_command("bp del 0x1234");
  EXPECT_EQ(resp, "OK\n");

  resp = send_command("bp list");
  EXPECT_EQ(resp, "OK count=1 4000\n");

  resp = send_command("bp clear");
  EXPECT_EQ(resp, "OK\n");

  resp = send_command("bp list");
  EXPECT_EQ(resp, "OK count=0\n");
}

TEST_F(IpcServerTest, WaitPcReturnsImmediatelyWhenMatched) {
  z80.PC.w.l = 0x2000;
  auto resp = send_command("wait pc 0x2000 50");
  EXPECT_EQ(resp, "OK\n");
}

TEST_F(IpcServerTest, WaitMemHonorsMask) {
  z80_write_mem(0x1000, 0xA5);
  auto resp = send_command("wait mem 0x1000 0xA0 mask=0xF0 50");
  EXPECT_EQ(resp, "OK\n");
}

TEST_F(IpcServerTest, WaitVblCompletes) {
  auto resp = send_command("wait vbl 1 100");
  EXPECT_EQ(resp, "OK\n");
}

TEST_F(IpcServerTest, ScreenshotReturnsErrorWithoutSurface) {
  back_surface = nullptr;
  auto resp = send_command("screenshot /tmp/kaprys_test.png");
  EXPECT_EQ(resp, "ERR 503 no-surface\n");
}

}  // namespace
