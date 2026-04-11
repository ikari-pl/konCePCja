/* konCePCja — Serial Interface Emulation (AMSIf-compatible)
 *
 * Emulates the Amstrad Serial Interface with:
 * - Z80 DART (Z8470) at ports $FADx
 * - Intel 8253 Timer at ports $FBDx
 * - Optional SI ROM firmware (expansion ROM)
 */

#ifndef SERIAL_INTERFACE_H
#define SERIAL_INTERFACE_H

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <queue>
#include <optional>
#include <filesystem>
#include <vector>
#include <mutex>
#include <thread>
#include "types.h"

namespace config {
    class Config;
}

// Z80 DART (Z8470) emulation
class Z80Dart {
public:
    Z80Dart();

    // Port I/O (called from io_dispatch)
    uint8_t read(uint8_t port);
    void write(uint8_t port, uint8_t val);

    // Reset
    void reset();

    // Connect a callback for transmitted bytes (Z80 → backend)
    using RxCallback = std::function<void(uint8_t byte)>;
    void set_rx_callback(RxCallback cb) { rx_callback_ = cb; }

    // Connect a poll callback for incoming data (backend → DART rx_fifo).
    // Called when the Z80 reads the status register, so data appears
    // "just in time" — matches real DART behavior.
    using RxPollCallback = std::function<void()>;
    void set_rx_poll(RxPollCallback cb) { rx_poll_ = cb; }

    // Check if interrupt is pending
    bool has_interrupt() const { return interrupt_pending_; }
    uint8_t get_interrupt_vector() const;

    // Status for debugging UI
    bool tx_empty() const { return tx_buffer_empty_; }
    bool rx_available() const { return !rx_fifo_.empty(); }
    bool cts() const { return cts_; }  // Clear To Send

    // Send a byte (injects into RX FIFO, used by terminal UI)
    void enqueue_rx(uint8_t byte);

private:
    // DART Register indices
    enum class Reg { RR0=0, RR1=1, RR2=2, RR3=3, WR0=0, WR1=1, WR2=2, WR3=3 };

    // RR0 status bits
    static constexpr uint8_t RR0_RX_AVAILABLE    = 0x01;
    static constexpr uint8_t RR0_ZERO_COUNT      = 0x02;
    static constexpr uint8_t RR0_TX_EMPTY        = 0x04;
    static constexpr uint8_t RR0_TX_BUFFER_EMPTY = 0x08;
    static constexpr uint8_t RR0_BREAK           = 0x10;
    static constexpr uint8_t RR0_CTS             = 0x20;
    static constexpr uint8_t RR0_SYNC_HUNT       = 0x40;
    static constexpr uint8_t RR0_DCD             = 0x80;

    // RR1 status bits
    static constexpr uint8_t RR1_ALL_SENT       = 0x01;
    static constexpr uint8_t RR1_TX_UNDERRUN      = 0x02;
    static constexpr uint8_t RR1_CRC_FRAMING_ERROR = 0x08;
    static constexpr uint8_t RR1_OVERRUN         = 0x10;
    static constexpr uint8_t RR1_PARITY_ERROR     = 0x20;
    static constexpr uint8_t RR1_RX_OVERFLOW      = 0x40;

    // Channel selection
    bool channel_a_ = true;  // true = A, false = B

    // Receive FIFO (max 3 bytes)
    std::queue<uint8_t> rx_fifo_;
    static constexpr size_t RX_FIFO_SIZE = 3;

    // Transmit
    bool tx_buffer_empty_ = true;
    bool tx_shift_reg_empty_ = true;
    uint8_t tx_buffer_ = 0;
    uint8_t tx_shift_ = 0;

    // Modem control signals
    bool rts_ = false;   // Request To Send
    bool cts_ = true;    // Clear To Send (inverted for loopback)
    bool dtr_ = false;   // Data Terminal Ready
    bool dsr_ = true;    // Data Set Ready
    bool dcd_ = false;   // Data Carrier Detect
    bool break_pending_ = false;

    // Error flags
    bool overrun_error_ = false;
    bool parity_error_ = false;
    bool framing_error_ = false;

    // Write registers (WR0-WR3 for each channel)
    uint8_t wr0_[2] = {0};  // Command register
    uint8_t wr1_[2] = {0};  // Interrupt/DMA enable
    uint8_t wr2_[2] = {0};  // Interrupt vector (Channel A) / same (Channel B)
    uint8_t wr3_[2] = {0};  // Receive parameters

    // Read registers (RR0-RR3 for each channel)
    uint8_t rr0_[2] = {0};
    uint8_t rr1_[2] = {0};
    uint8_t rr2_ = 0;  // Interrupt vector (Channel B only)

    // External interrupt
    bool interrupt_pending_ = false;
    uint8_t interrupt_vector_ = 0;

    // Rx callback (Z80 → backend)
    RxCallback rx_callback_;
    // Rx poll (backend → rx_fifo, called on status read)
    RxPollCallback rx_poll_;

    // Internal helpers
    uint8_t read_rr(int reg);
    void write_wr(int reg, uint8_t val);
    void update_interrupts();
    void do_tx();
};

// Intel 8253 Timer emulation
class Intel8253 {
public:
    Intel8253();

    // Port I/O (called from io_dispatch)
    uint8_t read(uint8_t port);  // port: 0,1,2 or mode (0x03 = mode)
    void write(uint8_t port, uint8_t val);

    // Reset
    void reset();

    // Connect baud rate callback (called when counter 0 changes)
    using BaudRateCallback = std::function<void(uint16_t divisor)>;
    void set_baud_callback(BaudRateCallback cb) { baud_callback_ = cb; }

    // Manual baud rate override (bypasses counter)
    void set_manual_baud(uint32_t baud);
    bool has_manual_baud() const { return manual_baud_.has_value(); }
    void clear_manual_baud() { manual_baud_.reset(); }

    // Read current count (latched or actual)
    uint16_t read_counter(int ch) const;

private:
    // Counter 0: TX baud rate clock (to DART)
    // Counter 1: unused
    // Counter 2: unused

    struct Counter {
        uint16_t count = 0;        // Current count (down counter)
        uint16_t latch = 0;       // Latched count for read
        uint8_t mode = 0;          // 0-5
        bool counting = false;
        bool gate = true;
    } counters_[3];

    // Mode register ($FBDF)
    uint8_t mode_register_ = 0;

    // Read back command
    bool latch_count_[3] = {false, false, false};

    // Manual baud override
    std::optional<uint32_t> manual_baud_;

    // Baud rate callback
    BaudRateCallback baud_callback_;

    // Calculate divisor from counter 0
    void update_baud();
};

// Serial backend interface (pluggable backends)
class SerialBackend {
public:
    virtual ~SerialBackend() = default;

    // Open/close
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // Send/receive
    virtual void send(uint8_t byte) = 0;
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
    void send(uint8_t) override {}
    bool has_data() const override { return false; }
    uint8_t recv() override { return 0; }
    bool connected() const override { return true; }
    std::string name() const override { return "Null"; }
    std::string status() const override { return "Dropping all data"; }
};

// File backend (input/output files)
class FileBackend : public SerialBackend {
public:
    FileBackend(const std::string& input_path, const std::string& output_path);

    bool open() override;
    void close() override;
    bool is_open() const override { return open_; }

    void send(uint8_t byte) override;
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
    mutable int lookahead_ = -1;  // buffered byte for peek, -1 = empty
};

// Host serial backend (POSIX /dev/tty.* on macOS/Linux)
class HostSerialBackend : public SerialBackend {
public:
    explicit HostSerialBackend(const std::string& device_path);
    ~HostSerialBackend() override;

    bool open() override;
    void close() override;
    bool is_open() const override { return fd_ >= 0; }

    void send(uint8_t byte) override;
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

// Null modem backend (loopback between two instances)
class NullModemBackend : public SerialBackend {
public:
    NullModemBackend();
    ~NullModemBackend() override;

    bool open() override;
    void close() override;
    bool is_open() const override { return open_; }

    void send(uint8_t byte) override;
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
    explicit TcpSocketBackend(const std::string& host, uint16_t port);
    ~TcpSocketBackend() override;

    bool open() override;
    void close() override;
    bool is_open() const override { return connected_; }

    void send(uint8_t byte) override;
    bool has_data() const override;
    uint8_t recv() override;

    bool connected() const override { return connected_; }
    std::string name() const override { return "TcpSocket"; }
    std::string status() const override;

private:
    std::string host_;
    uint16_t port_;
    int sockfd_ = -1;
    bool connected_ = false;
    std::vector<uint8_t> rx_buffer_;
    mutable std::mutex rx_mutex_;
};

// Serial backend types
enum class SerialBackendType {
    Null,       // Drop all data
    File,       // Input/output files
    HostSerial, // Physical serial port
    NullModem,  // Loopback
    TcpSocket,  // TCP client
    Plotter     // HP-GL plotter (HP 7470A)
};

// Serial configuration
struct SerialConfig {
    bool enabled = false;
    SerialBackendType backend_type = SerialBackendType::Null;
    
    // File backend
    std::string input_file;
    std::string output_file;
    
    // HostSerial backend
    std::string device_path;
    
    // TcpSocket backend
    std::string tcp_host = "127.0.0.1";
    uint16_t tcp_port = 23;
    
    // Common settings
    uint32_t baud_rate = 9600;
};

// Serial interface state container
struct SerialInterface {
    Z80Dart dart;
    Intel8253 timer;
    SerialBackend* backend = nullptr;
    
    void set_config(const SerialConfig& config);
    SerialConfig get_config() const { return config_; }
    void apply_config();
    
private:
    SerialConfig config_;
};

// SI ROM manager - handles loading SI ROM firmware into expansion ROM slot
class SIRomManager {
public:
    SIRomManager();
    ~SIRomManager();

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

    void send(uint8_t byte) override;
    bool has_data() const override;
    uint8_t recv() override;

    bool connected() const override { return open_; }
    std::string name() const override { return "Plotter"; }
    std::string status() const override;

    // Expose plotter for UI
    class HpglPlotter* plotter() { return plotter_; }

private:
    bool open_ = false;
    std::string cmd_buf_;           // Accumulates HP-GL bytes until command terminator
    std::queue<uint8_t> response_queue_;  // Queued response bytes (ENQ/OS;/OD; replies)
    class HpglPlotter* plotter_ = nullptr;

    void process_command();         // Called on ';', ':', '\r', '\n' in cmd_buf_
};

#endif // SERIAL_INTERFACE_H
