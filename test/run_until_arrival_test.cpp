// beads-1meo: `step to` onto a breakpoint of the caller's own at the target
// now and then answered ERR 409 stopped-elsewhere with PC=target in its own
// trailer. The walk (z80_run_until_ephemeral) learns of a stop two ways: the
// latched hit (the breakpoint-hit hook) and the announced stop. When the
// stop was visible before the hit was, the walk took it for an arrival by PC
// and never collected the hit, and the IPC side, which tells an arrival onto
// the caller's own breakpoint by the hit it was handed, called it elsewhere.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "koncepcja.h"
#include "subcycle_bridge.h"
#include "z80_view.h"

extern t_CPC CPC;
extern t_z80regs z80;

namespace {

bool g_hit_latched = false;
word g_hit_pc = 0;
bool g_announced_at_hook = false;

void latch_hit(word pc, bool /*watchpoint*/, uint64_t /*generation*/) {
  g_announced_at_hook = cpc_announced_stopped();
  g_hit_pc = pc;
  g_hit_latched = true;
}

void commit_breakpoint_stop(word pc) {
  CpcRunCause hit;
  hit.reason = CpcStopReason::Breakpoint;
  hit.has_pc = true;
  hit.pc = pc;
  hit.addr = pc;
  ASSERT_TRUE(cpc_commit_breakpoint_stop(cpc_resume_epoch(),
                                         z80_breakpoint_generation(), hit, pc,
                                         /*watchpoint=*/false));
}

}  // namespace

// The hit is latched before the stop is announced: whoever sees the stop
// finds the hit that made it.
TEST(BreakpointStopOrder, TheHitIsLatchedBeforeTheStopIsAnnounced) {
  g_hit_latched = false;
  g_announced_at_hook = true;
  cpc_pause();
  cpc_resume();
  ASSERT_FALSE(cpc_announced_stopped());
  z80_set_breakpoint_hit_hook(&latch_hit);
  commit_breakpoint_stop(0x6200);
  z80_set_breakpoint_hit_hook(nullptr);
  EXPECT_TRUE(g_hit_latched);
  EXPECT_TRUE(cpc_announced_stopped());
  EXPECT_FALSE(g_announced_at_hook)
      << "a walk that saw the stop could not find its hit";
  cpc_resume();
}

namespace {

// Brings the sub-cycle board up on a synthetic 32K system ROM that parks,
// so a walk has a machine; nothing clocks it, the test plays the runner.
class RunUntilArrivalTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() / "koncepcja-run-until-arrival";
    std::filesystem::create_directories(dir_);
    std::vector<char> rom(0x8000, 0);
    rom[0] = static_cast<char>(0x18);  // JR $
    rom[1] = static_cast<char>(0xFE);
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
    cpc_pause();
  }

  void TearDown() override {
    z80_set_breakpoint_hit_hook(nullptr);
    z80_record_probe_hit_source(false);
    if (started_) subcycle_bridge_stop();
    CPC.rom_path = saved_rom_path_;
    CPC.model = saved_model_;
    CPC.ram_size = saved_ram_;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    cpc_resume();
  }

  std::filesystem::path dir_;
  std::string saved_rom_path_;
  unsigned int saved_model_ = 0;
  unsigned int saved_ram_ = 0;
  bool started_ = false;
};

}  // namespace

// The user's breakpoint at the target fires just after the walk looked for a
// hit and found none: the next thing it sees is the stop. It must still be
// handed the hit, or it cannot tell its caller the stop was the arrival.
TEST_F(RunUntilArrivalTest, AStopSeenBeforeItsHitStillHandsOverTheHit) {
  constexpr word kTarget = 0x6200;
  g_hit_latched = false;
  z80_set_breakpoint_hit_hook(&latch_hit);
  int calls = 0;
  bool handed_over = false;
  word handed_pc = 0;
  auto consume = [&](uint16_t& pc, bool& watch) {
    ++calls;
    if (calls == 2) {
      // The frame runner, between this look and the walk's next one: the
      // CPU reaches the target, the user breakpoint there fires.
      z80.PC.w.l = kTarget;
      z80_record_probe_hit_source(true);
      commit_breakpoint_stop(kTarget);
      return false;
    }
    if (!g_hit_latched) return false;
    g_hit_latched = false;
    pc = g_hit_pc;
    watch = false;
    if (calls > 1) {
      handed_over = true;
      handed_pc = pc;
    }
    return true;
  };
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  EXPECT_EQ(Z80RunUntilResult::OtherBreak,
            z80_run_until_ephemeral(kTarget, deadline, consume, {}));
  EXPECT_TRUE(handed_over) << "the walk ended without collecting the hit";
  EXPECT_EQ(kTarget, handed_pc);
}
