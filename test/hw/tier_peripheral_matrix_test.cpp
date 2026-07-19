/* tier_peripheral_matrix_test.cpp — plugged expansions without wake contracts
 * must degrade Fast/Wake requests to Faithful while keeping the machine stable.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

#include "subcycle/machine.h"

namespace {

std::vector<uint8_t> read_rom() {
  auto read_file = [](const char* p) {
    std::vector<uint8_t> out;
    FILE* f = fopen(p, "rb");
    if (f == nullptr) return out;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
  };
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  return rom;
}

void boot_fast(subcycle::Machine& m, const std::vector<uint8_t>& rom) {
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  m.set_run_tier(subcycle::Machine::RunTier::Fast);
  m.run_frame();
}

}  // namespace

TEST(TierPeripheralMatrix, LightGunKeepsFastTier) {
  const std::vector<uint8_t> rom = read_rom();
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine m;
  boot_fast(m, rom);
  m.set_light_gun(1, 100, 20, false);
  m.run_frame();
  // The light gun carries a wake + Fast batch contract (wake_slot /
  // fs_advance_chars dispatch it), so plugging no longer degrades the tier.
  EXPECT_EQ(m.effective_run_tier(), subcycle::Machine::RunTier::Fast);
  for (int i = 0; i < 5; ++i) m.run_frame();
  EXPECT_EQ(m.effective_run_tier(), subcycle::Machine::RunTier::Fast);
}

TEST(TierPeripheralMatrix, SymbifaceDegradesFastToFaithful) {
  const std::vector<uint8_t> rom = read_rom();
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine m;
  boot_fast(m, rom);
  m.set_symbiface(true);
  m.run_frame();
  EXPECT_EQ(m.effective_run_tier(), subcycle::Machine::RunTier::Faithful);
}

TEST(TierPeripheralMatrix, Mf2DegradesFastToFaithful) {
  const std::vector<uint8_t> rom = read_rom();
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine m;
  boot_fast(m, rom);
  m.set_mf2(true);
  m.run_frame();
  EXPECT_EQ(m.effective_run_tier(), subcycle::Machine::RunTier::Faithful);
}
