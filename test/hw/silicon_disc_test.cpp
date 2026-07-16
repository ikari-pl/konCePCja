/* silicon_disc_test.cpp — the DK'Tronics Silicon Disc: NOT a bus Device but a
 * battery-backed region of expansion RAM at banks 4-7. Proves the two
 * defining properties: the CPU reaches banks 4-7 through the standard PAL
 * banking, and the contents survive a CPC reset (the battery). See
 * docs/hardware/silicon-disc-device.md. */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "subcycle/machine.h"

namespace {
std::vector<uint8_t> read_rom(const char* p) {
  std::vector<uint8_t> r;
  FILE* f = fopen(p, "rb");
  if (!f) return r;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n > 0) {
    r.resize(static_cast<size_t>(n));
    if (fread(r.data(), 1, r.size(), f) != r.size()) r.clear();
  }
  fclose(f);
  return r;
}
}  // namespace

TEST(SiliconDisc, EnableExposesBanks4To7AsBatteryBackedRam) {
  std::vector<uint8_t> rom = read_rom("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_rom("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));

  // A bare 6128 has only 64K expansion — bank 4 is not addressable and the
  // banking wraps it to bank 0.
  EXPECT_EQ(m.ram_size(), 0x10000u + 0x10000u);
  m.enable_silicon_disc(true);
  EXPECT_EQ(m.ram_size(), 0x10000u + subcycle::Machine::kSiliconEnd)
      << "the expansion grew to 512K to expose banks 4-7";

  // Load a known image into the battery region, then confirm the CPU sees it
  // when it banks bank 4 into the &4000 window (ram_config 0xE4).
  std::vector<uint8_t> img(subcycle::Machine::kSiliconSize, 0);
  img[0] = 0xA5;  // first byte of bank 4
  img[0x1234] = 0x5A;
  m.silicon_disc_load(img.data(), img.size());

  m.io_write(0x7F00, 0xE4);  // PAL: bank 4 -> &4000-&7FFF
  EXPECT_EQ(m.peek_mem(0x4000), 0xA5) << "the CPU reads the silicon region";
  EXPECT_EQ(m.peek_mem(0x5234), 0x5A);

  // The CPU writes it, and the write must survive a reset (the battery).
  m.poke_mem(0x4001, 0x77);
  m.reset();
  for (int i = 0; i < 3; ++i) m.run_frame();
  m.io_write(0x7F00, 0xE4);  // re-select bank 4 after reset
  EXPECT_EQ(m.peek_mem(0x4001), 0x77) << "battery-backed across reset";
  EXPECT_EQ(m.peek_mem(0x4000), 0xA5) << "and the rest of it too";

  // The host reads the region back out for persistence.
  std::vector<uint8_t> out(subcycle::Machine::kSiliconSize, 0);
  m.silicon_disc_save(out.data(), out.size());
  EXPECT_EQ(out[0], 0xA5);
  EXPECT_EQ(out[1], 0x77) << "the CPU's write is in the saved image";
}
