#pragma once

// konCePCja — the wire loops the KCPC tests speak the protocol with, on the
// socket shim the server and the client use (net_socket.h): one copy, shared
// by kcpc_server_test.cpp's TestClient and kcpc_client_test.cpp's SilentPeer
// (beads-3sp.12). Blocking with a deadline, one frame at a time; nothing
// here knows the server or the client.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "kcpc/kcpc_proto.h"
#include "net_socket.h"

namespace kcpc_test {

// How long a wait sleeps before it looks at the deadline again.
constexpr int kWireSliceMs = 20;

// Writes `bytes` whole by `timeout_ms`; false when the socket took them
// only in part, failed, or ran out of time.
inline bool send_all(net::Socket socket, const std::vector<std::uint8_t>& bytes,
                     int timeout_ms) {
  std::size_t sent = 0;
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (sent < bytes.size()) {
    net::IoResult const r =
        net::send_some(socket, bytes.data() + sent, bytes.size() - sent);
    if (r.status == net::IoStatus::kOk) {
      sent += r.bytes;
    } else if (r.status == net::IoStatus::kWouldBlock) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      net::wait_writable(socket, kWireSliceMs);
    } else {
      return false;
    }
  }
  return true;
}

// Reads exactly `size` bytes by `timeout_ms`; false on timeout, end of
// stream or failure.
inline bool read_exact(net::Socket socket, std::uint8_t* buf, std::size_t size,
                       int timeout_ms) {
  std::size_t got = 0;
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (got < size) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    if (!net::wait_readable(socket, kWireSliceMs)) continue;
    net::IoResult const r = net::recv_some(socket, buf + got, size - got);
    if (r.status == net::IoStatus::kOk) {
      got += r.bytes;
    } else if (r.status != net::IoStatus::kWouldBlock) {
      return false;
    }
  }
  return true;
}

// One whole frame of any kind, its payload bounded by `max_payload`; false
// on timeout, end of stream or a header this side refuses.
inline bool read_frame(net::Socket socket, kcpc::FrameHeader& header,
                       std::vector<std::uint8_t>& payload,
                       std::uint32_t max_payload, int timeout_ms) {
  std::uint8_t raw[kcpc::kHeaderSize];
  if (!read_exact(socket, raw, sizeof raw, timeout_ms)) return false;
  if (kcpc::parse_header(raw, max_payload, header) != kcpc::HeaderError::kOk) {
    return false;
  }
  payload.resize(header.length);
  return read_exact(socket, payload.data(), payload.size(), timeout_ms);
}

// What reading until the end of the stream found.
enum class StreamEnd : std::uint8_t {
  kClosed,   // the peer half-closed or closed: end of stream
  kData,     // a byte arrived instead (only with `discard_data` false)
  kError,    // the socket failed
  kTimeout,  // nothing by the deadline
};

// Reads until the peer closes. With `discard_data` the bytes that arrive
// meanwhile are thrown away; without it the first byte ends the wait with
// kData.
inline StreamEnd read_end(net::Socket socket, int timeout_ms,
                          bool discard_data) {
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::uint8_t sink[4096];
  while (std::chrono::steady_clock::now() < deadline) {
    if (!net::wait_readable(socket, kWireSliceMs)) continue;
    net::IoResult const r = net::recv_some(socket, sink, sizeof sink);
    if (r.status == net::IoStatus::kClosed) return StreamEnd::kClosed;
    if (r.status == net::IoStatus::kError) return StreamEnd::kError;
    if (r.status == net::IoStatus::kOk && !discard_data) {
      return StreamEnd::kData;
    }
  }
  return StreamEnd::kTimeout;
}

}  // namespace kcpc_test
