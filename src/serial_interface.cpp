/* konCePCja — Serial Interface Implementation */

#include "serial_interface.h"
#include "io_dispatch.h"
#include "log.h"
#include "plotter.h"
#include "z80.h"
#include <cstdio>
#include <cstring>
#include <filesystem>

// I/O port handlers for $FADx (DART) and $FBDx (8253 Timer)
// DART registers at $FADC-$FADF, timer at $FBDC-$FBDF.
// Must check full low byte 0xDC-0xDF to avoid colliding with FDC ($FB7E) etc.
static bool dart_in(reg_pair port, byte& ret_val) {
    uint8_t lo = port.b.l;
    if (lo >= 0xDC && lo <= 0xDF) {
        ret_val = g_serial_interface.dart.read(lo - 0xDC);
        return true;
    }
    return false;
}

static bool dart_out(reg_pair port, byte val) {
    uint8_t lo = port.b.l;
    if (lo >= 0xDC && lo <= 0xDF) {
        g_serial_interface.dart.write(lo - 0xDC, val);
        return true;
    }
    return false;
}

static bool timer_in(reg_pair port, byte& ret_val) {
    uint8_t lo = port.b.l;
    if (lo >= 0xDC && lo <= 0xDF) {
        ret_val = g_serial_interface.timer.read(lo - 0xDC);
        return true;
    }
    return false;
}

static bool timer_out(reg_pair port, byte val) {
    uint8_t lo = port.b.l;
    if (lo >= 0xDC && lo <= 0xDF) {
        g_serial_interface.timer.write(lo - 0xDC, val);
        return true;
    }
    return false;
}

// Configuration flag for enabling/disabling serial interface
bool serial_interface_enabled = false;

void serial_interface_register_io() {
    io_register_in(0xFA, dart_in, &serial_interface_enabled, "Serial Interface (DART)");
    io_register_out(0xFA, dart_out, &serial_interface_enabled, "Serial Interface (DART)");
    io_register_in(0xFB, timer_in, &serial_interface_enabled, "Serial Interface (8253 Timer)");
    io_register_out(0xFB, timer_out, &serial_interface_enabled, "Serial Interface (8253 Timer)");
}

// Z80 DART Implementation
Z80Dart::Z80Dart() {
    reset();
}

void Z80Dart::reset() {
    channel_a_ = true;
    while (!rx_fifo_.empty()) rx_fifo_.pop();
    
    tx_buffer_empty_ = true;
    tx_shift_reg_empty_ = true;
    tx_buffer_ = 0;
    tx_shift_ = 0;
    
    rts_ = false;
    cts_ = true;
    dtr_ = false;
    dsr_ = true;
    dcd_ = false;
    break_pending_ = false;
    
    overrun_error_ = false;
    parity_error_ = false;
    framing_error_ = false;
    
    memset(wr0_, 0, sizeof(wr0_));
    memset(wr1_, 0, sizeof(wr1_));
    memset(wr3_, 0, sizeof(wr3_));
    
    rr0_[0] = RR0_TX_EMPTY | RR0_TX_BUFFER_EMPTY | RR0_CTS;
    rr0_[1] = RR0_TX_EMPTY | RR0_TX_BUFFER_EMPTY | RR0_CTS;
    rr1_[0] = 0;
    rr1_[1] = 0;
    rr2_ = 0;
    
    interrupt_pending_ = false;
    interrupt_vector_ = 0;
}

uint8_t Z80Dart::read(uint8_t port) {
    // Z80 DART port mapping (active on Amstrad Serial Interface):
    //   Bit 0 = B/~A: 0 = Channel A, 1 = Channel B
    //   Bit 1 = C/~D: 0 = Data,      1 = Control
    //
    //   $FADC (00) = Channel A Data
    //   $FADD (01) = Channel B Data
    //   $FADE (10) = Channel A Control (RR0 by default)
    //   $FADF (11) = Channel B Control (RR0 by default)

    int reg = port & 0x03;

    switch (reg) {
        case 0x00:  // Channel A Data — read received byte
        case 0x01:  // Channel B Data
        {
            uint8_t val = 0;
            if (!rx_fifo_.empty()) {
                val = rx_fifo_.front();
                rx_fifo_.pop();
                update_interrupts();
            }
            return val;
        }

        case 0x02:  // Channel A Control — return RR0 (default register pointer)
        case 0x03:  // Channel B Control
            return read_rr(0);
    }

    return 0xFF;
}

uint8_t Z80Dart::read_rr(int reg) {
    int ch = channel_a_ ? 0 : 1;
    uint8_t val = 0;
    
    switch (reg) {
        case 0:  // RR0 - Status
            // Poll backend for incoming data before reporting status.
            // This ensures RX_AVAILABLE is up-to-date when the Z80 checks.
            if (rx_poll_) rx_poll_();
            val = rr0_[ch];
            // Update TX empty status
            if (tx_buffer_empty_) val |= RR0_TX_BUFFER_EMPTY;
            if (tx_shift_reg_empty_) val |= RR0_TX_EMPTY;
            // Update RX available
            if (!rx_fifo_.empty()) val |= RR0_RX_AVAILABLE;
            else val &= ~RR0_RX_AVAILABLE;
            // CTS follows the actual cts_ state (loopback = true)
            if (cts_) val |= RR0_CTS;
            else val &= ~RR0_CTS;
            break;
            
        case 1:  // RR1 - Special Receive Condition
            val = rr1_[ch];
            if (!rx_fifo_.empty()) val |= RR0_RX_AVAILABLE;
            if (overrun_error_) val |= RR1_OVERRUN;
            if (parity_error_) val |= RR1_PARITY_ERROR;
            if (framing_error_) val |= RR1_CRC_FRAMING_ERROR;
            break;
            
        case 2:  // RR2 - Interrupt Vector
            val = channel_a_ ? wr2_[0] : rr2_;
            break;
    }
    
    return val;
}

void Z80Dart::write(uint8_t port, uint8_t val) {
    int reg = port & 0x03;
    
    switch (reg) {
        case 0x00:  // Data register
            // Send byte to backend
            if (rx_callback_) {
                rx_callback_(val);
            }
            // Store in TX buffer
            tx_buffer_ = val;
            tx_buffer_empty_ = false;
            // Update status
            rr0_[channel_a_ ? 0 : 1] &= ~RR0_TX_BUFFER_EMPTY;
            do_tx();
            break;
            
        case 0x01:  // Control register - WR0 (reset/command)
            wr0_[channel_a_ ? 0 : 1] = val;
            // Check for reset commands
            if ((val & 0xC0) == 0xC0) {
                // Reset
                int reset_type = (val >> 6) & 0x03;
                switch (reset_type) {
                    case 1:  // Reset receiver
                        while (!rx_fifo_.empty()) rx_fifo_.pop();
                        overrun_error_ = false;
                        parity_error_ = false;
                        framing_error_ = false;
                        break;
                    case 2:  // Reset transmitter
                        tx_buffer_empty_ = true;
                        tx_shift_reg_empty_ = true;
                        rr0_[channel_a_ ? 0 : 1] |= RR0_TX_EMPTY | RR0_TX_BUFFER_EMPTY;
                        break;
                    case 3:  // Reset error flags
                        overrun_error_ = false;
                        parity_error_ = false;
                        framing_error_ = false;
                        rr1_[channel_a_ ? 0 : 1] = 0;
                        break;
                }
            }
            // Channel select is in WR0 bits 2-3
            if ((val & 0x0C) == 0x04) channel_a_ = false;
            else if ((val & 0x0C) == 0x00) channel_a_ = true;
            break;
            
        case 0x02:  // WR1 or WR2 depending on channel
            if (channel_a_) {
                wr1_[0] = val;  // Interrupt enable
            } else {
                wr2_[1] = val;  // Channel B interrupt vector
            }
            update_interrupts();
            break;
            
        case 0x03:  // WR3 (receive parameters) or WR2 (interrupt vector for A)
            if (channel_a_) {
                wr3_[0] = val;
            } else {
                wr2_[1] = val;  // Same as above
            }
            break;
    }
}

void Z80Dart::enqueue_rx(uint8_t byte) {
    if (rx_fifo_.size() < RX_FIFO_SIZE) {
        rx_fifo_.push(byte);
    } else {
        overrun_error_ = true;
    }
    update_interrupts();
}

void Z80Dart::do_tx() {
    if (tx_buffer_empty_) return;
    
    tx_shift_ = tx_buffer_;
    tx_buffer_empty_ = true;
    tx_shift_reg_empty_ = false;
    rr0_[channel_a_ ? 0 : 1] |= RR0_TX_BUFFER_EMPTY;
    
    // TX complete - mark shift register empty after "transmit time"
    // In real hardware this would be baud rate dependent
    tx_shift_reg_empty_ = true;
    rr0_[channel_a_ ? 0 : 1] |= RR0_TX_EMPTY;
    rr1_[channel_a_ ? 0 : 1] |= RR1_ALL_SENT;
    
    update_interrupts();
}

void Z80Dart::update_interrupts() {
    bool should_int = false;
    
    int ch = channel_a_ ? 0 : 1;
    
    // Check interrupt conditions based on WR1
    uint8_t wr1 = wr1_[ch];
    
    // External/Status interrupt - fires on CTS/DCD/DSR changes (bit 0)
    // For simplicity, we don't simulate these status changes
    // Real hardware would check for status changes via RR0 bits
    
    // Transmit interrupt - fires when TX buffer empty and TX interrupt enabled (bit 1)
    if ((wr1 & 0x02) && tx_buffer_empty_) {
        should_int = true;
    }
    
    // Receive interrupt - fires when character available and Rx interrupt enabled (bit 2)
    if ((wr1 & 0x04) && !rx_fifo_.empty()) {
        should_int = true;
    }
    
    interrupt_pending_ = should_int;
    
    // Set interrupt vector
    interrupt_vector_ = wr2_[0] | (should_int ? 0x00 : 0x08);
}

uint8_t Z80Dart::get_interrupt_vector() const {
    return interrupt_vector_;
}

// Intel 8253 Timer Implementation
Intel8253::Intel8253() {
    reset();
}

void Intel8253::reset() {
    for (int i = 0; i < 3; i++) {
        counters_[i].count = 0xFFFF;  // un-programmed 8253 reads high
        counters_[i].latch = 0xFFFF;
        counters_[i].mode = 0;
        counters_[i].counting = false;
        counters_[i].gate = true;
        latch_count_[i] = false;
    }
    mode_register_ = 0;
}

uint8_t Intel8253::read(uint8_t port) {
    int ch = port & 0x03;
    
    // Port 3 is mode register - reading it returns 0
    if (ch >= 3) return 0;  // Mode register is write-only
    
    // Check if count is latched for reading
    if (latch_count_[ch]) {
        latch_count_[ch] = false;
        // Return latched value (LSB)
        return counters_[ch].latch & 0xFF;
    }
    
    return counters_[ch].count & 0xFF;
}

void Intel8253::write(uint8_t port, uint8_t val) {
    int ch = port & 0x03;
    
    if (ch == 3) {
        // Mode register write (port 3 = 0x0F)
        // Check for latch commands (bits 7-6)
        switch (val >> 6) {
            case 0:
                counters_[0].latch = counters_[0].count;
                latch_count_[0] = true;
                break;
            case 1:
                counters_[1].latch = counters_[1].count;
                latch_count_[1] = true;
                break;
            case 2:
                counters_[2].latch = counters_[2].count;
                latch_count_[2] = true;
                break;
            case 3:
                counters_[0].latch = counters_[0].count;
                counters_[1].latch = counters_[1].count;
                counters_[2].latch = counters_[2].count;
                latch_count_[0] = true;
                latch_count_[1] = true;
                latch_count_[2] = true;
                break;
        }
        return;
    }
    
    // Counter write - always update the counter value
    Counter& c = counters_[ch];
    c.count = val;
    c.counting = true;
    
    // Counter 0 is used for baud rate
    if (ch == 0) {
        update_baud();
    }
}

void Intel8253::update_baud() {
    if (manual_baud_.has_value()) return;
    
    uint16_t divisor = counters_[0].count;
    if (divisor == 0) divisor = 1;  // Avoid divide by zero
    
    if (baud_callback_) {
        baud_callback_(divisor);
    }
}

void Intel8253::set_manual_baud(uint32_t baud) {
    manual_baud_ = baud;
}

uint16_t Intel8253::read_counter(int ch) const {
    if (ch < 0 || ch >= 3) return 0;
    return counters_[ch].count;
}

// File Backend Implementation
FileBackend::FileBackend(const std::string& input_path, const std::string& output_path)
    : input_path_(input_path), output_path_(output_path) {
    // With no files, backend is still "open" (like NullBackend)
    if (input_path_.empty() && output_path_.empty()) {
        open_ = true;
    }
}

bool FileBackend::open() {
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
        fclose(input_file_);
        input_file_ = nullptr;
    }
    if (output_file_) {
        fclose(output_file_);
        output_file_ = nullptr;
    }
    open_ = false;
}

void FileBackend::send(uint8_t byte) {
    if (output_file_) {
        fputc(byte, output_file_);
        fflush(output_file_);
    }
}

bool FileBackend::has_data() const {
    if (!input_file_) return false;
    if (lookahead_ >= 0) return true;
    int ch = fgetc(input_file_);
    if (ch == EOF) return false;
    lookahead_ = ch;
    return true;
}

uint8_t FileBackend::recv() {
    if (lookahead_ >= 0) {
        uint8_t byte = static_cast<uint8_t>(lookahead_);
        lookahead_ = -1;
        return byte;
    }
    if (!input_file_) return 0;
    int ch = fgetc(input_file_);
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

// Host Serial Backend Implementation
#ifdef __APPLE__
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <dirent.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <dirent.h>
#endif

HostSerialBackend::HostSerialBackend(const std::string& device_path)
    : device_path_(device_path), fd_(-1), original_flags_(0) {}

HostSerialBackend::~HostSerialBackend() {
    close();
}

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

void HostSerialBackend::send(uint8_t byte) {
    if (fd_ >= 0) {
        ::write(fd_, &byte, 1);
    }
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
    ssize_t n = ::read(fd_, &byte, 1);
    return (n > 0) ? byte : 0;
}

bool HostSerialBackend::connected() const {
    return fd_ >= 0;
}

std::string HostSerialBackend::status() const {
    if (fd_ >= 0) {
        return "Connected to " + device_path_;
    }
    return "Disconnected from " + device_path_;
}

std::vector<std::string> HostSerialBackend::list_ports() {
    std::vector<std::string> ports;
#if defined(__APPLE__)
    DIR* dir = opendir("/dev");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
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

// Null Modem Backend Implementation
NullModemBackend::NullModemBackend() : open_(false), peer_(nullptr) {}

NullModemBackend::~NullModemBackend() {
    close();
}

bool NullModemBackend::open() {
    open_ = true;
    return true;
}

void NullModemBackend::close() {
    if (peer_) {
        if (peer_->peer_ == this) {
            {
                std::lock_guard<std::mutex> lock(peer_->rx_mutex_);
                while (!peer_->rx_buffer_.empty()) peer_->rx_buffer_.pop();
            }
            peer_->peer_ = nullptr;
        }
        peer_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        while (!rx_buffer_.empty()) rx_buffer_.pop();
    }
    open_ = false;
}

void NullModemBackend::send(uint8_t byte) {
    if (peer_ && open_) {
        std::lock_guard<std::mutex> lock(peer_->rx_mutex_);
        peer_->rx_buffer_.push(byte);
    }
}

bool NullModemBackend::has_data() const {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    return !rx_buffer_.empty();
}

uint8_t NullModemBackend::recv() {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_buffer_.empty()) return 0;
    uint8_t byte = rx_buffer_.front();
    rx_buffer_.pop();
    return byte;
}

std::string NullModemBackend::status() const {
    if (peer_) {
        return "Connected to peer";
    }
    return "Waiting for peer connection";
}

void NullModemBackend::connect_peer(NullModemBackend* peer) {
    peer_ = peer;
}

void NullModemBackend::disconnect_peer() {
    {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        while (!rx_buffer_.empty()) rx_buffer_.pop();
    }
    if (peer_ && peer_->peer_ == this) {
        peer_->peer_ = nullptr;
    }
    peer_ = nullptr;
}

// TCP Socket Backend Implementation
#ifdef __APPLE__
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#elif defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#endif

TcpSocketBackend::TcpSocketBackend(const std::string& host, uint16_t port)
    : host_(host), port_(port), sockfd_(-1), connected_(false) {}

TcpSocketBackend::~TcpSocketBackend() {
    close();
}

bool TcpSocketBackend::open() {
    if (connected_) return true;

    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        return false;
    }

    struct hostent* server = gethostbyname(host_.c_str());
    if (server == nullptr) {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port_);

    int flags = fcntl(sockfd_, F_GETFL, 0);
    fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK);

    if (::connect(sockfd_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        if (errno != EINPROGRESS) {
            ::close(sockfd_);
            sockfd_ = -1;
            return false;
        }
    }

    connected_ = true;
    return true;
}

void TcpSocketBackend::close() {
    if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
    }
    {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        rx_buffer_.clear();
    }
    connected_ = false;
}

void TcpSocketBackend::send(uint8_t byte) {
    if (connected_ && sockfd_ >= 0) {
        ::send(sockfd_, &byte, 1, 0);
    }
}

bool TcpSocketBackend::has_data() const {
    if (!connected_) return false;
    std::lock_guard<std::mutex> lock(rx_mutex_);
    return !rx_buffer_.empty();
}

uint8_t TcpSocketBackend::recv() {
    if (!connected_) return 0;

    {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        if (!rx_buffer_.empty()) {
            uint8_t byte = rx_buffer_.front();
            rx_buffer_.erase(rx_buffer_.begin());
            return byte;
        }
    }

    if (sockfd_ >= 0) {
        uint8_t buf[256];
        ssize_t n = ::recv(sockfd_, buf, sizeof(buf), 0);
        if (n > 0) {
            std::lock_guard<std::mutex> lock(rx_mutex_);
            for (ssize_t i = 0; i < n; i++) {
                rx_buffer_.push_back(buf[i]);
            }
            if (!rx_buffer_.empty()) {
                uint8_t byte = rx_buffer_.front();
                rx_buffer_.erase(rx_buffer_.begin());
                return byte;
            }
        } else if (n == 0) {
            connected_ = false;
        }
    }
    return 0;
}

std::string TcpSocketBackend::status() const {
    if (connected_) {
        return "Connected to " + host_ + ":" + std::to_string(port_);
    }
    return "Disconnected from " + host_ + ":" + std::to_string(port_);
}

// SI ROM Manager Implementation
SIRomManager::SIRomManager()
    : rom_slot_(DEFAULT_SLOT), loaded_(false), auto_loaded_(false) {}

SIRomManager::~SIRomManager() {}

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
    static const char* rom_names[] = { "serial.rom", "SERIAL.ROM", "si_rom.bin", "SI_ROM.BIN", nullptr };
    for (int i = 0; rom_names[i]; i++) {
        std::string candidate = rom_path + "/" + rom_names[i];
        if (std::filesystem::exists(candidate)) {
            FILE* fp = fopen(candidate.c_str(), "rb");
            if (fp) {
                byte* rom_data = new byte[16384];
                memset(rom_data, 0xFF, 16384);
                size_t nread = fread(rom_data, 1, 16384, fp);
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

    // Sync I/O dispatch gate with config
    extern bool serial_interface_enabled;
    serial_interface_enabled = config_.enabled;

    if (!config_.enabled) {
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
        backend->open();
        
        // Connect DART TX to backend + mirror to serial terminal
        dart.set_rx_callback([this](uint8_t byte) {
            if (backend) backend->send(byte);
            extern void serial_terminal_feed_byte(uint8_t byte);
            serial_terminal_feed_byte(byte);
        });

        // Poll backend for incoming data on DART status reads
        dart.set_rx_poll([this]() {
            if (backend && backend->has_data()) {
                dart.enqueue_rx(backend->recv());
            }
        });
        
        // Set manual baud rate if configured
        if (config_.baud_rate > 0) {
            timer.set_manual_baud(config_.baud_rate);
        }

        // For the plotter backend, intercept BDOS L_WRITE (C=5) and A_READ (C=3)
        // at the emulator level — equivalent to what the real AMSIf BIOS patch did.
        // Both hooks simulate RET, bypassing BDOS/BIOS entirely.
        // C=5: route byte to PlotterBackend (avoids BIOS LIST → Centronics "not ready").
        // C=3: return queued plotter response (ENQ→ACK, OS;→"0\r", OD;→dimensions).
        if (config_.backend_type == SerialBackendType::Plotter) {
            z80_set_bdos_serial_out_hook([this](uint8_t byte) {
                if (backend) backend->send(byte);
            });
            z80_set_bdos_serial_in_hook([this]() -> uint8_t {
                if (backend && backend->has_data()) return backend->recv();
                return 0x11; // XON fallback
            });
        } else {
            z80_set_bdos_serial_out_hook(nullptr);
            z80_set_bdos_serial_in_hook(nullptr);
        }
    }
}

// Global SI ROM manager instance
SIRomManager g_si_rom;

// Global serial interface instance
SerialInterface g_serial_interface;


// PlotterBackend implementation
PlotterBackend::PlotterBackend() : open_(false), plotter_(&g_plotter) {}

PlotterBackend::~PlotterBackend() {
    close();
}

bool PlotterBackend::open() {
    open_ = true;
    if (plotter_) {
        plotter_->reset();
    }
    return true;
}

void PlotterBackend::close() {
    open_ = false;
}

void PlotterBackend::send(uint8_t byte) {
    if (!open_) return;

    if (byte == 0x05) {             // ENQ — handshake query
        enq_pending_ = true;        // respond with ACK on next recv()
        return;
    }

    if (plotter_) plotter_->feed_byte(byte & 0x7F);  // HP-GL is 7-bit ASCII

    // Accumulate for query command detection
    char c = static_cast<char>(byte & 0x7F);
    cmd_buf_ += c;

    if (c == ';' || c == ':' || c == '\r' || c == '\n') {
        process_command();
        cmd_buf_.clear();
    }
    if (cmd_buf_.size() > 64) cmd_buf_.clear();  // safety: discard runaway partial cmds
}

void PlotterBackend::process_command() {
    // Uppercase and strip whitespace for matching
    std::string cmd;
    for (char c : cmd_buf_) {
        if (c >= 'a' && c <= 'z') cmd += static_cast<char>(c - 32);
        else if (static_cast<uint8_t>(c) > ' ') cmd += c;
    }

    // OS; — Output Status: plotter responds with decimal status then '\r'
    // 0 = ready, no errors.
    if (cmd.size() >= 2 && cmd[0] == 'O' && cmd[1] == 'S') {
        for (uint8_t b : {(uint8_t)'0', (uint8_t)'\r'}) response_queue_.push(b);
    }
    // OD; — Output Digitize/Dimensions: plotter responds with coordinates then '\r'
    // HP 7470A plottable area: 0,0 to 10300,7650 (0.025mm/unit → 257×191mm ≈ A4)
    else if (cmd.size() >= 2 && cmd[0] == 'O' && cmd[1] == 'D') {
        for (char c : std::string("0,0,10300,7650\r")) response_queue_.push(static_cast<uint8_t>(c));
    }
    // OI; — Output Identification: returns model string
    else if (cmd.size() >= 2 && cmd[0] == 'O' && cmd[1] == 'I') {
        for (char c : std::string("7470A\r")) response_queue_.push(static_cast<uint8_t>(c));
    }
}

bool PlotterBackend::has_data() const {
    return enq_pending_ || !response_queue_.empty();
}

uint8_t PlotterBackend::recv() {
    if (enq_pending_) {
        enq_pending_ = false;
        return 0x06;  // ACK — plotter ready
    }
    if (!response_queue_.empty()) {
        uint8_t b = response_queue_.front();
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
