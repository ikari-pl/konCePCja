#include <gtest/gtest.h>
#include "koncepcja.h"
#include "configuration.h"
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

extern t_CPC CPC;
extern t_GateArray GateArray;
extern byte *pbRAM;
extern byte *membank_read[4];
extern byte *membank_write[4];
extern t_MemBankConfig membank_config;

class RamExpansionTest : public testing::Test {
 protected:
  unsigned int saved_ram_size;
  unsigned char saved_RAM_config;
  unsigned char saved_RAM_bank;
  unsigned char saved_RAM_ext;

  void SetUp() override {
    saved_ram_size = CPC.ram_size;
    saved_RAM_config = GateArray.RAM_config;
    saved_RAM_bank = GateArray.RAM_bank;
    saved_RAM_ext = GateArray.RAM_ext;
  }

  void TearDown() override {
    CPC.ram_size = saved_ram_size;
    GateArray.RAM_config = saved_RAM_config;
    GateArray.RAM_bank = saved_RAM_bank;
    GateArray.RAM_ext = saved_RAM_ext;
  }
};

TEST_F(RamExpansionTest, DefaultRamSizeIs128K) {
  std::string tmpfile = (std::filesystem::temp_directory_path() / "ram_test_default.cfg").string();
  {
    std::ofstream f(tmpfile);
    f << "[system]\nmodel=2\n";
  }
  t_CPC test_cpc;
  loadConfiguration(test_cpc, tmpfile);
  EXPECT_EQ(128u, test_cpc.ram_size);
  std::filesystem::remove(tmpfile);
}

TEST_F(RamExpansionTest, RamSize64K) {
  std::string tmpfile = (std::filesystem::temp_directory_path() / "ram_test_64.cfg").string();
  {
    std::ofstream f(tmpfile);
    f << "[system]\nmodel=0\nram_size=64\n";
  }
  t_CPC test_cpc;
  loadConfiguration(test_cpc, tmpfile);
  EXPECT_EQ(64u, test_cpc.ram_size);
  std::filesystem::remove(tmpfile);
}

TEST_F(RamExpansionTest, RamSize256K) {
  std::string tmpfile = (std::filesystem::temp_directory_path() / "ram_test_256.cfg").string();
  {
    std::ofstream f(tmpfile);
    f << "[system]\nmodel=2\nram_size=256\n";
  }
  t_CPC test_cpc;
  loadConfiguration(test_cpc, tmpfile);
  EXPECT_EQ(256u, test_cpc.ram_size);
  std::filesystem::remove(tmpfile);
}

TEST_F(RamExpansionTest, RamSize512K) {
  std::string tmpfile = (std::filesystem::temp_directory_path() / "ram_test_512.cfg").string();
  {
    std::ofstream f(tmpfile);
    f << "[system]\nmodel=2\nram_size=512\n";
  }
  t_CPC test_cpc;
  loadConfiguration(test_cpc, tmpfile);
  EXPECT_EQ(512u, test_cpc.ram_size);
  std::filesystem::remove(tmpfile);
}

TEST_F(RamExpansionTest, RamSize4160K_Yarek) {
  std::string tmpfile = (std::filesystem::temp_directory_path() / "ram_test_4160.cfg").string();
  {
    std::ofstream f(tmpfile);
    f << "[system]\nmodel=2\nram_size=4160\n";
  }
  t_CPC test_cpc;
  loadConfiguration(test_cpc, tmpfile);
  EXPECT_EQ(4160u, test_cpc.ram_size);
  std::filesystem::remove(tmpfile);
}

TEST_F(RamExpansionTest, InvalidRamSizeDefaultsTo128) {
  std::string tmpfile = (std::filesystem::temp_directory_path() / "ram_test_invalid.cfg").string();
  {
    std::ofstream f(tmpfile);
    f << "[system]\nmodel=2\nram_size=999\n";
  }
  t_CPC test_cpc;
  loadConfiguration(test_cpc, tmpfile);
  EXPECT_EQ(128u, test_cpc.ram_size);
  std::filesystem::remove(tmpfile);
}

TEST_F(RamExpansionTest, CPC6128MinRamIs128K) {
  std::string tmpfile = (std::filesystem::temp_directory_path() / "ram_test_6128_64.cfg").string();
  {
    std::ofstream f(tmpfile);
    f << "[system]\nmodel=2\nram_size=64\n";
  }
  t_CPC test_cpc;
  loadConfiguration(test_cpc, tmpfile);
  EXPECT_EQ(128u, test_cpc.ram_size);
  std::filesystem::remove(tmpfile);
}

TEST_F(RamExpansionTest, Banking64K_ForcesConfig0) {
  CPC.ram_size = 64;
  GateArray.RAM_config = 0xC7;
  GateArray.RAM_ext = 0;
  GateArray.RAM_bank = 0;
  ga_memory_manager();
  EXPECT_EQ(0u, GateArray.RAM_config);
  EXPECT_EQ(0u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, Banking128K_Bank0Config0) {
  CPC.ram_size = 128;
  GateArray.RAM_config = 0xC0;
  GateArray.RAM_ext = 0;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(0u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, Banking128K_BankOutOfRange) {
  CPC.ram_size = 128;
  GateArray.RAM_config = 0xC8;
  GateArray.RAM_ext = 0;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(0u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, Banking256K_ValidAndInvalid) {
  CPC.ram_size = 256;
  GateArray.RAM_ext = 0;

  GateArray.RAM_config = 0xD0;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(2u, GateArray.RAM_bank);

  GateArray.RAM_config = 0xD8;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(0u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, Banking512K_Bank6Valid_Bank7Invalid) {
  CPC.ram_size = 512;
  GateArray.RAM_ext = 0;

  GateArray.RAM_config = 0xF0;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(6u, GateArray.RAM_bank);

  GateArray.RAM_config = 0xF8;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(0u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, Banking576K_AllBanks0Through7) {
  CPC.ram_size = 576;
  GateArray.RAM_ext = 0;

  GateArray.RAM_config = 0xF8;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(7u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, Yarek4MB_StandardPortBackwardCompatible) {
  CPC.ram_size = 4160;
  GateArray.RAM_config = 0xC0;
  GateArray.RAM_ext = 0;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(0u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, Yarek4MB_ExtBank1) {
  CPC.ram_size = 4160;
  GateArray.RAM_config = 0xC0;
  GateArray.RAM_ext = 1;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(8u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, Yarek4MB_MaxBank63) {
  CPC.ram_size = 4160;
  GateArray.RAM_config = 0xF8;
  GateArray.RAM_ext = 7;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(63u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, Yarek4MB_MixedBits) {
  CPC.ram_size = 4160;
  GateArray.RAM_config = 0xE8;
  GateArray.RAM_ext = 3;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(29u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, Yarek4MB_ExtBitsIgnoredForSmallRam) {
  CPC.ram_size = 128;
  GateArray.RAM_config = 0xC0;
  GateArray.RAM_ext = 5;
  GateArray.RAM_bank = 255;
  ga_memory_manager();
  EXPECT_EQ(0u, GateArray.RAM_bank);
}

TEST_F(RamExpansionTest, InitBanking_Bank0_PointsToBase) {
  if (!pbRAM) GTEST_SKIP() << "pbRAM not allocated";
  ga_init_banking(membank_config, 0);
  EXPECT_EQ(pbRAM + 0 * 16384, membank_config[0][0]);
  EXPECT_EQ(pbRAM + 1 * 16384, membank_config[0][1]);
  EXPECT_EQ(pbRAM + 2 * 16384, membank_config[0][2]);
  EXPECT_EQ(pbRAM + 3 * 16384, membank_config[0][3]);
}

TEST_F(RamExpansionTest, InitBanking_Config4to7_MapsExpansionToSlot1) {
  if (!pbRAM) GTEST_SKIP() << "pbRAM not allocated";
  ga_init_banking(membank_config, 0);
  byte *expansion_base = pbRAM + (0 + 1) * 65536;
  EXPECT_EQ(expansion_base + 0 * 16384, membank_config[4][1]);
  EXPECT_EQ(expansion_base + 1 * 16384, membank_config[5][1]);
  EXPECT_EQ(expansion_base + 2 * 16384, membank_config[6][1]);
  EXPECT_EQ(expansion_base + 3 * 16384, membank_config[7][1]);
}
