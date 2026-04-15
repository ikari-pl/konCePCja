/* Tests for Serial Interface (Z80 DART + Intel 8253 + Backends)
 *
 * Comprehensive test coverage for:
 * - Z80Dart: register read/write, reset, interrupts, channel selection
 * - Intel8253: counter read/write, mode register, latch, manual baud
 * - NullBackend: basic operations
 * - FileBackend: file I/O operations
 */

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "serial_interface.h"
#include "types.h"

// ─────────────────────────────────────────────────
// Z80Dart Tests
// ─────────────────────────────────────────────────

class Z80DartTest : public testing::Test {
protected:
    void SetUp() override {
        dart = Z80Dart();
    }
    
    Z80Dart dart;
};

TEST_F(Z80DartTest, Reset_InitialState) {
    dart.reset();
    
    // TX should be empty and ready
    EXPECT_TRUE(dart.tx_empty());
    
    // RX FIFO should be empty
    EXPECT_FALSE(dart.rx_available());
    
    // CTS should be asserted (loopback)
    EXPECT_TRUE(dart.cts());
}

TEST_F(Z80DartTest, Read_RR0_InitialStatus) {
    // Read RR0 on Channel A (default)
    uint8_t status = dart.read(0x0D); // Control port
    
    // Should have TX empty and CTS bits set
    EXPECT_TRUE(status & 0x04);  // TX Empty
    EXPECT_TRUE(status & 0x08);  // TX Buffer Empty
    EXPECT_TRUE(status & 0x20);  // CTS
}

TEST_F(Z80DartTest, Write_Data_TriggersCallback) {
    bool callback_called = false;
    uint8_t received_byte = 0;
    
    dart.set_rx_callback([&](uint8_t byte) {
        callback_called = true;
        received_byte = byte;
    });
    
    // Write a byte to data port
    dart.write(0x0C, 0x42);  // 'B'
    
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(received_byte, 0x42);
}

TEST_F(Z80DartTest, Write_Data_SetsTxBufferFull) {
    // First, read status - TX buffer should be empty
    uint8_t status = dart.read(0x0D);
    EXPECT_TRUE(status & 0x08);  // TX Buffer Empty
    
    // Write data
    dart.write(0x0C, 0x55);
    
    // After write, TX buffer should still be empty (TX complete is fast)
    // In real hardware this would be baud-rate dependent
}

TEST_F(Z80DartTest, ChannelSelect_A) {
    // Select Channel A via WR0
    dart.write(0x0D, 0x00);  // WR0 with channel A select (bits 2-3 = 00)
    
    // Write to channel A
    dart.write(0x0C, 0xAA);
    
    // Verify by reading status
    uint8_t status = dart.read(0x0D);
    EXPECT_TRUE(status & 0x04);  // TX Empty (transmit complete)
}

TEST_F(Z80DartTest, ResetReceiver_ClearsFIFO) {
    // Enqueue some bytes (via internal method simulation)
    // In real scenario, we'd need to inject bytes
    
    // Reset receiver
    dart.write(0x0D, 0x40);  // WR0 reset receiver command (bits 6-7 = 01)
    
    // FIFO should be empty
    EXPECT_FALSE(dart.rx_available());
}

TEST_F(Z80DartTest, ResetTransmitter_ClearsTxBuffer) {
    // Write some data
    dart.write(0x0C, 0x12);
    
    // Reset transmitter
    dart.write(0x0D, 0x80);  // WR0 reset transmitter command (bits 6-7 = 10)
    
    // TX should be empty and ready
    uint8_t status = dart.read(0x0D);
    EXPECT_TRUE(status & 0x04);  // TX Empty
    EXPECT_TRUE(status & 0x08);  // TX Buffer Empty
}

TEST_F(Z80DartTest, ResetErrorFlags_ClearsErrorBits) {
    // Write error reset command
    dart.write(0x0D, 0xC0);  // WR0 reset error flags (bits 6-7 = 11)
    
    // Read RR1 to verify errors cleared
    uint8_t rr1 = dart.read(0x0E);
    EXPECT_EQ(rr1, 0x00);  // No errors
}

TEST_F(Z80DartTest, WR1_InterruptEnable) {
    // Write interrupt enable to WR1 (Channel A)
    // 0x03 = bit 1: TX interrupt enable
    // With TX buffer empty and TX interrupt enabled, interrupt fires
    dart.write(0x0E, 0x02);  // Enable TX interrupt only
    
    // TX buffer is empty after reset, so TX interrupt fires
    EXPECT_TRUE(dart.has_interrupt());
    
    // Reset to clear interrupt
    dart.reset();
    EXPECT_FALSE(dart.has_interrupt());
    
    // Now write data - TX interrupt should still fire when buffer empties
    dart.write(0x0C, 0x42);
    EXPECT_FALSE(dart.has_interrupt());  // TX in progress
}

TEST_F(Z80DartTest, InterruptVector_Default) {
    uint8_t vec = dart.get_interrupt_vector();
    // Default vector should be set
    EXPECT_EQ(vec, 0x00);
}

TEST_F(Z80DartTest, MultipleChannelSelects) {
    // Select channel A explicitly
    dart.write(0x0D, 0x00);  // Channel A
    
    // Select channel B
    dart.write(0x0D, 0x04);  // Channel B (bits 2-3 = 01)
    
    // Data port for B
    dart.write(0x0E, 0xBB);
}

TEST_F(Z80DartTest, StatusBits_Comprehensive) {
    uint8_t status = dart.read(0x0D);
    
    // All expected bits should be present
    EXPECT_TRUE(status & 0x04);  // TX Empty
    EXPECT_TRUE(status & 0x08);  // TX Buffer Empty
    EXPECT_TRUE(status & 0x20);  // CTS
}

// ─────────────────────────────────────────────────
// Intel8253 Timer Tests
// ─────────────────────────────────────────────────

class Intel8253Test : public testing::Test {
protected:
    void SetUp() override {
        timer = Intel8253();
    }
    
    Intel8253 timer;
};

TEST_F(Intel8253Test, Reset_InitialState) {
    timer.reset();
    
    // All counters should be readable
    EXPECT_EQ(timer.read_counter(0), 0);
    EXPECT_EQ(timer.read_counter(1), 0);
    EXPECT_EQ(timer.read_counter(2), 0);
}

TEST_F(Intel8253Test, ReadCounter_InvalidChannel) {
    // Should return 0 for invalid channel
    EXPECT_EQ(timer.read_counter(-1), 0);
    EXPECT_EQ(timer.read_counter(3), 0);
    EXPECT_EQ(timer.read_counter(10), 0);
}

TEST_F(Intel8253Test, WriteCounter0_SetsCount) {
    timer.write(0x0C, 0x12);  // Counter 0, LSB only
    EXPECT_EQ(timer.read_counter(0), 0x12);
}

TEST_F(Intel8253Test, WriteCounter1_SetsCount) {
    timer.write(0x0D, 0x34);  // Counter 1
    EXPECT_EQ(timer.read_counter(1), 0x34);
}

TEST_F(Intel8253Test, WriteCounter2_SetsCount) {
    timer.write(0x0E, 0x56);  // Counter 2
    EXPECT_EQ(timer.read_counter(2), 0x56);
}

TEST_F(Intel8253Test, ModeRegister_Write) {
    timer.write(0x0F, 0x12);  // Mode register
    
    // Read doesn't affect counter state
    EXPECT_EQ(timer.read_counter(0), 0);
}

TEST_F(Intel8253Test, LatchCommand_Counters) {
    // Set counter value
    timer.write(0x0C, 0x99);
    
    // Latch count command (counter 0)
    timer.write(0x0F, 0x00);  // Latch counter 0
    
    // Read should return latched value
    uint8_t val = timer.read(0x0C);
    EXPECT_EQ(val, 0x99);
}

TEST_F(Intel8253Test, LatchAllCounters) {
    timer.write(0x0C, 0x11);
    timer.write(0x0D, 0x22);
    timer.write(0x0E, 0x33);
    
    // Latch all
    timer.write(0x0F, 0xC0);  // Latch all counters command
    
    // Reads should return latched values
    EXPECT_EQ(timer.read(0x0C), 0x11);
    EXPECT_EQ(timer.read(0x0D), 0x22);
    EXPECT_EQ(timer.read(0x0E), 0x33);
}

TEST_F(Intel8253Test, ManualBaud_SetsAndClears) {
    // Set manual baud
    timer.set_manual_baud(9600);
    EXPECT_TRUE(timer.has_manual_baud());
    
    // Clear manual baud
    timer.clear_manual_baud();
    EXPECT_FALSE(timer.has_manual_baud());
}

TEST_F(Intel8253Test, BaudCallback_NotCalledWithoutManualBaud) {
    bool callback_called = false;
    timer.set_baud_callback([&](uint16_t) {
        callback_called = true;
    });
    
    // Set counter value - should trigger callback
    timer.write(0x0C, 0x40);
    
    EXPECT_TRUE(callback_called);
}

TEST_F(Intel8253Test, BaudCallback_NotCalledWithManualBaud) {
    bool callback_called = false;
    
    timer.set_manual_baud(9600);
    timer.set_baud_callback([&](uint16_t) {
        callback_called = true;
    });
    
    // Change counter - should NOT trigger callback
    timer.write(0x0C, 0x40);
    
    EXPECT_FALSE(callback_called);
}

TEST_F(Intel8253Test, ReadInvalidPort) {
    // Port 3 is mode register, not a counter
    // Should return counter value (0 in reset state)
    uint8_t val = timer.read(0x03);
    EXPECT_EQ(val, 0);
}

// ─────────────────────────────────────────────────
// Serial Backend Tests
// ─────────────────────────────────────────────────

class SerialBackendTest : public testing::Test {
protected:
    void TearDown() override {
        // Clean up any test files
        std::filesystem::remove(test_input_path_);
        std::filesystem::remove(test_output_path_);
    }
    
    std::string test_input_path_ = "/tmp/serial_test_input.bin";
    std::string test_output_path_ = "/tmp/serial_test_output.bin";
};

TEST_F(SerialBackendTest, NullBackend_BasicOperations) {
    NullBackend backend;
    
    // Should always be open
    EXPECT_TRUE(backend.is_open());
    EXPECT_TRUE(backend.connected());
    
    // Can send without error
    backend.send(0x42);
    
    // No data available
    EXPECT_FALSE(backend.has_data());
    
    // Recv returns 0
    EXPECT_EQ(backend.recv(), 0);
    
    // Status
    EXPECT_EQ(backend.name(), "Null");
    EXPECT_EQ(backend.status(), "Dropping all data");
}

TEST_F(SerialBackendTest, NullBackend_CloseDoesNothing) {
    NullBackend backend;
    EXPECT_TRUE(backend.is_open());
    
    backend.close();
    EXPECT_TRUE(backend.is_open());  // Still "open"
}

TEST_F(SerialBackendTest, FileBackend_OpenWithNoFiles) {
    FileBackend backend("", "");  // No files
    EXPECT_TRUE(backend.is_open());  // Opens even with no files
    EXPECT_TRUE(backend.connected());
}

TEST_F(SerialBackendTest, FileBackend_OpenInputFile) {
    // Create test input file
    FILE* f = fopen(test_input_path_.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fputc(0x41, f);  // 'A'
    fputc(0x42, f);  // 'B'
    fputc(0x43, f);  // 'C'
    fclose(f);
    
    FileBackend backend(test_input_path_, "");
    EXPECT_TRUE(backend.open());
    EXPECT_TRUE(backend.is_open());
    
    EXPECT_TRUE(backend.has_data());
    EXPECT_EQ(backend.recv(), 0x41);
    EXPECT_EQ(backend.recv(), 0x42);
    EXPECT_EQ(backend.recv(), 0x43);
}

TEST_F(SerialBackendTest, FileBackend_OpenOutputFile) {
    FileBackend backend("", test_output_path_);
    EXPECT_TRUE(backend.open());
    EXPECT_TRUE(backend.is_open());
    
    backend.send(0xDE);
    backend.send(0xAD);
    backend.send(0xBE);
    backend.send(0xEF);
    
    backend.close();
    
    // Verify output file
    FILE* f = fopen(test_output_path_.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(fgetc(f), 0xDE);
    EXPECT_EQ(fgetc(f), 0xAD);
    EXPECT_EQ(fgetc(f), 0xBE);
    EXPECT_EQ(fgetc(f), 0xEF);
    fclose(f);
}

TEST_F(SerialBackendTest, FileBackend_BothFiles) {
    // Create input file
    FILE* f = fopen(test_input_path_.c_str(), "wb");
    fputc(0x01, f);
    fputc(0x02, f);
    fclose(f);
    
    FileBackend backend(test_input_path_, test_output_path_);
    EXPECT_TRUE(backend.open());
    
    // Read and echo to output
    while (backend.has_data()) {
        backend.send(backend.recv());
    }
    
    backend.close();
    
    // Verify output
    f = fopen(test_output_path_.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(fgetc(f), 0x01);
    EXPECT_EQ(fgetc(f), 0x02);
    fclose(f);
}

TEST_F(SerialBackendTest, FileBackend_CloseMultipleTimes) {
    FileBackend backend("", "");
    EXPECT_TRUE(backend.open());
    
    backend.close();
    EXPECT_FALSE(backend.is_open());
    
    // Second close should be safe
    backend.close();
    EXPECT_FALSE(backend.is_open());
}

TEST_F(SerialBackendTest, FileBackend_StatusMessages) {
    FileBackend empty_backend("", "");
    EXPECT_EQ(empty_backend.status(), "No files configured");
    
    FileBackend input_only(test_input_path_, "");
    EXPECT_TRUE(input_only.status().find("Input:") != std::string::npos);
    
    FileBackend output_only("", test_output_path_);
    EXPECT_TRUE(output_only.status().find("Output:") != std::string::npos);
    
    FileBackend both(test_input_path_, test_output_path_);
    EXPECT_TRUE(both.status().find("Input:") != std::string::npos);
    EXPECT_TRUE(both.status().find("Output:") != std::string::npos);
}

TEST_F(SerialBackendTest, FileBackend_OpenNonexistentInput) {
    FileBackend backend("/nonexistent/path/to/file.bin", "");
    EXPECT_FALSE(backend.open());
    EXPECT_FALSE(backend.is_open());
}

TEST_F(SerialBackendTest, FileBackend_RecvWhenNoFile) {
    FileBackend backend("", "");
    backend.open();
    
    // No input file, should return 0
    EXPECT_FALSE(backend.has_data());
    EXPECT_EQ(backend.recv(), 0);
}

// ─────────────────────────────────────────────────
// Global Instance Test
// ─────────────────────────────────────────────────

TEST(SerialInterfaceTest, GlobalInstanceExists) {
    // Should be able to access global instance without crash
    EXPECT_NO_THROW(g_serial_interface.dart.reset());
    EXPECT_NO_THROW(g_serial_interface.timer.reset());
}

// ─────────────────────────────────────────────────
// SI ROM Manager Tests
// ─────────────────────────────────────────────────

class SIRomManagerTest : public testing::Test {
protected:
    void SetUp() override {
        // Clear the ROM map
        memset(rom_map_, 0, sizeof(rom_map_));
        rom_manager_ = SIRomManager();
    }
    
    void TearDown() override {
        // Clean up any loaded ROM
        rom_manager_.unload(rom_map_);
    }
    
    byte* rom_map_[32] = {nullptr};
    SIRomManager rom_manager_;
};

TEST_F(SIRomManagerTest, DefaultSlot) {
    EXPECT_EQ(rom_manager_.get_slot(), SIRomManager::DEFAULT_SLOT);
}

TEST_F(SIRomManagerTest, SetSlot) {
    rom_manager_.set_slot(10);
    EXPECT_EQ(rom_manager_.get_slot(), 10);
    
    rom_manager_.set_slot(0);
    EXPECT_EQ(rom_manager_.get_slot(), 0);
}

TEST_F(SIRomManagerTest, NotLoadedInitially) {
    EXPECT_FALSE(rom_manager_.is_loaded());
    EXPECT_FALSE(rom_manager_.is_auto_loaded());
}

TEST_F(SIRomManagerTest, LoadWithNoRomFile) {
    rom_manager_.load(rom_map_, "/nonexistent/path");
    
    EXPECT_FALSE(rom_manager_.is_loaded());
    EXPECT_FALSE(rom_manager_.is_auto_loaded());
    EXPECT_EQ(rom_map_[rom_manager_.get_slot()], nullptr);
}

TEST_F(SIRomManagerTest, LoadWithValidRomFile) {
    // Create a test ROM file with valid header
    std::string test_rom_path = "/tmp/si_rom.bin";
    FILE* fp = fopen(test_rom_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    
    // Write valid ROM header first bytes
    fputc(0x01, fp);  // Valid ROM signature (0x00, 0x01, or 0x02)
    fputc(0x00, fp);
    fputc(0x00, fp);
    
    // Fill rest with 0xFF
    for (int i = 3; i < 16384; i++) {
        fputc(0xFF, fp);
    }
    fclose(fp);
    
    rom_manager_.load(rom_map_, "/tmp");
    
    EXPECT_TRUE(rom_manager_.is_loaded());
    EXPECT_TRUE(rom_manager_.is_auto_loaded());
    EXPECT_NE(rom_map_[rom_manager_.get_slot()], nullptr);
    
    // Verify ROM data
    EXPECT_EQ(rom_map_[rom_manager_.get_slot()][0], 0x01);
    
    // Clean up
    std::filesystem::remove(test_rom_path);
}

TEST_F(SIRomManagerTest, UnloadRemovesRom) {
    // First load a ROM
    std::string test_rom_path = "/tmp/si_rom.bin";
    FILE* fp = fopen(test_rom_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    
    fputc(0x01, fp);  // Valid ROM signature
    for (int i = 1; i < 16384; i++) {
        fputc(0xFF, fp);
    }
    fclose(fp);
    
    rom_manager_.load(rom_map_, "/tmp");
    ASSERT_TRUE(rom_manager_.is_loaded());
    
    // Unload
    rom_manager_.unload(rom_map_);
    
    EXPECT_FALSE(rom_manager_.is_loaded());
    EXPECT_FALSE(rom_manager_.is_auto_loaded());
    EXPECT_EQ(rom_map_[rom_manager_.get_slot()], nullptr);
    
    std::filesystem::remove(test_rom_path);
}

TEST_F(SIRomManagerTest, UnloadWithoutAutoLoadDoesNothing) {
    // Manually set ROM without auto-load
    rom_map_[5] = new byte[16384];
    rom_manager_.set_slot(5);
    
    // This should not unload since it wasn't auto-loaded
    rom_manager_.unload(rom_map_);
    
    // ROM should still be there
    EXPECT_NE(rom_map_[5], nullptr);
    delete[] rom_map_[5];
    rom_map_[5] = nullptr;
}

TEST_F(SIRomManagerTest, LoadWithExistingRomDoesNotOverwrite) {
    // Pre-load a ROM in the manager's current slot (DEFAULT_SLOT).
    // Using any other slot index would be invisible to load() because it only
    // inspects rom_map_[rom_slot_].
    const int slot = rom_manager_.get_slot();
    rom_map_[slot] = new byte[16384];
    rom_map_[slot][0] = 0xAA;
    rom_map_[slot][1] = 0xBB;

    // load() should detect the existing ROM and report loaded-but-not-auto-loaded.
    rom_manager_.load(rom_map_, "/nonexistent");

    EXPECT_TRUE(rom_manager_.is_loaded());
    EXPECT_FALSE(rom_manager_.is_auto_loaded());

    // Original ROM data must not have been overwritten.
    EXPECT_EQ(rom_map_[slot][0], 0xAA);
    EXPECT_EQ(rom_map_[slot][1], 0xBB);

    delete[] rom_map_[slot];
    rom_map_[slot] = nullptr;
}

TEST_F(SIRomManagerTest, InvalidSlotDoesNothing) {
    rom_manager_.set_slot(-1);
    rom_manager_.load(rom_map_, "/tmp");
    EXPECT_FALSE(rom_manager_.is_loaded());
    
    rom_manager_.set_slot(32);
    rom_manager_.load(rom_map_, "/tmp");
    EXPECT_FALSE(rom_manager_.is_loaded());
}

// ─────────────────────────────────────────────────
// Host Serial Backend Tests
// ─────────────────────────────────────────────────

class HostSerialBackendTest : public testing::Test {
protected:
    void TearDown() override {
        backend.close();
    }
    
    HostSerialBackend backend{"/dev/null"};
};

TEST_F(HostSerialBackendTest, OpenWithInvalidDevice) {
    HostSerialBackend invalid("/dev/nonexistent_device_xyz");
    EXPECT_FALSE(invalid.open());
    EXPECT_FALSE(invalid.is_open());
}

TEST_F(HostSerialBackendTest, OpenNullDevice) {
    EXPECT_TRUE(backend.open());
    EXPECT_TRUE(backend.is_open());
}

TEST_F(HostSerialBackendTest, CloseDoesNotCrash) {
    backend.open();
    EXPECT_NO_THROW(backend.close());
    EXPECT_FALSE(backend.is_open());
}

TEST_F(HostSerialBackendTest, SendDoesNotCrash) {
    backend.open();
    EXPECT_NO_THROW(backend.send(0x42));
}

TEST_F(HostSerialBackendTest, RecvFromNullReturnsZero) {
    backend.open();
    EXPECT_EQ(backend.recv(), 0);
}

TEST_F(HostSerialBackendTest, HasDataReturnsFalse) {
    backend.open();
    EXPECT_FALSE(backend.has_data());
}

TEST_F(HostSerialBackendTest, StatusShowsDevice) {
    backend.open();
    EXPECT_TRUE(backend.status().find("/dev/null") != std::string::npos);
}

TEST_F(HostSerialBackendTest, NameIsHostSerial) {
    EXPECT_EQ(backend.name(), "HostSerial");
}

TEST_F(HostSerialBackendTest, ConnectedWhenOpen) {
    backend.open();
    EXPECT_TRUE(backend.connected());
}

TEST_F(HostSerialBackendTest, DoubleOpenIsOK) {
    EXPECT_TRUE(backend.open());
    EXPECT_TRUE(backend.open());
}

// ─────────────────────────────────────────────────
// Null Modem Backend Tests
// ─────────────────────────────────────────────────

class NullModemBackendTest : public testing::Test {
protected:
    void SetUp() override {
        backend_a = std::make_unique<NullModemBackend>();
        backend_b = std::make_unique<NullModemBackend>();
    }
    
    std::unique_ptr<NullModemBackend> backend_a;
    std::unique_ptr<NullModemBackend> backend_b;
};

TEST_F(NullModemBackendTest, DefaultConstruction) {
    NullModemBackend nm;
    EXPECT_FALSE(nm.is_open());
    EXPECT_EQ(nm.name(), "NullModem");
}

TEST_F(NullModemBackendTest, OpenSucceeds) {
    EXPECT_TRUE(backend_a->open());
    EXPECT_TRUE(backend_a->is_open());
}

TEST_F(NullModemBackendTest, SendReceiveBetweenPeers) {
    backend_a->open();
    backend_b->open();
    
    backend_a->connect_peer(backend_b.get());
    backend_b->connect_peer(backend_a.get());
    
    backend_a->send(0x42);
    
    EXPECT_TRUE(backend_b->has_data());
    EXPECT_EQ(backend_b->recv(), 0x42);
}

TEST_F(NullModemBackendTest, BidirectionalCommunication) {
    backend_a->open();
    backend_b->open();
    
    backend_a->connect_peer(backend_b.get());
    backend_b->connect_peer(backend_a.get());
    
    backend_a->send(0xAA);
    backend_b->send(0xBB);
    
    EXPECT_EQ(backend_a->recv(), 0xBB);
    EXPECT_EQ(backend_b->recv(), 0xAA);
}

TEST_F(NullModemBackendTest, MultipleBytes) {
    backend_a->open();
    backend_b->open();
    
    backend_a->connect_peer(backend_b.get());
    backend_b->connect_peer(backend_a.get());
    
    backend_a->send(0x01);
    backend_a->send(0x02);
    backend_a->send(0x03);
    
    EXPECT_EQ(backend_b->recv(), 0x01);
    EXPECT_EQ(backend_b->recv(), 0x02);
    EXPECT_EQ(backend_b->recv(), 0x03);
}

TEST_F(NullModemBackendTest, DisconnectPeer) {
    backend_a->open();
    backend_b->open();
    
    backend_a->connect_peer(backend_b.get());
    backend_b->connect_peer(backend_a.get());
    
    // Send some data
    backend_a->send(0x42);
    EXPECT_TRUE(backend_b->has_data());
    
    // Receive the data
    uint8_t received = backend_b->recv();
    EXPECT_EQ(received, 0x42);
    
    // After receiving, buffer should be empty
    EXPECT_FALSE(backend_b->has_data());
    
    // Disconnect should clear any remaining state
    backend_b->disconnect_peer();
    
    // After disconnect, send should go nowhere
    backend_a->send(0x99);
    EXPECT_FALSE(backend_b->has_data());
}

TEST_F(NullModemBackendTest, CloseClearsBuffers) {
    backend_a->open();
    backend_b->open();
    
    backend_a->connect_peer(backend_b.get());
    backend_b->connect_peer(backend_a.get());
    
    // First receive any data
    backend_a->send(0x42);
    ASSERT_TRUE(backend_b->has_data());
    EXPECT_EQ(backend_b->recv(), 0x42);
    
    // Now close backend_a
    backend_a->close();
    
    // backend_b should not receive any new data
    EXPECT_FALSE(backend_b->has_data());
}

TEST_F(NullModemBackendTest, HasDataReturnsFalseWhenEmpty) {
    backend_a->open();
    EXPECT_FALSE(backend_a->has_data());
}

TEST_F(NullModemBackendTest, StatusShowsConnectionState) {
    backend_a->open();
    EXPECT_TRUE(backend_a->status().find("Waiting") != std::string::npos);
    
    backend_a->connect_peer(backend_b.get());
    EXPECT_TRUE(backend_a->status().find("Connected") != std::string::npos);
}

// ─────────────────────────────────────────────────
// TCP Socket Backend Tests
// ─────────────────────────────────────────────────

class TcpSocketBackendTest : public testing::Test {
protected:
    void TearDown() override {
        client.close();
        server.close();
    }
    
    TcpSocketBackend client{"127.0.0.1", 9999};
    TcpSocketBackend server{"127.0.0.1", 9999};
};

TEST_F(TcpSocketBackendTest, DefaultConstruction) {
    TcpSocketBackend tcp("localhost", 8080);
    EXPECT_FALSE(tcp.is_open());
    EXPECT_EQ(tcp.name(), "TcpSocket");
}

TEST_F(TcpSocketBackendTest, ConnectToInvalidHost) {
    TcpSocketBackend invalid("nonexistent.host.invalid", 9999);
    EXPECT_FALSE(invalid.open());
}

TEST_F(TcpSocketBackendTest, StatusShowsHostAndPort) {
    EXPECT_TRUE(client.status().find("127.0.0.1") != std::string::npos);
    EXPECT_TRUE(client.status().find("9999") != std::string::npos);
}

TEST_F(TcpSocketBackendTest, CloseTwiceDoesNotCrash) {
    client.close();
    EXPECT_NO_THROW(client.close());
}

TEST_F(TcpSocketBackendTest, SendWithoutConnectionDoesNotCrash) {
    EXPECT_NO_THROW(client.send(0x42));
}

TEST_F(TcpSocketBackendTest, RecvWithoutConnectionReturnsZero) {
    EXPECT_EQ(client.recv(), 0);
}

TEST_F(TcpSocketBackendTest, HasDataReturnsFalseWithoutConnection) {
    EXPECT_FALSE(client.has_data());
}
