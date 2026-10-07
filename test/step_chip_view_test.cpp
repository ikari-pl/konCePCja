// beads-szc0: a debugger step must refresh the host chip views, not just the
// registers. The IPC context line (rom:LO,...), the Gate Array / CRTC / PSG
// DevTools windows and the disassembly cache all read the GateArray / CRTC /
// PSG mirrors, which subcycle_bridge_debug_sync() used to publish only once
// per frame. While paused nothing runs a frame, so after stepping the
// firmware's ROM-switching OUT the context still said rom:LO while the CPU
// view had the RAM paged in (726 of 1500 stops on a 6128+ in the e2e probe).

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "hw/gate_array.h"
#include "koncepcja.h"
#include "subcycle/machine.h"
#include "subcycle_bridge.h"
#include "z80_view.h"

extern t_CPC CPC;
extern t_GateArray GateArray;

namespace {

// Brings the sub-cycle board up on a synthetic 32K system ROM whose reset
// code disables both ROMs with OUT (C),C and parks, and tears it down again
// so no other test sees an active bridge.
class StepChipViewTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / "koncepcja-step-chip-view";
    std::filesystem::create_directories(dir_);
    std::vector<char> rom(0x8000, 0);
    const unsigned char prog[] = {0x01, 0x8D, 0x7F,  // LD BC,&7F8D
                                  0xED, 0x49,        // OUT (C),C
                                  0x18, 0xFE};       // JR $
    for (size_t i = 0; i < sizeof(prog); ++i)
      rom[i] = static_cast<char>(prog[i]);
    std::filesystem::path const rom_file = dir_ / "cpc6128.rom";
    FILE* f = fopen(rom_file.string().c_str(), "wb");
    ASSERT_NE(nullptr, f);
    ASSERT_EQ(rom.size(), fwrite(rom.data(), 1, rom.size(), f));
    ASSERT_EQ(0, fclose(f));
    saved_rom_path_ = CPC.rom_path;
    saved_model_ = CPC.model;
    saved_ram_ = CPC.ram_size;
    CPC.rom_path = dir_.string();
    CPC.model = 2;  // chROMFile[2] == "cpc6128.rom"
    CPC.ram_size = 128;
    ASSERT_TRUE(subcycle_bridge_start());
    started_ = true;
  }

  void TearDown() override {
    if (started_) subcycle_bridge_stop();
    CPC.rom_path = saved_rom_path_;
    CPC.model = saved_model_;
    CPC.ram_size = saved_ram_;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::string saved_rom_path_;
  unsigned int saved_model_ = 0;
  unsigned int saved_ram_ = 0;
  bool started_ = false;
};

}  // namespace

TEST_F(StepChipViewTest, StepAcrossRomSwitchOutRefreshesTheGateArrayView) {
  subcycle::Machine* m = subcycle_bridge_machine();
  ASSERT_NE(nullptr, m);
  m->reset();
  GateArray.ROM_config = 0;  // the view as a frame before the step left it

  z80_step_instruction();  // LD BC,&7F8D
  z80_step_instruction();  // OUT (C),C
  ASSERT_EQ(0x0005, m->regs().pc);

  GateArrayRegs ga{};
  ga_peek(m->gate_array(), &ga);
  ASSERT_EQ(0x8D, ga.rom_config) << "the Gate Array latched the OUT";
  EXPECT_EQ(ga.rom_config, GateArray.ROM_config)
      << "the host view (IPC rom:, DevTools) must follow the step";
  EXPECT_EQ(0x00, m->peek_mem(0x0000)) << "lower ROM off: RAM at &0000";
}
