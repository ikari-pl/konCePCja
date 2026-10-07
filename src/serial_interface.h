/* konCePCja — Serial Interface host side (AMSIf-compatible)
 *
 * The card itself -- Z80 DART at $FADx, Intel 8253 at $FBDx -- is the rs232
 * Device the board clocks (src/hw/rs232.cpp, docs/hardware/rs232-device.md).
 * This file is the host end of its wire: the pluggable backends, the queues
 * the IPC server and the Serial Terminal share with the Z80 thread, and the
 * SI ROM loader.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "serial_config.h"
#include "types.h"

namespace config {
class Config;
}

// Serial backend interface (pluggable backends)
class SerialBackend {
 public:
  virtual ~SerialBackend() = default;

  // Open/close
  virtual bool open() = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;

  // Send/receive
  // false when the byte could not be delivered (not connected, I/O error).
  virtual bool send(uint8_t byte) = 0;
  virtual bool has_data() const = 0;
  virtual uint8_t recv() = 0;

  // Status
  virtual bool connected() const = 0;
  virtual std::string name() const = 0;
  virtual std::string status() const = 0;
};

// Null backend (drops all data, for testing)
class NullBackend : public SerialBackend {
 public:
  bool open() override { return true; }
  void close() override {}
  bool is_open() const override { return true; }
  bool send(uint8_t /*byte*/) override { return true; }
  bool has_data() const override { return false; }
  uint8_t recv() override { return 0; }
  bool connected() const override { return true; }
  std::string name() const override { return "Null"; }
  std::string status() const override { return "Dropping all data"; }
};

// File backend (input/output files)
class FileBackend : public SerialBackend {
 public:
  FileBackend(std::string input_path, std::string output_path);
  ~FileBackend() override { close(); }

  bool open() override;
  void close() override;
  bool is_open() const override { return open_; }

  bool send(uint8_t byte) override;
  bool has_data() const override;
  uint8_t recv() override;

  bool connected() const override { return open_; }
  std::string name() const override { return "File"; }
  std::string status() const override;

 private:
  std::string input_path_;
  std::string output_path_;
  FILE* input_file_ = nullptr;
  FILE* output_file_ = nullptr;
  bool open_ = false;
  bool tx_error_logged_ = false;  // report a failing output file once
  mutable int lookahead_ = -1;    // buffered byte for peek, -1 = empty
};

// Host serial backend (POSIX /dev/tty.* on macOS/Linux only)
#ifndef _WIN32
class HostSerialBackend : public SerialBackend {
 public:
  explicit HostSerialBackend(std::string device_path);
  ~HostSerialBackend() override;

  bool open() override;
  void close() override;
  bool is_open() const override { return fd_ >= 0; }

  bool send(uint8_t byte) override;
  bool has_data() const override;
  uint8_t recv() override;

  bool connected() const override;
  std::string name() const override { return "HostSerial"; }
  std::string status() const override;

  static std::vector<std::string> list_ports();

 private:
  std::string device_path_;
  int fd_ = -1;
  int original_flags_ = 0;
};
#else
// Windows stub — termios/POSIX serial not available on Win32
class HostSerialBackend : public SerialBackend {
 public:
  explicit HostSerialBackend(const std::string&) {}
  bool open() override { return false; }
  void close() override {}
  bool is_open() const override { return false; }
  bool send(uint8_t) override { return false; }
  bool has_data() const override { return false; }
  uint8_t recv() override { return 0; }
  bool connected() const override { return false; }
  std::string name() const override { return "HostSerial"; }
  std::string status() const override { return "not supported on Windows"; }
  static std::vector<std::string> list_ports() { return {}; }
};
#endif

// Null modem backend (loopback between two instances)
class NullModemBackend : public SerialBackend {
 public:
  NullModemBackend();
  ~NullModemBackend() override;

  bool open() override;
  void close() override;
  bool is_open() const override { return open_; }

  bool send(uint8_t byte) override;
  bool has_data() const override;
  uint8_t recv() override;

  bool connected() const override { return open_; }
  std::string name() const override { return "NullModem"; }
  std::string status() const override;

  void connect_peer(NullModemBackend* peer);
  void disconnect_peer();

 private:
  bool open_ = false;
  NullModemBackend* peer_ = nullptr;
  std::queue<uint8_t> rx_buffer_;
  mutable std::mutex rx_mutex_;
};

// TCP socket backend (client mode)
class TcpSocketBackend : public SerialBackend {
 public:
  explicit TcpSocketBackend(std::string host, uint16_t port);
  ~TcpSocketBackend() override;

  // How long open() waits for a non-blocking connect() to resolve before
  // returning with the connection still in flight (resolved lazily later).
  static constexpr int kConnectWaitMs = 100;

  // How long open() waits for the host name to resolve. getaddrinfo() has no
  // timeout of its own, so the lookup runs on a worker thread and open()
  // gives up here rather than freezing its caller (startup, `config apply`,
  // or the IPC thread) for as long as an unreachable resolver takes.
  static constexpr int kResolveWaitMs = 2000;

  // true once the socket exists and the connect is established or still in
  // flight; false when the host does not resolve in time and when the peer
  // refused.
  bool open() override;
  void close() override;
  bool is_open() const override { return state_ != State::Disconnected; }

  // false when the byte was not handed to the kernel: not (yet) connected,
  // or the peer went away (which also drops the connection).
  bool send(uint8_t byte) override;
  bool has_data() const override;
  uint8_t recv() override;

  // Only true after the connect has really completed (SO_ERROR == 0).
  bool connected() const override;
  std::string name() const override { return "TcpSocket"; }
  std::string status() const override;

 private:
  enum class State : std::uint8_t { Disconnected, Connecting, Connected };

  // Resolve a Connecting socket: wait up to timeout_ms for writability, then
  // read SO_ERROR. Returns the state afterwards.
  State resolve_connect(int timeout_ms) const;
  // The peer closed or reset the connection.
  void mark_disconnected() const;

  std::string host_;
  uint16_t port_;
  int sockfd_ = -1;
  mutable std::atomic<State> state_{State::Disconnected};
  mutable std::vector<uint8_t> rx_buffer_;
  mutable std::mutex rx_mutex_;
};

// Serial interface state container
struct SerialInterface {
  // The open backend, or null. The Z80 thread transmits through it (the
  // Machine's host_tx ctx is this SerialInterface, never the backend) while
  // the IPC and UI threads may replace it via apply_config() at any moment,
  // so callers get a counted reference: a backend swapped out mid-send stays
  // alive until that send returns, and is destroyed by whoever drops it last.
  std::shared_ptr<SerialBackend> backend() const;
  // Replace the backend (apply_config()'s own path; tests inject one).
  void set_backend(std::shared_ptr<SerialBackend> next);

  void set_config(const SerialConfig& config);
  SerialConfig get_config() const;
  void apply_config();
  // True once apply_config() has actually reopened the backend for the
  // config currently staged in config_ -- false right after set_config()
  // stages a change apply_config() hasn't seen yet. A rebuild triggered by
  // an unrelated setting (RAM size, CRTC type, model) should skip
  // re-opening an already-current backend rather than truncate/reconnect it.
  bool config_applied() const;

  // Hand one CPC-transmitted byte to the backend, counting the ones it could
  // not deliver (not connected, peer gone, disk full) so `serial status`
  // shows them instead of a log line nobody reads.
  void host_tx(uint8_t byte);
  uint64_t tx_dropped() const {
    return tx_dropped_.load(std::memory_order_relaxed);
  }

  // True while the card's wire runs to the host (enabled, any backend but
  // the plotter, which is a Device on that wire and has it to itself). Only
  // then do the queues below mean anything.
  bool host_wired() const;

  // Host -> CPC bytes typed in the Serial Terminal or sent over IPC. They
  // wait here for room in the card's RX FIFO, which is three bytes deep and
  // overruns when pushed past that. Any thread may queue; returns how many
  // of the n bytes fitted under kRxQueueCap.
  size_t queue_rx(const uint8_t* data, size_t n);
  size_t rx_pending() const;
  // Hand at most `room` bytes to `sink` (the card's RX FIFO): queued bytes
  // first, then whatever the backend has. The bridge calls this on the Z80
  // thread once per frame with the FIFO's free slots. Returns bytes moved.
  size_t pump_rx(size_t room, void (*sink)(uint8_t, void*), void* ctx);
  // CPC -> host bytes, mirrored for the Serial Terminal: host_tx() appends,
  // keeping the newest kMonitorCap while nobody drains; the UI thread
  // appends them all to `out`.
  void drain_monitor(std::vector<uint8_t>& out);

  static constexpr size_t kRxQueueCap = 64 * 1024;
  static constexpr size_t kMonitorCap = 4096;

 private:
  std::atomic<uint64_t> tx_dropped_{0};
  // Guards backend_, config_, applied_config_ and applied_. Held only to copy
  // or swap them, never across backend I/O.
  mutable std::mutex mu_;
  std::shared_ptr<SerialBackend> backend_;
  // Guards rx_queue_ and monitor_ only: the IPC/UI threads fill and drain
  // them while the Z80 thread pumps and transmits.
  mutable std::mutex queue_mu_;
  std::deque<uint8_t> rx_queue_;
  std::deque<uint8_t> monitor_;
  SerialConfig config_;
  SerialConfig applied_config_;
  bool applied_ = false;
};

// SI ROM manager - handles loading SI ROM firmware into expansion ROM slot
class SIRomManager {
 public:
  SIRomManager();
  ~SIRomManager() = default;

  void load(byte** rom_map, const std::string& rom_path);
  void unload(byte** rom_map);

  int get_slot() const { return rom_slot_; }
  void set_slot(int slot) { rom_slot_ = slot; }
  bool is_loaded() const { return loaded_; }
  bool is_auto_loaded() const { return auto_loaded_; }

  static constexpr int DEFAULT_SLOT = 2;

 private:
  int rom_slot_;
  bool loaded_;
  bool auto_loaded_;
};

// Global serial interface instance
extern SerialInterface g_serial_interface;

// Global SI ROM manager
extern SIRomManager g_si_rom;

// Plotter backend - connects to HP-GL plotter
class PlotterBackend : public SerialBackend {
 public:
  PlotterBackend();
  ~PlotterBackend() override;

  bool open() override;
  void close() override;
  bool is_open() const override { return open_; }

  bool send(uint8_t byte) override;
  bool has_data() const override;
  uint8_t recv() override;

  bool connected() const override { return open_; }
  std::string name() const override { return "Plotter"; }
  std::string status() const override;

  // Expose plotter for UI
  class HpglPlotter* plotter() { return plotter_; }

 private:
  bool open_ = false;
  std::string cmd_buf_;  // Accumulates HP-GL bytes until command terminator
  std::queue<uint8_t>
      response_queue_;  // Queued response bytes (ENQ/OS;/OD; replies)
  class HpglPlotter* plotter_ = nullptr;

  void process_command();  // Called on ';', ':', '\r', '\n' in cmd_buf_
};
