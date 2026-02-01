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
  addr.sin_port = htons(kPort);
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
#else
  ssize_t written = ::write(fd, line.data(), line.size());
  EXPECT_EQ(written, static_cast<ssize_t>(line.size()));
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
    server.start();
    // Give the listener thread time to bind and listen
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  static void TearDownTestSuite() {
    server.stop();
#ifdef _WIN32
    WSACleanup();
#endif
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
