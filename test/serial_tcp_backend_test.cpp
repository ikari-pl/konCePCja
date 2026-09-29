/* TcpSocketBackend against a real localhost peer (beads-1af).
 *
 * open() starts a non-blocking connect(). It used to report "connected" the
 * moment connect() returned EINPROGRESS, so a refused connect looked live and
 * every byte sent afterwards vanished. These tests pin the resolved states:
 * a listening peer connects and carries bytes both ways and its hang-up is
 * noticed; a closed port never reports connected and send() fails.
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

#include "serial_interface.h"

#ifdef _WIN32
using test_sock_t = SOCKET;
constexpr test_sock_t kBadSock = INVALID_SOCKET;
using test_socklen_t = int;
namespace {
void test_sock_close(test_sock_t s) { closesocket(s); }
struct WsaInit {
  WsaInit() {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
  }
  ~WsaInit() { WSACleanup(); }
} g_wsa_init;
}  // namespace
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using test_sock_t = int;
constexpr test_sock_t kBadSock = -1;
using test_socklen_t = socklen_t;
namespace {
void test_sock_close(test_sock_t s) { ::close(s); }
}  // namespace
#endif

namespace {

// A 127.0.0.1 socket bound to an ephemeral port; `port` is what the kernel
// picked. With listen=false the port is reserved but nothing accepts on it.
struct LocalSocket {
  test_sock_t fd = kBadSock;
  uint16_t port = 0;

  explicit LocalSocket(bool listen) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kBadSock) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    test_socklen_t len = sizeof(addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
        (listen && ::listen(fd, 1) != 0)) {
      close();
      return;
    }
    port = ntohs(addr.sin_port);
  }
  ~LocalSocket() { close(); }
  void close() {
    if (fd != kBadSock) test_sock_close(fd);
    fd = kBadSock;
  }
};

// Poll `cond` until it holds or `ms` elapse.
bool eventually(const std::function<bool()>& cond, int ms = 3000) {
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return cond();
}

}  // namespace

TEST(TcpSocketBackendLive, ConnectsToListenerAndCarriesBytesBothWays) {
  LocalSocket server(/*listen=*/true);
  ASSERT_NE(server.fd, kBadSock);
  ASSERT_NE(server.port, 0);

  TcpSocketBackend client("127.0.0.1", server.port);
  ASSERT_TRUE(client.open());
  // The handshake completes against the listen backlog, before accept().
  ASSERT_TRUE(eventually([&] { return client.connected(); }));
  EXPECT_NE(client.status().find("Connected to"), std::string::npos);

  test_sock_t const peer = ::accept(server.fd, nullptr, nullptr);
  ASSERT_NE(peer, kBadSock);

  // Client -> peer.
  EXPECT_TRUE(client.send(0x42));
  char got = 0;
  ASSERT_EQ(::recv(peer, &got, 1, 0), 1);
  EXPECT_EQ(static_cast<uint8_t>(got), 0x42);

  // Peer -> client.
  char const out = 'Z';
  ASSERT_EQ(::send(peer, &out, 1, 0), 1);
  ASSERT_TRUE(eventually([&] { return client.has_data(); }));
  EXPECT_EQ(client.recv(), 'Z');

  // The peer hangs up: the client must notice instead of reporting a live
  // line forever.
  test_sock_close(peer);
  ASSERT_TRUE(eventually([&] {
    client.has_data();
    return !client.connected();
  }));
  EXPECT_FALSE(client.send(0x43));
  EXPECT_NE(client.status().find("Disconnected"), std::string::npos);
}

TEST(TcpSocketBackendLive, ClosedPortNeverReportsConnected) {
  uint16_t port = 0;
  {
    LocalSocket reserved(/*listen=*/false);
    ASSERT_NE(reserved.fd, kBadSock);
    port = reserved.port;
  }  // closed: nothing listens on `port` now

  TcpSocketBackend client("127.0.0.1", port);
  // A local refusal usually lands inside open()'s short wait (false); on
  // Windows it can take longer, so open() may return true while the connect
  // is still in flight. Either way it must never become connected.
  bool const opened = client.open();
  EXPECT_FALSE(client.connected());
  EXPECT_FALSE(client.send(0x42));
  if (opened) {
    ASSERT_TRUE(eventually(
        [&] {
          EXPECT_FALSE(client.connected());
          return !client.is_open();
        },
        5000));
  }
  EXPECT_FALSE(client.is_open());
  EXPECT_FALSE(client.connected());
  EXPECT_FALSE(client.send(0x42));
  EXPECT_FALSE(client.has_data());
  EXPECT_NE(client.status().find("Disconnected"), std::string::npos);
}
