/* Tests for the Serial Interface host side (the card itself is the rs232
 * Device: test/hw/rs232_test.cpp)
 *
 * - NullBackend: basic operations
 * - FileBackend: file I/O operations
 * - SerialInterface: config staging, the host<->card queues
 */

#include "serial_interface.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iterator>
#include <vector>

#include "file_size_limit.h"
#include "types.h"

// ─────────────────────────────────────────────────
// Serial Backend Tests
// ─────────────────────────────────────────────────

class SerialBackendTest : public testing::Test {
 protected:
  void TearDown() override {
    // Use error_code overload — on Windows a file left open by a
    // FileBackend without explicit close() would throw "file in use".
    std::error_code ec;
    std::filesystem::remove(test_input_path_, ec);
    std::filesystem::remove(test_output_path_, ec);
  }

  // Use temp_directory_path() — /tmp doesn't exist on MINGW/Windows.
  std::string test_input_path_ =
      (std::filesystem::temp_directory_path() / "serial_test_input.bin")
          .string();
  std::string test_output_path_ =
      (std::filesystem::temp_directory_path() / "serial_test_output.bin")
          .string();
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
  FileBackend backend("", "");     // No files
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

// Bytes the CPC transmits but the backend cannot deliver are counted, so
// `serial status` can report them (tx_dropped).
TEST_F(SerialBackendTest, HostTxCountsUndeliveredBytes) {
  SerialInterface si;
  si.host_tx(0x41);  // no backend at all
  EXPECT_EQ(1u, si.tx_dropped());

  NullBackend sink;
  si.backend = &sink;
  si.host_tx(0x42);  // delivered (and dropped by design, not by failure)
  EXPECT_EQ(1u, si.tx_dropped());

  FileBackend no_output("", "");
  ASSERT_TRUE(no_output.is_open());
  si.backend = &no_output;
  si.host_tx(0x43);
  EXPECT_EQ(2u, si.tx_dropped());
  si.backend = nullptr;
}

// beads-5os: a failing output stream must be reported, not swallowed.
TEST_F(SerialBackendTest, FileBackend_SendReportsWriteFailure) {
  FileBackend no_output("", "");
  ASSERT_TRUE(no_output.is_open());
  EXPECT_FALSE(no_output.send(0x42));  // nowhere to put it

  FileBackend ok("", test_output_path_);
  ASSERT_TRUE(ok.open());
  EXPECT_TRUE(ok.send(0x42));
  ok.close();

  if (!ScopedFileSizeLimit::supported()) {
    GTEST_SKIP() << "no per-process file size limit on this platform";
  }
  // The disk-full case: the file opens, then refuses to grow.
  FileBackend full("", test_output_path_);
  ASSERT_TRUE(full.open());
  bool first = true;
  bool second = true;
  {
    ScopedFileSizeLimit const limit(0);
    ASSERT_TRUE(limit.active());
    first = full.send(0x42);
    second = full.send(0x43);  // keeps failing (logged once, not per byte)
  }
  full.close();
  EXPECT_FALSE(first);
  EXPECT_FALSE(second);
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
  std::string test_rom_path =
      (std::filesystem::temp_directory_path() / "si_rom.bin").string();
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

  rom_manager_.load(rom_map_, std::filesystem::temp_directory_path().string());

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
  std::string test_rom_path =
      (std::filesystem::temp_directory_path() / "si_rom.bin").string();
  FILE* fp = fopen(test_rom_path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);

  fputc(0x01, fp);  // Valid ROM signature
  for (int i = 1; i < 16384; i++) {
    fputc(0xFF, fp);
  }
  fclose(fp);

  rom_manager_.load(rom_map_, std::filesystem::temp_directory_path().string());
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

  // load() should detect the existing ROM and report
  // loaded-but-not-auto-loaded.
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
  std::string tmp = std::filesystem::temp_directory_path().string();
  rom_manager_.set_slot(-1);
  rom_manager_.load(rom_map_, tmp);
  EXPECT_FALSE(rom_manager_.is_loaded());

  rom_manager_.set_slot(32);
  rom_manager_.load(rom_map_, tmp);
  EXPECT_FALSE(rom_manager_.is_loaded());
}

// ─────────────────────────────────────────────────
// Host Serial Backend Tests (POSIX only — stub on Windows always fails open())
// ─────────────────────────────────────────────────

#ifndef _WIN32
class HostSerialBackendTest : public testing::Test {
 protected:
  void TearDown() override { backend.close(); }

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
#endif  // !_WIN32

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

// ─────────────────────────────────────────────────
// SerialConfig equality + SerialInterface::config_applied()
//
// koncpc_rebuild_machine() runs for every settings change, not just serial
// ones (RAM size, CRTC type, model...). Before beads-o0iq item 3 fixed
// beads-related drift here, apply_config() ran unconditionally on every
// rebuild, truncating a File backend's output file / dropping a live
// TcpSocket connection even when serial settings never changed.
// config_applied() is the guard: only re-open the backend when the staged
// config actually differs from what's already applied.
// ─────────────────────────────────────────────────

TEST(SerialConfigEquality, IdenticalConfigsCompareEqual) {
  SerialConfig a;
  SerialConfig b;
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);
}

TEST(SerialConfigEquality, DiffersOnEnabled) {
  SerialConfig a;
  SerialConfig b;
  b.enabled = !a.enabled;
  EXPECT_FALSE(a == b);
  EXPECT_TRUE(a != b);
}

TEST(SerialConfigEquality, DiffersOnBackendType) {
  SerialConfig a;
  SerialConfig b;
  a.backend_type = SerialBackendType::Null;
  b.backend_type = SerialBackendType::TcpSocket;
  EXPECT_TRUE(a != b);
}

TEST(SerialConfigEquality, DiffersOnFileBackendPaths) {
  SerialConfig a;
  SerialConfig b = a;
  b.input_file = "different_input.bin";
  EXPECT_TRUE(a != b);

  SerialConfig c = a;
  c.output_file = "different_output.bin";
  EXPECT_TRUE(a != c);
}

TEST(SerialConfigEquality, DiffersOnTcpHostAndPort) {
  SerialConfig a;
  SerialConfig b = a;
  b.tcp_host = "192.168.1.1";
  EXPECT_TRUE(a != b);

  SerialConfig c = a;
  c.tcp_port = a.tcp_port + 1;
  EXPECT_TRUE(a != c);
}

TEST(SerialConfigEquality, DiffersOnBaudRate) {
  SerialConfig a;
  SerialConfig b = a;
  b.baud_rate = a.baud_rate + 1;
  EXPECT_TRUE(a != b);
}

class SerialInterfaceConfigTest : public testing::Test {
 protected:
  SerialInterface iface;
};

TEST_F(SerialInterfaceConfigTest, NeverAppliedStartsFalse) {
  EXPECT_FALSE(iface.config_applied());
}

TEST_F(SerialInterfaceConfigTest, ApplyMakesItTrue) {
  SerialConfig cfg;
  cfg.enabled = false;  // Null/disabled backend: no real I/O in a unit test
  iface.set_config(cfg);
  iface.apply_config();
  EXPECT_TRUE(iface.config_applied());
}

TEST_F(SerialInterfaceConfigTest, SettingAnIdenticalConfigStaysApplied) {
  SerialConfig cfg;
  cfg.enabled = false;
  iface.set_config(cfg);
  iface.apply_config();
  ASSERT_TRUE(iface.config_applied());

  // Re-staging the exact same values (e.g. re-reading them from the same
  // Options fields) must not report the backend as needing a rebuild.
  iface.set_config(cfg);
  EXPECT_TRUE(iface.config_applied());
}

TEST_F(SerialInterfaceConfigTest, StagingADifferentConfigClearsApplied) {
  SerialConfig cfg;
  cfg.enabled = false;
  iface.set_config(cfg);
  iface.apply_config();
  ASSERT_TRUE(iface.config_applied());

  cfg.baud_rate = 19200;
  iface.set_config(cfg);
  EXPECT_FALSE(iface.config_applied())
      << "an actually-changed config must be reported as not-yet-applied, "
         "so callers know a rebuild's re-open is warranted";
}

TEST_F(SerialInterfaceConfigTest, ReapplyingAfterChangeRestoresApplied) {
  SerialConfig cfg;
  cfg.enabled = false;
  iface.set_config(cfg);
  iface.apply_config();

  cfg.baud_rate = 19200;
  iface.set_config(cfg);
  ASSERT_FALSE(iface.config_applied());

  iface.apply_config();
  EXPECT_TRUE(iface.config_applied());
}

// ─────────────────────────────────────────────────
// Host <-> card queues (beads-2myi): the IPC server and the Serial Terminal
// reach the board's rs232 Device only through these.
// ─────────────────────────────────────────────────

namespace {

// A backend with bytes waiting to be received, and nothing else.
class ScriptedRxBackend : public NullBackend {
 public:
  explicit ScriptedRxBackend(std::vector<uint8_t> rx)
      : rx_(rx.begin(), rx.end()) {}
  bool has_data() const override { return !rx_.empty(); }
  uint8_t recv() override {
    uint8_t const b = rx_.front();
    rx_.pop_front();
    return b;
  }
  size_t left() const { return rx_.size(); }

 private:
  std::deque<uint8_t> rx_;
};

void collect(uint8_t byte, void* ctx) {
  static_cast<std::vector<uint8_t>*>(ctx)->push_back(byte);
}

const uint8_t kHello[] = {'H', 'E', 'L', 'L', 'O'};

}  // namespace

TEST(SerialHostQueues, PumpNeverHandsTheCardMoreThanItHasRoomFor) {
  SerialInterface si;
  ASSERT_EQ(5u, si.queue_rx(kHello, sizeof(kHello)));
  EXPECT_EQ(5u, si.rx_pending());

  std::vector<uint8_t> card;
  EXPECT_EQ(3u, si.pump_rx(3, collect, &card));  // a three-deep FIFO
  EXPECT_EQ((std::vector<uint8_t>{'H', 'E', 'L'}), card);
  EXPECT_EQ(2u, si.rx_pending());

  EXPECT_EQ(0u, si.pump_rx(0, collect, &card));  // FIFO still full
  EXPECT_EQ(3u, card.size());

  EXPECT_EQ(2u, si.pump_rx(3, collect, &card));
  EXPECT_EQ((std::vector<uint8_t>{'H', 'E', 'L', 'L', 'O'}), card);
  EXPECT_EQ(0u, si.rx_pending());
}

TEST(SerialHostQueues, QueuedBytesGoBeforeTheBackendsAndTheRestWait) {
  SerialInterface si;
  ScriptedRxBackend wire({'x', 'y', 'z'});
  si.backend = &wire;
  const uint8_t typed[] = {'A', 'B'};
  si.queue_rx(typed, sizeof(typed));

  std::vector<uint8_t> card;
  EXPECT_EQ(3u, si.pump_rx(3, collect, &card));
  EXPECT_EQ((std::vector<uint8_t>{'A', 'B', 'x'}), card);
  // The backend keeps what did not fit, instead of overrunning the FIFO.
  EXPECT_EQ(2u, wire.left());
  si.backend = nullptr;
}

TEST(SerialHostQueues, QueueIsBoundedAndReportsWhatFitted) {
  SerialInterface si;
  std::vector<uint8_t> big(SerialInterface::kRxQueueCap - 2, 0x55);
  ASSERT_EQ(big.size(), si.queue_rx(big.data(), big.size()));
  EXPECT_EQ(2u, si.queue_rx(kHello, sizeof(kHello)));
  EXPECT_EQ(SerialInterface::kRxQueueCap, si.rx_pending());
}

TEST(SerialHostQueues, DisablingTheCardDropsWhatWasQueuedForIt) {
  SerialInterface si;
  si.queue_rx(kHello, sizeof(kHello));
  SerialConfig cfg;
  cfg.enabled = false;
  si.set_config(cfg);
  si.apply_config();
  EXPECT_EQ(0u, si.rx_pending());
}

TEST(SerialHostQueues, HostWiredOnlyWhenEnabledAndNotThePlotter) {
  SerialInterface si;
  SerialConfig cfg;
  cfg.enabled = false;
  si.set_config(cfg);
  EXPECT_FALSE(si.host_wired());
  cfg.enabled = true;
  cfg.backend_type = SerialBackendType::Null;
  si.set_config(cfg);
  EXPECT_TRUE(si.host_wired());
  cfg.backend_type = SerialBackendType::Plotter;  // a Device on that wire
  si.set_config(cfg);
  EXPECT_FALSE(si.host_wired());
}

TEST(SerialHostQueues, TransmittedBytesReachTheTerminalMonitor) {
  SerialInterface si;
  NullBackend sink;
  si.backend = &sink;
  for (uint8_t const b : kHello) si.host_tx(b);
  si.backend = nullptr;

  std::vector<uint8_t> shown;
  si.drain_monitor(shown);
  EXPECT_EQ(std::vector<uint8_t>(std::begin(kHello), std::end(kHello)), shown);
  si.drain_monitor(shown);  // drained: nothing new
  EXPECT_EQ(5u, shown.size());
}

TEST(SerialHostQueues, MonitorKeepsTheNewestBytesWhileNobodyDrains) {
  SerialInterface si;
  for (size_t i = 0; i < SerialInterface::kMonitorCap + 2; ++i)
    si.host_tx(static_cast<uint8_t>(i));

  std::vector<uint8_t> shown;
  si.drain_monitor(shown);
  ASSERT_EQ(SerialInterface::kMonitorCap, shown.size());
  EXPECT_EQ(2, shown.front());
  EXPECT_EQ(static_cast<uint8_t>(SerialInterface::kMonitorCap + 1),
            shown.back());
}
