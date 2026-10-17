// beads-m9dq: the startup manifest reports the tier the machine resolved to
// (machine.effective_tier), so a harness knows whether per-cycle
// observability is on. The tier policy used to be applied only on entry to a
// frame, so a machine that had just been built still ran (and reported) the
// tier it was left in, or Faithful before it was built at all. A freshly
// started machine must already be in the tier the policy chooses.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "koncepcja.h"
#include "subcycle_bridge.h"
#include "z80_view.h"

extern t_CPC CPC;

namespace {

class BridgeStartTierTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / "koncepcja-start-tier";
    std::filesystem::create_directories(dir_);
    std::vector<char> rom(0x8000, 0);
    std::filesystem::path const rom_file = dir_ / "cpc6128.rom";
    FILE* f = fopen(rom_file.string().c_str(), "wb");
    ASSERT_NE(nullptr, f);
    ASSERT_EQ(rom.size(), fwrite(rom.data(), 1, rom.size(), f));
    ASSERT_EQ(0, fclose(f));
    saved_rom_path_ = CPC.rom_path;
    saved_model_ = CPC.model;
    saved_ram_ = CPC.ram_size;
    saved_policy_ = subcycle_bridge_tier_policy();
    CPC.rom_path = dir_.string();
    CPC.model = 2;  // chROMFile[2] == "cpc6128.rom"
    CPC.ram_size = 128;
    z80_clear_breakpoints();
    if (subcycle_bridge_tier_env_pinned() != 0)
      GTEST_SKIP() << "KONCPC_TIER / KONCPC_WAKE pins the tier";
  }

  void TearDown() override {
    if (started_) subcycle_bridge_stop();
    z80_clear_breakpoints();
    subcycle_bridge_set_tier_policy(saved_policy_);
    CPC.rom_path = saved_rom_path_;
    CPC.model = saved_model_;
    CPC.ram_size = saved_ram_;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  // Builds the machine and reports its tier before it has run a frame.
  std::string tier_after_start(BridgeTierPolicy policy) {
    if (started_) subcycle_bridge_stop();
    started_ = false;
    subcycle_bridge_set_tier_policy(policy);
    if (!subcycle_bridge_start()) return "start failed";
    started_ = true;
    return subcycle_bridge_effective_tier_name();
  }

  std::filesystem::path dir_;
  std::string saved_rom_path_;
  unsigned int saved_model_ = 0;
  unsigned int saved_ram_ = 0;
  BridgeTierPolicy saved_policy_ = BridgeTierPolicy::Auto;
  bool started_ = false;
};

TEST_F(BridgeStartTierTest, AStartedMachineIsInThePolicysTier) {
  EXPECT_EQ("faithful", tier_after_start(BridgeTierPolicy::Faithful));
  EXPECT_EQ("fast", tier_after_start(BridgeTierPolicy::Auto));
  EXPECT_EQ("wake", tier_after_start(BridgeTierPolicy::Wake));
  EXPECT_EQ("fast", tier_after_start(BridgeTierPolicy::Fast));
}

// Auto drops to Wake while a breakpoint is set: a breakpoint loaded before
// the machine is built must count from the first frame, and in the manifest.
TEST_F(BridgeStartTierTest, AutoWithABreakpointStartsInWake) {
  z80_add_breakpoint(0x4000);
  EXPECT_EQ("wake", tier_after_start(BridgeTierPolicy::Auto));
  z80_clear_breakpoints();
  EXPECT_EQ("fast", tier_after_start(BridgeTierPolicy::Auto));
}

}  // namespace
