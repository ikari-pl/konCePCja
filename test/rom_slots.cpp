#include <gtest/gtest.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "koncepcja.h"
#include "koncepcja_ipc_server.h"
#include "z80.h"

extern t_z80regs z80;
extern t_CPC CPC;
extern byte *membank_read[4];
extern byte *membank_write[4];
extern byte *memmap_ROM[256];
extern byte *pbExpansionROM;
extern byte *pbROMhi;
extern t_GateArray GateArray;

namespace {

constexpr int kPort = 6543;
constexpr size_t kBankSize = 16 * 1024;

std::string send_command(const std::string& command) {
#ifdef _WIN32
  SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  EXPECT_NE(fd, INVALID_SOCKET);
  if (fd == INVALID_SOCKET) return "";
#else
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);
  if (fd < 0) return "";
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  bool connected = false;
  for (int attempt = 0; attempt < 100 && !connected; attempt++) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(connected);
  if (!connected) {
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
    return "";
  }

  std::string line = command + "\n";
#ifdef _WIN32
  int written = send(fd, line.data(), static_cast<int>(line.size()), 0);
  EXPECT_EQ(written, static_cast<int>(line.size()));
#else
  ssize_t written = ::write(fd, line.data(), line.size());
  EXPECT_EQ(written, static_cast<ssize_t>(line.size()));
#endif

  std::string response;
  char buffer[4096];
#ifdef _WIN32
  int n = 0;
  while ((n = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
    response.append(buffer, buffer + n);
  }
  closesocket(fd);
#else
  ssize_t n = 0;
  while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
    response.append(buffer, buffer + n);
  }
  ::close(fd);
#endif
  return response;
}

// Create a valid CPC ROM file (16K, type byte 0x01 = background ROM)
std::string create_test_rom(const std::filesystem::path& dir, const std::string& name, byte type_byte = 0x01) {
  auto path = dir / name;
  std::vector<byte> rom(16384, 0x00);
  rom[0] = type_byte;
  rom[1] = 0x01;
  rom[2] = 0x01;
  for (int i = 16; i < 128; i++) {
    rom[i] = static_cast<byte>(i & 0xFF);
  }
  std::ofstream ofs(path.string(), std::ios::binary);
  ofs.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
  ofs.close();
  return path.string();
}

class RomSlotsTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    CPC.snd_enabled = 0;
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  static void TearDownTestSuite() {
    server.stop();
#ifdef _WIN32
    WSACleanup();
#endif
  }

  void SetUp() override {
    z80 = t_z80regs();
    for (int i = 0; i < 4; i++) {
      std::memset(memory[i], 0, kBankSize);
      membank_read[i] = memory[i];
      membank_write[i] = memory[i];
    }
    for (int i = 2; i < 32; i++) {
      if (memmap_ROM[i] != nullptr) {
        delete[] memmap_ROM[i];
        memmap_ROM[i] = nullptr;
      }
      CPC.rom_file[i] = "";
    }
    GateArray.ROM_config = 0x0C;
    GateArray.upper_ROM = 0;
    pbExpansionROM = memory[3];
    pbROMhi = memory[3];
    tmp_dir = std::filesystem::temp_directory_path() / "koncepcja_rom_test";
    std::filesystem::create_directories(tmp_dir);
  }

  void TearDown() override {
    for (int i = 2; i < 32; i++) {
      if (memmap_ROM[i] != nullptr) {
        delete[] memmap_ROM[i];
        memmap_ROM[i] = nullptr;
      }
      CPC.rom_file[i] = "";
    }
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
  }

  static KoncepcjaIpcServer server;
  static byte memory[4][kBankSize];
  std::filesystem::path tmp_dir;
};

KoncepcjaIpcServer RomSlotsTest::server;
byte RomSlotsTest::memory[4][kBankSize];

TEST_F(RomSlotsTest, RomListShowsAllSlots) {
  auto resp = send_command("rom list");
  EXPECT_TRUE(resp.find("OK") == 0);
  EXPECT_TRUE(resp.find("0=(empty)") != std::string::npos);
  EXPECT_TRUE(resp.find("31=(empty)") != std::string::npos);
}

TEST_F(RomSlotsTest, RomListShowsLoadedSlot) {
  auto rom_path = create_test_rom(tmp_dir, "test.rom");
  auto resp = send_command("rom load 10 " + rom_path);
  EXPECT_EQ(resp, "OK\n");
  resp = send_command("rom list");
  EXPECT_TRUE(resp.find("10=" + rom_path) != std::string::npos);
}

TEST_F(RomSlotsTest, RomLoadSlot0to31) {
  for (int slot : {2, 7, 15, 16, 24, 31}) {
    auto rom_path = create_test_rom(tmp_dir, "rom_slot_" + std::to_string(slot) + ".rom");
    auto resp = send_command("rom load " + std::to_string(slot) + " " + rom_path);
    EXPECT_EQ(resp, "OK\n") << "Failed loading slot " << slot;
    EXPECT_NE(memmap_ROM[slot], nullptr) << "ROM data null for slot " << slot;
    EXPECT_EQ(CPC.rom_file[slot], rom_path) << "rom_file wrong for slot " << slot;
  }
}

TEST_F(RomSlotsTest, RomLoadRejectsSlot32) {
  auto rom_path = create_test_rom(tmp_dir, "test.rom");
  auto resp = send_command("rom load 32 " + rom_path);
  EXPECT_TRUE(resp.find("ERR 400 slot must be 0-31") != std::string::npos);
}

TEST_F(RomSlotsTest, RomLoadRejectsNegativeSlot) {
  auto rom_path = create_test_rom(tmp_dir, "test.rom");
  auto resp = send_command("rom load -1 " + rom_path);
  EXPECT_TRUE(resp.find("ERR 400 slot must be 0-31") != std::string::npos);
}

TEST_F(RomSlotsTest, RomLoadFileNotFound) {
  auto resp = send_command("rom load 10 /nonexistent/path/rom.bin");
  EXPECT_TRUE(resp.find("ERR 404") != std::string::npos);
}

TEST_F(RomSlotsTest, RomLoadInvalidRom) {
  auto path = tmp_dir / "bad.rom";
  std::vector<byte> bad_rom(16384, 0xFF);
  std::ofstream ofs(path.string(), std::ios::binary);
  ofs.write(reinterpret_cast<const char*>(bad_rom.data()), static_cast<std::streamsize>(bad_rom.size()));
  ofs.close();
  auto resp = send_command("rom load 10 " + path.string());
  EXPECT_TRUE(resp.find("ERR 400 not-a-valid-rom") != std::string::npos);
}

TEST_F(RomSlotsTest, RomLoadReplacesExisting) {
  auto rom1 = create_test_rom(tmp_dir, "rom1.rom");
  auto rom2 = create_test_rom(tmp_dir, "rom2.rom", 0x02);
  auto resp = send_command("rom load 10 " + rom1);
  EXPECT_EQ(resp, "OK\n");
  EXPECT_EQ(CPC.rom_file[10], rom1);
  resp = send_command("rom load 10 " + rom2);
  EXPECT_EQ(resp, "OK\n");
  EXPECT_EQ(CPC.rom_file[10], rom2);
}

TEST_F(RomSlotsTest, RomUnloadSlot) {
  auto rom_path = create_test_rom(tmp_dir, "test.rom");
  send_command("rom load 10 " + rom_path);
  EXPECT_NE(memmap_ROM[10], nullptr);
  auto resp = send_command("rom unload 10");
  EXPECT_EQ(resp, "OK\n");
  EXPECT_EQ(memmap_ROM[10], nullptr);
  EXPECT_EQ(CPC.rom_file[10], "");
}

TEST_F(RomSlotsTest, RomUnloadRejectsSystemSlots) {
  auto resp = send_command("rom unload 0");
  EXPECT_TRUE(resp.find("ERR 400 cannot-unload-system-rom") != std::string::npos);
  resp = send_command("rom unload 1");
  EXPECT_TRUE(resp.find("ERR 400 cannot-unload-system-rom") != std::string::npos);
}

TEST_F(RomSlotsTest, RomUnloadEmptySlotIsOk) {
  auto resp = send_command("rom unload 20");
  EXPECT_EQ(resp, "OK\n");
}

TEST_F(RomSlotsTest, RomUnloadRejectsSlot32) {
  auto resp = send_command("rom unload 32");
  EXPECT_TRUE(resp.find("ERR 400 slot must be 0-31") != std::string::npos);
}

TEST_F(RomSlotsTest, RomInfoEmptySlot) {
  auto resp = send_command("rom info 20");
  EXPECT_TRUE(resp.find("OK slot=20 loaded=false") != std::string::npos);
}

TEST_F(RomSlotsTest, RomInfoLoadedSlot) {
  auto rom_path = create_test_rom(tmp_dir, "test.rom");
  send_command("rom load 10 " + rom_path);
  auto resp = send_command("rom info 10");
  EXPECT_TRUE(resp.find("OK slot=10 loaded=true") != std::string::npos);
  EXPECT_TRUE(resp.find("size=16384") != std::string::npos);
  EXPECT_TRUE(resp.find("crc=") != std::string::npos);
  EXPECT_TRUE(resp.find("path=") != std::string::npos);
}

TEST_F(RomSlotsTest, RomInfoRejectsSlot32) {
  auto resp = send_command("rom info 32");
  EXPECT_TRUE(resp.find("ERR 400 slot must be 0-31") != std::string::npos);
}

TEST_F(RomSlotsTest, ArraySizeIs32) {
  for (int i = 0; i < 32; i++) {
    CPC.rom_file[i] = "slot_" + std::to_string(i);
  }
  for (int i = 0; i < 32; i++) {
    EXPECT_EQ(CPC.rom_file[i], "slot_" + std::to_string(i));
  }
  for (int i = 0; i < 32; i++) {
    CPC.rom_file[i] = "";
  }
}

TEST_F(RomSlotsTest, BackwardCompatibility16SlotConfig) {
  for (int i = 0; i < 16; i++) {
    CPC.rom_file[i] = "legacy_rom_" + std::to_string(i);
  }
  for (int i = 16; i < 32; i++) {
    CPC.rom_file[i] = "";
  }
  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(CPC.rom_file[i], "legacy_rom_" + std::to_string(i));
  }
  for (int i = 16; i < 32; i++) {
    EXPECT_EQ(CPC.rom_file[i], "");
  }
  for (int i = 0; i < 32; i++) {
    CPC.rom_file[i] = "";
  }
}

TEST_F(RomSlotsTest, RomBadSubcommand) {
  auto resp = send_command("rom bogus");
  EXPECT_TRUE(resp.find("ERR 400 bad-rom-cmd") != std::string::npos);
}

}  // namespace
