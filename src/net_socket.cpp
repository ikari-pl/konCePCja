#include "net_socket.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace net {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr int kSendFlags = 0;

NativeSocket native(Socket s) { return static_cast<NativeSocket>(s); }
Socket wrap(NativeSocket s) {
  return s == INVALID_SOCKET ? kInvalidSocket : static_cast<Socket>(s);
}
bool would_block() {
  int const e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINTR;
}
bool connect_pending() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool set_nonblocking(NativeSocket s) {
  u_long on = 1;
  return ioctlsocket(s, FIONBIO, &on) == 0;
}
void no_sigpipe(NativeSocket /*s*/) {}
void close_native(NativeSocket s) { closesocket(s); }

// SO_REUSEADDR means something else on Windows: it lets a second socket bind
// a port that is already being listened on, so two cores would share a port
// and the probe for the next free one would never move on. SO_EXCLUSIVEADDRUSE
// is the option that gives the POSIX behaviour.
void set_listen_options(NativeSocket s) {
  BOOL on = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
             reinterpret_cast<const char*>(&on), sizeof(on));
}

// 1 ready, 0 timeout. An error counts as ready: the following recv/send
// reports it.
bool wait_for(NativeSocket s, bool for_write, int timeout_ms) {
  fd_set set;
  fd_set failed;
  FD_ZERO(&set);
  FD_ZERO(&failed);
  FD_SET(s, &set);
  FD_SET(s, &failed);
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  int const r = select(0, for_write ? nullptr : &set,
                       for_write ? &set : nullptr, &failed, &tv);
  return r != 0;
}
#else
using NativeSocket = int;
#ifdef __APPLE__
constexpr int kSendFlags = 0;  // no MSG_NOSIGNAL; SO_NOSIGPIPE per socket
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

NativeSocket native(Socket s) { return static_cast<NativeSocket>(s); }
Socket wrap(NativeSocket s) {
  return s < 0 ? kInvalidSocket : static_cast<Socket>(s);
}
bool would_block() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
bool connect_pending() { return errno == EINPROGRESS || errno == EINTR; }
bool set_nonblocking(NativeSocket s) {
  int const flags = fcntl(s, F_GETFL, 0);
  return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
// A send() to a peer that went away must fail with EPIPE, not kill the
// emulator with SIGPIPE.
void no_sigpipe(NativeSocket s) {
#ifdef __APPLE__
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)s;
#endif
}
void close_native(NativeSocket s) { ::close(s); }

// Lets a restarted core bind its port again while old connections linger in
// TIME_WAIT. On POSIX it does not let two sockets listen on one port.
void set_listen_options(NativeSocket s) {
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}

// poll(), not select(): select() is undefined for descriptors >= FD_SETSIZE
// and the GUI process holds plenty.
bool wait_for(NativeSocket s, bool for_write, int timeout_ms) {
  pollfd p{};
  p.fd = s;
  p.events = for_write ? POLLOUT : POLLIN;
  int const r = ::poll(&p, 1, timeout_ms);
  if (r < 0) return errno != EINTR;
  return r > 0;
}
#endif

void no_delay(NativeSocket s) {
  // Frames are small and answered one for one; do not let Nagle hold a reply
  // back waiting for an ACK.
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
             sizeof(one));
}

sockaddr_in loopback(int port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  return addr;
}

}  // namespace

bool startup() {
#ifdef _WIN32
  WSADATA wsa;
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
  return true;
#endif
}

void cleanup() {
#ifdef _WIN32
  WSACleanup();
#endif
}

Socket listen_loopback(int port, int backlog) {
  if (port <= 0 || port > 65535) return kInvalidSocket;
  NativeSocket const s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (wrap(s) == kInvalidSocket) return kInvalidSocket;
  set_listen_options(s);
  sockaddr_in const addr = loopback(port);
  if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(s, backlog) != 0 || !set_nonblocking(s)) {
    close_native(s);
    return kInvalidSocket;
  }
  return wrap(s);
}

Socket accept_client(Socket listener) {
  NativeSocket const s = ::accept(native(listener), nullptr, nullptr);
  if (wrap(s) == kInvalidSocket) return kInvalidSocket;
  if (!set_nonblocking(s)) {
    close_native(s);
    return kInvalidSocket;
  }
  no_sigpipe(s);
  no_delay(s);
  return wrap(s);
}

Socket connect_loopback(int port, int timeout_ms) {
  if (port <= 0 || port > 65535) return kInvalidSocket;
  NativeSocket const s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (wrap(s) == kInvalidSocket) return kInvalidSocket;
  if (!set_nonblocking(s)) {
    close_native(s);
    return kInvalidSocket;
  }
  no_sigpipe(s);
  no_delay(s);
  sockaddr_in const addr = loopback(port);
  if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    bool connected = false;
    if (connect_pending() && wait_for(s, true, timeout_ms)) {
      int err = 0;
#ifdef _WIN32
      int len = sizeof(err);
#else
      socklen_t len = sizeof(err);
#endif
      connected = getsockopt(s, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char*>(&err), &len) == 0 &&
                  err == 0;
    }
    if (!connected) {
      close_native(s);
      return kInvalidSocket;
    }
  }
  return wrap(s);
}

bool wait_readable(Socket s, int timeout_ms) {
  return wait_for(native(s), false, timeout_ms);
}

bool wait_writable(Socket s, int timeout_ms) {
  return wait_for(native(s), true, timeout_ms);
}

IoResult recv_some(Socket s, std::uint8_t* buf, std::size_t size) {
#ifdef _WIN32
  int const n = ::recv(native(s), reinterpret_cast<char*>(buf),
                       static_cast<int>(size), 0);
#else
  ssize_t const n = ::recv(native(s), buf, size, 0);
#endif
  if (n > 0) return {IoStatus::kOk, static_cast<std::size_t>(n)};
  if (n == 0) return {IoStatus::kClosed, 0};
  return {would_block() ? IoStatus::kWouldBlock : IoStatus::kError, 0};
}

IoResult send_some(Socket s, const std::uint8_t* buf, std::size_t size) {
#ifdef _WIN32
  int const n = ::send(native(s), reinterpret_cast<const char*>(buf),
                       static_cast<int>(size), kSendFlags);
#else
  ssize_t const n = ::send(native(s), buf, size, kSendFlags);
#endif
  if (n > 0) return {IoStatus::kOk, static_cast<std::size_t>(n)};
  return {would_block() ? IoStatus::kWouldBlock : IoStatus::kError, 0};
}

void shutdown_send(Socket s) {
#ifdef _WIN32
  ::shutdown(native(s), SD_SEND);
#else
  ::shutdown(native(s), SHUT_WR);
#endif
}

void close_socket(Socket s) {
  if (s != kInvalidSocket) close_native(native(s));
}

}  // namespace net
