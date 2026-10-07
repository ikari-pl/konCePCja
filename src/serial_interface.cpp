/* konCePCja — Serial Interface Implementation */

// winsock2.h MUST come before any other include on Windows.
// <filesystem> (and other standard headers) drag in <windows.h> which includes
// <winsock.h> (v1). If winsock2.h arrives later, its guards prevent the v2
// definitions from being re-processed, leaving the TU with v1 structs and
// corrupt Winsock runtime state in Debug builds.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0  // socket is already non-blocking; flag 0 is equivalent
#endif
typedef int ssize_t;
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

#include "bounded_deadline.h"
#include "log.h"
#include "plotter.h"
#include "serial_interface.h"
#include "subcycle_bridge.h"
#include "z80_view.h"

// File Backend Implementation
FileBackend::FileBackend(std::string input_path, std::string output_path)
    : input_path_(std::move(input_path)), output_path_(std::move(output_path)) {
  // With no files, backend is still "open" (like NullBackend)
  if (input_path_.empty() && output_path_.empty()) {
    open_ = true;
  }
}

bool FileBackend::open() {
  tx_error_logged_ = false;  // a reopened file gets its own report
  if (!input_path_.empty()) {
    input_file_ = fopen(input_path_.c_str(), "rb");
    if (!input_file_) return false;
  }

  if (!output_path_.empty()) {
    output_file_ = fopen(output_path_.c_str(), "wb");
    if (!output_file_) {
      if (input_file_) fclose(input_file_);
      return false;
    }
  }

  open_ = true;
  return true;
}

void FileBackend::close() {
  if (input_file_) {
    if (fclose(input_file_) != 0)
      LOG_ERROR("Serial file backend: closing " << input_path_ << " failed");
    input_file_ = nullptr;
  }
  if (output_file_) {
    // fclose flushes: a disk that filled up while buffered bytes were still
    // in flight loses them here, and nowhere else would say so.
    if (fclose(output_file_) != 0)
      LOG_ERROR("Serial file backend: closing " << output_path_
                                                << " failed; output may be "
                                                   "incomplete");
    output_file_ = nullptr;
  }
  open_ = false;
}

bool FileBackend::send(uint8_t byte) {
  if (!output_file_) return false;
  bool const ok = fputc(byte, output_file_) != EOF && fflush(output_file_) == 0;
  if (!ok && !tx_error_logged_) {
    // Once, not per byte: a full disk would otherwise flood the log.
    LOG_ERROR("Serial file backend: cannot write " << output_path_);
    tx_error_logged_ = true;
  }
  return ok;
}

bool FileBackend::has_data() const {
  if (!input_file_) return false;
  if (lookahead_ >= 0) return true;
  int const ch = fgetc(input_file_);
  if (ch == EOF) return false;
  lookahead_ = ch;
  return true;
}

uint8_t FileBackend::recv() {
  if (lookahead_ >= 0) {
    uint8_t const byte = static_cast<uint8_t>(lookahead_);
    lookahead_ = -1;
    return byte;
  }
  if (!input_file_) return 0;
  int const ch = fgetc(input_file_);
  return (ch == EOF) ? 0 : static_cast<uint8_t>(ch);
}

std::string FileBackend::status() const {
  std::string s;
  if (!input_path_.empty()) {
    s += "Input: " + input_path_;
  }
  if (!output_path_.empty()) {
    if (!s.empty()) s += ", ";
    s += "Output: " + output_path_;
  }
  return s.empty() ? "No files configured" : s;
}

// Host Serial Backend Implementation (POSIX only — not available on Windows)
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

HostSerialBackend::HostSerialBackend(std::string device_path)
    : device_path_(std::move(device_path)), fd_(-1), original_flags_(0) {}

HostSerialBackend::~HostSerialBackend() { close(); }

bool HostSerialBackend::open() {
  if (fd_ >= 0) return true;

  fd_ = ::open(device_path_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) {
    return false;
  }

  struct termios tty;
  if (tcgetattr(fd_, &tty) == 0) {
    original_flags_ = fcntl(fd_, F_GETFL);

    cfmakeraw(&tty);
    cfsetispeed(&tty, B9600);
    cfsetospeed(&tty, B9600);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    tcsetattr(fd_, TCSANOW, &tty);
    fcntl(fd_, F_SETFL, O_RDWR | O_NONBLOCK);
  }

  return true;
}

void HostSerialBackend::close() {
  if (fd_ >= 0) {
    fcntl(fd_, F_SETFL, original_flags_);
    ::close(fd_);
    fd_ = -1;
  }
}

bool HostSerialBackend::send(uint8_t byte) {
  return fd_ >= 0 && ::write(fd_, &byte, 1) == 1;
}

bool HostSerialBackend::has_data() const {
  if (fd_ < 0) return false;
  int bytes_available = 0;
  ioctl(fd_, FIONREAD, &bytes_available);
  return bytes_available > 0;
}

uint8_t HostSerialBackend::recv() {
  if (fd_ < 0) return 0;
  uint8_t byte = 0;
  ssize_t const n = ::read(fd_, &byte, 1);
  return (n > 0) ? byte : 0;
}

bool HostSerialBackend::connected() const { return fd_ >= 0; }

std::string HostSerialBackend::status() const {
  if (fd_ >= 0) {
    return "Connected to " + device_path_;
  }
  return "Disconnected from " + device_path_;
}

std::vector<std::string> HostSerialBackend::list_ports() {
  std::vector<std::string> ports;
#ifdef __APPLE__
  DIR* dir = opendir("/dev");
  if (dir) {
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      std::string const name = entry->d_name;
      if (name.find("tty.") == 0 || name.find("cu.") == 0) {
        ports.push_back("/dev/" + name);
      }
    }
    closedir(dir);
  }
#elif defined(__linux__)
  DIR* dir = opendir("/dev");
  if (dir) {
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      std::string name = entry->d_name;
      if (name.find("ttyUSB") == 0 || name.find("ttyACM") == 0 ||
          name.find("ttyS") == 0) {
        ports.push_back("/dev/" + name);
      }
    }
    closedir(dir);
  }
#endif
  return ports;
}
#endif  // _WIN32

// Null Modem Backend Implementation
NullModemBackend::NullModemBackend() : open_(false), peer_(nullptr) {}

NullModemBackend::~NullModemBackend() { close(); }

bool NullModemBackend::open() {
  open_ = true;
  return true;
}

void NullModemBackend::close() {
  if (peer_) {
    if (peer_->peer_ == this) {
      {
        std::scoped_lock const lock(peer_->rx_mutex_);
        while (!peer_->rx_buffer_.empty()) peer_->rx_buffer_.pop();
      }
      peer_->peer_ = nullptr;
    }
    peer_ = nullptr;
  }
  {
    std::scoped_lock const lock(rx_mutex_);
    while (!rx_buffer_.empty()) rx_buffer_.pop();
  }
  open_ = false;
}

bool NullModemBackend::send(uint8_t byte) {
  if (!peer_ || !open_) return false;
  std::scoped_lock const lock(peer_->rx_mutex_);
  peer_->rx_buffer_.push(byte);
  return true;
}

bool NullModemBackend::has_data() const {
  std::scoped_lock const lock(rx_mutex_);
  return !rx_buffer_.empty();
}

uint8_t NullModemBackend::recv() {
  std::scoped_lock const lock(rx_mutex_);
  if (rx_buffer_.empty()) return 0;
  uint8_t const byte = rx_buffer_.front();
  rx_buffer_.pop();
  return byte;
}

std::string NullModemBackend::status() const {
  if (peer_) {
    return "Connected to peer";
  }
  return "Waiting for peer connection";
}

void NullModemBackend::connect_peer(NullModemBackend* peer) { peer_ = peer; }

void NullModemBackend::disconnect_peer() {
  {
    std::scoped_lock const lock(rx_mutex_);
    while (!rx_buffer_.empty()) rx_buffer_.pop();
  }
  if (peer_ && peer_->peer_ == this) {
    peer_->peer_ = nullptr;
  }
  peer_ = nullptr;
}

// TCP Socket Backend Implementation
#ifdef _WIN32
namespace {
inline void tcp_sock_close(int s) { closesocket(s); }
inline void tcp_set_nonblocking(int s) {
  u_long m = 1;
  ioctlsocket(s, FIONBIO, &m);
}
inline bool tcp_conn_inprogress(int err) { return err == WSAEWOULDBLOCK; }
inline bool tcp_would_block(int err) { return err == WSAEWOULDBLOCK; }
inline int tcp_last_error() { return WSAGetLastError(); }
inline void tcp_no_sigpipe(int /*s*/) {}
constexpr int kTcpSendFlags = 0;
// 1 = writable or failed (SO_ERROR tells which), 0 = still pending, -1 = error.
// Winsock reports a failed non-blocking connect in the except set, not the
// write set.
inline int tcp_wait_connect(int s, int timeout_ms) {
  fd_set wfds;
  fd_set efds;
  FD_ZERO(&wfds);
  FD_ZERO(&efds);
  FD_SET(static_cast<SOCKET>(s), &wfds);
  FD_SET(static_cast<SOCKET>(s), &efds);
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int const r = select(0, nullptr, &wfds, &efds, &tv);
  return r < 0 ? -1 : (r > 0 ? 1 : 0);
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
namespace {
inline void tcp_sock_close(int s) { ::close(s); }
inline void tcp_set_nonblocking(int s) {
  int const f = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, f | O_NONBLOCK);
}
inline bool tcp_conn_inprogress(int err) { return err == EINPROGRESS; }
inline bool tcp_would_block(int err) {
  return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}
inline int tcp_last_error() { return errno; }
// A send() to a peer that went away must fail with EPIPE, not kill the
// emulator with SIGPIPE.
#ifdef __APPLE__
inline void tcp_no_sigpipe(int s) {
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
}
constexpr int kTcpSendFlags = 0;
#else
inline void tcp_no_sigpipe(int /*s*/) {}
constexpr int kTcpSendFlags = MSG_NOSIGNAL;
#endif
// 1 = writable or failed (SO_ERROR tells which), 0 = still pending, -1 = error.
inline int tcp_wait_connect(int s, int timeout_ms) {
  pollfd p{};
  p.fd = s;
  p.events = POLLOUT;
  int const r = ::poll(&p, 1, timeout_ms);
  if (r < 0) return errno == EINTR ? 0 : -1;
  return r > 0 ? 1 : 0;
}
#endif
inline int tcp_socket_error(int s) {
  int err = 0;
#ifdef _WIN32
  int len = sizeof(err);
#else
  socklen_t len = sizeof(err);
#endif
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) <
      0)
    return tcp_last_error();
  return err;
}
}  // namespace

TcpSocketBackend::TcpSocketBackend(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

TcpSocketBackend::~TcpSocketBackend() { close(); }

bool TcpSocketBackend::open() {
  if (state_ != State::Disconnected) return true;
  if (sockfd_ >= 0) close();  // a connection that dropped: start over

  // getaddrinfo() is the one call left in this path with no timeout of its
  // own: a stale or unreachable resolver blocks for tens of seconds, and
  // open() runs on the main thread at startup and on the IPC thread from
  // `serial config set`. Bound it the way the connect step is bounded; a
  // lookup that answers too late frees its own result.
  struct addrinfo* result = nullptr;
  std::string const port_str = std::to_string(port_);
  DeadlineResult const resolved = run_with_deadline<struct addrinfo*>(
      [host = host_, port_str](struct addrinfo*& out) {
        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        out = nullptr;
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &out) != 0) {
          out = nullptr;  // undefined on failure, and nothing was allocated
          return false;
        }
        return out != nullptr;
      },
      [](struct addrinfo* late) { freeaddrinfo(late); }, kResolveWaitMs,
      result);
  if (resolved == DeadlineResult::TimedOut) {
    LOG_ERROR("Serial TCP: resolving " << host_ << " timed out after "
                                       << kResolveWaitMs << " ms");
    return false;
  }
  if (resolved != DeadlineResult::Ok) {
    LOG_ERROR("Serial TCP: cannot resolve " << host_);
    return false;
  }

  sockfd_ = static_cast<int>(
      socket(result->ai_family, result->ai_socktype, result->ai_protocol));
  if (sockfd_ < 0) {
    freeaddrinfo(result);
    return false;
  }
  tcp_set_nonblocking(sockfd_);
  tcp_no_sigpipe(sockfd_);

  int const rc =
      ::connect(sockfd_, result->ai_addr, static_cast<int>(result->ai_addrlen));
  int const err = rc < 0 ? tcp_last_error() : 0;
  freeaddrinfo(result);

  if (rc == 0) {
    state_ = State::Connected;
    return true;
  }
  if (!tcp_conn_inprogress(err)) {
    LOG_ERROR("Serial TCP: connect to " << host_ << ":" << port_
                                        << " failed (error " << err << ")");
    close();
    return false;
  }

  // The connect is in flight. A local or refused peer answers within the
  // short wait; a slow remote one stays Connecting and is resolved by the
  // next has_data()/send()/connected().
  state_ = State::Connecting;
  if (resolve_connect(kConnectWaitMs) == State::Disconnected) {
    close();
    return false;
  }
  return true;
}

TcpSocketBackend::State TcpSocketBackend::resolve_connect(
    int timeout_ms) const {
  if (state_ != State::Connecting) return state_;
  int const ready = tcp_wait_connect(sockfd_, timeout_ms);
  if (ready == 0) return State::Connecting;
  int const err = ready < 0 ? tcp_last_error() : tcp_socket_error(sockfd_);
  State const settled = err != 0 ? State::Disconnected : State::Connected;
  // The IPC thread (status/connected) and the Z80 thread (has_data/send) both
  // resolve the same socket: settle it once, and report what the winner
  // settled it to.
  State expected = State::Connecting;
  if (!state_.compare_exchange_strong(expected, settled)) return expected;
  if (err != 0)
    LOG_ERROR("Serial TCP: connect to " << host_ << ":" << port_
                                        << " failed (error " << err << ")");
  return settled;
}

void TcpSocketBackend::mark_disconnected() const {
  if (state_.exchange(State::Disconnected) != State::Disconnected)
    LOG_INFO("Serial TCP: " << host_ << ":" << port_ << " disconnected");
}

bool TcpSocketBackend::connected() const {
  return resolve_connect(0) == State::Connected;
}

void TcpSocketBackend::close() {
  if (sockfd_ >= 0) {
    tcp_sock_close(sockfd_);
    sockfd_ = -1;
  }
  {
    std::scoped_lock const lock(rx_mutex_);
    rx_buffer_.clear();
  }
  state_ = State::Disconnected;
}

bool TcpSocketBackend::send(uint8_t byte) {
  if (resolve_connect(0) != State::Connected) return false;
  auto const n =
      ::send(sockfd_, reinterpret_cast<const char*>(&byte), 1, kTcpSendFlags);
  if (n == 1) return true;
  // A full send buffer drops this byte but keeps the line up; anything else
  // (EPIPE, ECONNRESET, ...) means the peer is gone.
  if (n < 0 && !tcp_would_block(tcp_last_error())) mark_disconnected();
  return false;
}

bool TcpSocketBackend::has_data() const {
  if (resolve_connect(0) == State::Connected) {
    // Drain pending socket data into rx_buffer_ (the socket is non-blocking).
    uint8_t buf[256];
    for (;;) {
      auto const n = ::recv(sockfd_, reinterpret_cast<char*>(buf),
                            static_cast<int>(sizeof(buf)), MSG_DONTWAIT);
      if (n > 0) {
        std::scoped_lock const lock(rx_mutex_);
        rx_buffer_.insert(rx_buffer_.end(), buf, buf + n);
        continue;
      }
      // 0 = orderly shutdown by the peer; <0 other than would-block = reset.
      if (n == 0 || !tcp_would_block(tcp_last_error())) mark_disconnected();
      break;
    }
  }
  // Bytes that arrived before a disconnect are still delivered.
  std::scoped_lock const lock(rx_mutex_);
  return !rx_buffer_.empty();
}

uint8_t TcpSocketBackend::recv() {
  if (!has_data()) return 0;
  std::scoped_lock const lock(rx_mutex_);
  uint8_t const byte = rx_buffer_.front();
  rx_buffer_.erase(rx_buffer_.begin());
  return byte;
}

std::string TcpSocketBackend::status() const {
  resolve_connect(0);  // a paused machine never calls has_data()/send()
  std::string const where = host_ + ":" + std::to_string(port_);
  switch (state_.load()) {
    case State::Connected:
      return "Connected to " + where;
    case State::Connecting:
      return "Connecting to " + where;
    case State::Disconnected:
      break;
  }
  return "Disconnected from " + where;
}

// SI ROM Manager Implementation
SIRomManager::SIRomManager()
    : rom_slot_(DEFAULT_SLOT), loaded_(false), auto_loaded_(false) {}

// SI ROM image loaded from rom/serial.rom at runtime.
// Source: src/si_rom.asm — rebuild with:
//   z80asm -o src/si_rom.bin src/si_rom.asm
//   dd if=src/si_rom.bin of=rom/serial.rom bs=16384 count=1 conv=sync

void SIRomManager::load(byte** rom_map, const std::string& rom_path) {
  if (rom_slot_ < 0 || rom_slot_ >= 32) {
    return;
  }

  // Check if ROM already loaded in this slot
  if (rom_map[rom_slot_] != nullptr) {
    loaded_ = true;
    auto_loaded_ = false;
    return;
  }

  // Try loading SI ROM file
  static const char* const rom_names[] = {"serial.rom", "SERIAL.ROM",
                                          "si_rom.bin", "SI_ROM.BIN", nullptr};
  for (int i = 0; rom_names[i]; i++) {
    std::string const candidate = rom_path + "/" + rom_names[i];
    if (std::filesystem::exists(candidate)) {
      FILE* fp = fopen(candidate.c_str(), "rb");
      if (fp) {
        byte* rom_data = new byte[16384];
        memset(rom_data, 0xFF, 16384);
        size_t const nread = fread(rom_data, 1, 16384, fp);
        fclose(fp);
        if (nread >= 128 && rom_data[0] <= 0x02) {
          rom_map[rom_slot_] = rom_data;
          loaded_ = true;
          auto_loaded_ = true;
          return;
        }
        delete[] rom_data;
      }
    }
  }

  // No ROM file found — serial interface has no expansion ROM
  LOG_INFO("Serial interface ROM not found in " << rom_path);
}

void SIRomManager::unload(byte** rom_map) {
  if (!auto_loaded_) return;

  if (rom_slot_ >= 0 && rom_slot_ < 32 && rom_map[rom_slot_] != nullptr) {
    delete[] rom_map[rom_slot_];
    rom_map[rom_slot_] = nullptr;
  }

  loaded_ = false;
  auto_loaded_ = false;
}

// SerialInterface implementation
void SerialInterface::host_tx(uint8_t byte) {
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    if (monitor_.size() == kMonitorCap) monitor_.pop_front();
    monitor_.push_back(byte);
  }
  if (backend == nullptr || !backend->send(byte))
    tx_dropped_.fetch_add(1, std::memory_order_relaxed);
}

bool SerialInterface::host_wired() const {
  const SerialConfig cfg = get_config();
  return cfg.enabled && cfg.backend_type != SerialBackendType::Plotter;
}

size_t SerialInterface::queue_rx(const uint8_t* data, size_t n) {
  std::lock_guard<std::mutex> lock(queue_mu_);
  size_t const room = kRxQueueCap - rx_queue_.size();
  size_t const taken = n < room ? n : room;
  rx_queue_.insert(rx_queue_.end(), data, data + taken);
  return taken;
}

size_t SerialInterface::rx_pending() const {
  std::lock_guard<std::mutex> lock(queue_mu_);
  return rx_queue_.size();
}

size_t SerialInterface::pump_rx(size_t room, void (*sink)(uint8_t, void*),
                                void* ctx) {
  size_t moved = 0;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    while (moved < room && !rx_queue_.empty()) {
      sink(rx_queue_.front(), ctx);
      rx_queue_.pop_front();
      ++moved;
    }
  }
  // The backend's bytes stay in the backend (socket buffer, file, tty) until
  // the FIFO has room for them, rather than overrunning it.
  while (moved < room && backend != nullptr && backend->has_data()) {
    sink(backend->recv(), ctx);
    ++moved;
  }
  return moved;
}

void SerialInterface::drain_monitor(std::vector<uint8_t>& out) {
  std::lock_guard<std::mutex> lock(queue_mu_);
  out.insert(out.end(), monitor_.begin(), monitor_.end());
  monitor_.clear();
}

void SerialInterface::set_config(const SerialConfig& config) {
  config_ = config;
}

void SerialInterface::apply_config() {
  // Close existing backend
  if (backend) {
    backend->close();
    delete backend;
    backend = nullptr;
  }

  applied_config_ = config_;
  applied_ = true;

  if (!config_.enabled) {
    // Bytes queued for a card that is now unplugged would otherwise arrive
    // the next time someone enables it.
    {
      std::lock_guard<std::mutex> lock(queue_mu_);
      rx_queue_.clear();
    }
    z80_set_bdos_serial_out_hook(nullptr);
    z80_set_bdos_serial_in_hook(nullptr);
    return;
  }

  // Create new backend based on type
  switch (config_.backend_type) {
    case SerialBackendType::Null:
      backend = new NullBackend();
      break;

    case SerialBackendType::File:
      backend = new FileBackend(config_.input_file, config_.output_file);
      break;

    case SerialBackendType::HostSerial:
      backend = new HostSerialBackend(config_.device_path);
      break;

    case SerialBackendType::NullModem:
      backend = new NullModemBackend();
      break;

    case SerialBackendType::TcpSocket:
      backend = new TcpSocketBackend(config_.tcp_host, config_.tcp_port);
      break;

    case SerialBackendType::Plotter:
      backend = new PlotterBackend();
      break;
  }

  if (backend) {
    // The board's rs232 Device reaches this backend through the bridge:
    // host_tx() for bytes the CPC sends, pump_rx() for bytes it receives.
    backend->open();

    // The BDOS serial hooks that used to shortcut the plotter backend are
    // retired (beads-5q4v milestone C): the plotter is a bus Device on the
    // RS232 card's wire now, driven through the DART registers like real
    // hardware — no firmware-vector intercepts. The hook API itself lives on
    // in z80_view.h until the legacy core deletion.
    z80_set_bdos_serial_out_hook(nullptr);
    z80_set_bdos_serial_in_hook(nullptr);
  }
}

// Global SI ROM manager instance
SIRomManager g_si_rom;

// Global serial interface instance
SerialInterface g_serial_interface;

// PlotterBackend implementation
PlotterBackend::PlotterBackend() : open_(false), plotter_(&g_plotter) {}

PlotterBackend::~PlotterBackend() { close(); }

bool PlotterBackend::open() {
  open_ = true;
  if (plotter_) {
    plotter_->reset();
  }
  return true;
}

void PlotterBackend::close() { open_ = false; }

bool PlotterBackend::send(uint8_t byte) {
  if (!open_) return false;

  if (byte == 0x05) {  // ENQ — buffer-space query
    // HP 7470A responds with available buffer space as decimal + CR.
    // The driver reads this via the same BDOS3 read-until-CR loop used for
    // OS;/OD;. Our emulated plotter has unlimited capacity — report maximum
    // (128 bytes free).
    for (char c : std::string("128\r"))
      response_queue_.push(static_cast<uint8_t>(c));
    return true;
  }

  if (plotter_) plotter_->feed_byte(byte & 0x7F);  // HP-GL is 7-bit ASCII

  // Accumulate for query command detection
  char const c = static_cast<char>(byte & 0x7F);
  cmd_buf_ += c;

  if (c == ';' || c == ':' || c == '\r' || c == '\n') {
    process_command();
    cmd_buf_.clear();
  }
  if (cmd_buf_.size() > 64)
    cmd_buf_.clear();  // safety: discard runaway partial cmds
  return true;
}

void PlotterBackend::process_command() {
  // Uppercase and strip whitespace for matching
  std::string cmd;
  for (char const c : cmd_buf_) {
    if (c >= 'a' && c <= 'z')
      cmd += static_cast<char>(c - 32);
    else if (static_cast<uint8_t>(c) > ' ')
      cmd += c;
  }

  // OS; — Output Status: plotter responds with decimal status then '\r'
  // HP 7470A status bits: bit4=Ready(16), bit3=Initialized(8). After
  // power-on/IN: first call returns 24 (Ready+Initialized), subsequent calls
  // return 16 (Ready). DDHP7470.PRL reads OS; response until it gets CR,
  // checking for non-error status.
  if (cmd.size() >= 2 && cmd[0] == 'O' && cmd[1] == 'S') {
    for (char c : std::string("16\r"))
      response_queue_.push(static_cast<uint8_t>(c));
  }
  // OD; — Output Digitize/Dimensions: plotter responds with coordinates then
  // '\r' HP 7470A plottable area: 0,0 to 10300,7650 (0.025mm/unit → 257×191mm ≈
  // A4)
  else if (cmd.size() >= 2 && cmd[0] == 'O' && cmd[1] == 'D') {
    for (char c : std::string("0,0,10300,7650\r"))
      response_queue_.push(static_cast<uint8_t>(c));
  }
  // OI; — Output Identification: returns model string
  else if (cmd.size() >= 2 && cmd[0] == 'O' && cmd[1] == 'I') {
    for (char c : std::string("7470A\r"))
      response_queue_.push(static_cast<uint8_t>(c));
  }
}

bool PlotterBackend::has_data() const { return !response_queue_.empty(); }

uint8_t PlotterBackend::recv() {
  if (!response_queue_.empty()) {
    uint8_t const b = response_queue_.front();
    response_queue_.pop();
    return b;
  }
  return 0x00;
}

std::string PlotterBackend::status() const {
  if (plotter_ && plotter_->has_output()) {
    return "HP-GL ready, output pending";
  }
  return "HP-GL ready";
}
