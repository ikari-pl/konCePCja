#pragma once

// A thin shim over BSD sockets and Winsock for loopback TCP.
//
// This is the one place with `#ifdef _WIN32` around sockets for the code that
// uses it (the KCPC binary server; decision D5: std::thread and sockets, no
// SDL). It carries no policy: no threads, no framing, no logging.
//
// Client sockets are non-blocking. Every call that could wait takes a timeout
// instead, so a thread using this shim can always notice a stop flag: there is
// no blocking recv() or send() that only the peer can end.

#include <cstddef>
#include <cstdint>

namespace net {

// Wide enough for a POSIX fd and for a Winsock SOCKET (a UINT_PTR).
using Socket = std::intptr_t;
inline constexpr Socket kInvalidSocket = -1;

// Process-wide socket setup (WSAStartup on Windows, nothing elsewhere). Calls
// nest: each successful startup() needs one cleanup().
bool startup();
void cleanup();

// A listening socket bound to 127.0.0.1:port, or kInvalidSocket when the port
// is taken or anything else fails. Never binds another address. The socket is
// non-blocking, so accept_client() cannot hang on a connection that was reset
// between wait_readable() and accept().
Socket listen_loopback(int port, int backlog);

// The next pending connection as a non-blocking socket that will not raise
// SIGPIPE, or kInvalidSocket when none is pending.
Socket accept_client(Socket listener);

// A non-blocking client socket connected to 127.0.0.1:port, or
// kInvalidSocket. Waits up to timeout_ms for the connection to complete.
Socket connect_loopback(int port, int timeout_ms);

// True when the socket has something to read or accept, or has been closed or
// failed (so the next recv_some() reports it); false on timeout.
bool wait_readable(Socket s, int timeout_ms);
// True when the socket can take more bytes or has failed; false on timeout.
bool wait_writable(Socket s, int timeout_ms);

enum class IoStatus : std::uint8_t {
  kOk,          // `bytes` > 0 were transferred
  kWouldBlock,  // nothing transferred; wait and call again
  kClosed,      // the peer closed its side (recv only)
  kError,       // the connection is unusable
};

struct IoResult {
  IoStatus status = IoStatus::kError;
  std::size_t bytes = 0;
};

IoResult recv_some(Socket s, std::uint8_t* buf, std::size_t size);
IoResult send_some(Socket s, const std::uint8_t* buf, std::size_t size);

// Sends FIN: the peer reads end-of-stream once it has read everything already
// sent, while this side can still read. Closing a socket that has unread
// input makes the stack send a reset instead, which can destroy data the peer
// has not read yet; half-closing first and reading until the peer closes is
// how a last message is delivered reliably.
void shutdown_send(Socket s);

void close_socket(Socket s);

}  // namespace net
