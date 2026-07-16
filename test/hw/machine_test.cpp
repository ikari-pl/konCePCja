/* machine_test.cpp — the embeddable sub-cycle CPC (src/subcycle/machine.h),
 * the Milestone-B seam's sub-cycle side: boot the real firmware, type through
 * the shadow-matrix key API, and pull video + audio, all through the one
 * object an application loop gets to see. Skipped without rom/cpc6128.rom. */

#include "subcycle/machine.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "hw/fdc.h"
#include "hw/gate_array.h"
#include "hw/probe.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

// Tap one packed (row << 4 | bit) key: hold across scans, release with a gap.
void tap(subcycle::Machine& m, uint8_t code) {
  m.key(code, true);
  for (int i = 0; i < 4; ++i) m.run_frame();
  m.key(code, false);
  for (int i = 0; i < 4; ++i) m.run_frame();
}

}  // namespace

TEST(SubcycleMachine, BootsTypesAndSounds) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  std::vector<uint8_t> fb(
      static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3, 0);
  m.attach_framebuffer(fb.data(), subcycle::kFbWidth, subcycle::kFbHeight);

  for (int i = 0; i < 120; ++i) m.run_frame();  // to the Ready screen
  int nonzero = 0;
  for (uint8_t v : fb)
    if (v) nonzero++;
  EXPECT_GT(nonzero, 100000) << "the firmware painted the boot screen";
  EXPECT_EQ(m.audio().size() % 2, 0u) << "stereo interleave";
  EXPECT_NEAR(static_cast<double>(m.audio().size()) / 2,
              subcycle::kAudioHz / 50.0, 5.0)
      << "one frame yields ~882 stereo sample pairs";

  // Type: sound 1,239,50,15 <RETURN> — middle C for half a second. The packed
  // codes come from the keyboard matrix table (s=0x74, o=0x42, u=0x52 ...).
  const uint8_t seq[] = {0x74, 0x42, 0x52, 0x56, 0x75, 0x57,  // "sound "
                         0x80, 0x47,                          // "1,"
                         0x81, 0x71, 0x41, 0x47,              // "239,"
                         0x61, 0x40, 0x47,                    // "50,"
                         0x80, 0x61,                          // "15"
                         0x22};                               // RETURN
  for (uint8_t code : seq) tap(m, code);

  // The PSG must now be singing: measure the peak over the next second.
  int peak = 0;
  for (int i = 0; i < 50; ++i) {
    m.run_frame();
    for (int16_t s : m.audio()) peak = std::max<int>(peak, s < 0 ? -s : s);
  }
  EXPECT_GT(peak, 2000) << "SOUND played through the machine's audio path";

  // The screen changed (the typed line echoed).
  int nonzero2 = 0;
  for (uint8_t v : fb)
    if (v) nonzero2++;
  EXPECT_NE(nonzero2, nonzero) << "typing echoed to the display";

  // Reset boots back to a fresh Ready screen (media/ROMs persist as wiring).
  m.reset();
  for (int i = 0; i < 120; ++i) m.run_frame();
  int nonzero3 = 0;
  for (uint8_t v : fb)
    if (v) nonzero3++;
  EXPECT_GT(nonzero3, 100000) << "reboot reached the Ready screen again";
}

#include <cstdio>
#include <filesystem>

#include "slotshandler.h"

TEST(SubcycleMachine, SnapshotRoundTripRestoresTheWholeMachine) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine a;
  ASSERT_TRUE(a.build(rom.data(), rom.size()));
  for (int i = 0; i < 120; ++i) a.run_frame();  // a real, lived-in state

  // Distinctive state: RAM markers (base + expansion), registers, an ink.
  a.poke_mem(0x4321, 0xA5);
  a.ram_write(0x10000 + 0x123, 0x77);  // expansion bank 0
  Z80Regs regs = a.regs();
  regs.ix = 0xBEEF;
  a.set_regs(regs);
  a.io_write(0x7F00, 0x05);         // pen 5
  a.io_write(0x7F00, 0x40 | 0x0B);  // ink 11
  fdc_poke_mechanics(a.fdc(), 1, 42, 7);

  const std::string path =
      (std::filesystem::temp_directory_path() / "koncpc_sna_test.sna").string();
  ASSERT_EQ(snapshot_save_machine(a, path), 0);

  subcycle::Machine b;
  ASSERT_TRUE(b.build(rom.data(), rom.size()));
  FILE* f = fopen(path.c_str(), "rb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(snapshot_load_machine(b, f), 0);
  fclose(f);
  std::filesystem::remove(path);

  EXPECT_EQ(b.regs().ix, 0xBEEF);
  EXPECT_EQ(b.regs().pc, a.regs().pc);
  EXPECT_EQ(b.regs().sp, a.regs().sp);
  EXPECT_EQ(b.peek_mem(0x4321), 0xA5);
  EXPECT_EQ(b.ram_read(0x10000 + 0x123), 0x77);
  GateArrayRegs ga{};
  ga_peek(b.gate_array(), &ga);
  EXPECT_EQ(ga.pen, 5);
  EXPECT_EQ(ga.ink[5], 0x0B);
  FdcRegs fa{}, fb{};
  fdc_peek(a.fdc(), &fa);
  fdc_peek(b.fdc(), &fb);
  EXPECT_EQ(fb.motor, fa.motor);
  EXPECT_EQ(fb.track[0], fa.track[0]);
  EXPECT_EQ(fb.track[1], fa.track[1]);
  // And the restored machine RUNS:
  ASSERT_EQ(probe_add_exec(b.probe(), 0x0038), 0);
  bool hit = false;
  for (int i = 0; i < 20 && !(hit = b.probe_hit(nullptr)); ++i) b.run_frame();
  EXPECT_TRUE(hit) << "the restored machine executes and takes interrupts";
}

namespace {
std::string g_tapped_text;
void collect_txt_output(void* /*ctx*/, uint16_t /*addr*/) {
  // Reading regs needs the machine; the test stores it via the ctx-free
  // global below (single-threaded test).
}
subcycle::Machine* g_tap_machine = nullptr;
void collect_banner(void*, uint16_t) {
  if (g_tap_machine != nullptr)
    g_tapped_text += static_cast<char>(g_tap_machine->regs().af >> 8);
}
}  // namespace

TEST(SubcycleMachine, TxtOutputTapCollectsTheBootBanner) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  g_tap_machine = &m;
  g_tapped_text.clear();
  (void)collect_txt_output;
  // The firmware prints its own banner through TXT_OUTPUT (&BB5A): a tap
  // there collects the boot text with the caller's A intact at the fetch.
  ASSERT_TRUE(m.add_tap(0xBB5A, collect_banner, nullptr));
  for (int i = 0; i < 150; ++i) m.run_frame();
  g_tap_machine = nullptr;
  EXPECT_NE(g_tapped_text.find("BASIC 1.1"), std::string::npos)
      << "collected: " << g_tapped_text.substr(0, 120);
}

// F8 regression: taps must not gate the Fast tier (the GUI console taps are
// always armed — a tap-gated batch could never engage there), and the batch
// driver must fire them itself at the instruction boundary with the caller's
// registers intact. fast_frames_run() proves the batch actually engaged: the
// tap firing via a wake/faithful fallback would silently pass otherwise.
TEST(SubcycleMachine, TxtOutputTapFiresUnderTheFastTier) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  m.set_run_tier(subcycle::Machine::RunTier::Fast);
  g_tap_machine = &m;
  g_tapped_text.clear();
  ASSERT_TRUE(m.add_tap(0xBB5A, collect_banner, nullptr));
  for (int i = 0; i < 150; ++i) m.run_frame();
  g_tap_machine = nullptr;
  EXPECT_NE(g_tapped_text.find("BASIC 1.1"), std::string::npos)
      << "collected: " << g_tapped_text.substr(0, 120);
  EXPECT_GT(m.fast_frames_run(), 0u)
      << "the batch driver never engaged — the tap gate is back?";
}

// Dormant-device exclusion (beads-7qzk): an unplugged peripheral's tick is
// structurally inert (`if (!plugged) return;`), so recompose_active drops it
// from the per-cycle tick list — saving an indirect call per master cycle. The
// exclusion is observation-neutral (DifferentialHarness.ColdBoot guards that);
// this locks the mechanism itself: a stock machine skips its unplugged
// expansions, and plugging one puts exactly it back.
TEST(SubcycleMachine, DormantPeripheralsLeaveTheTickList) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  std::vector<uint8_t> fb(
      static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3, 0);
  m.attach_framebuffer(fb.data(), subcycle::kFbWidth, subcycle::kFbHeight);

  m.run_frame();  // recompose_active runs at frame start
  const int stock = m.active_tick_count();
  EXPECT_LT(stock, 18)
      << "unplugged expansions + the off-model ASIC are skipped";

  m.set_asic(true);  // plug the ASIC (model 3): re-includes exactly one device
  m.run_frame();
  EXPECT_EQ(m.active_tick_count(), stock + 1) << "the ASIC ticks once plugged";

  m.set_asic(false);
  m.run_frame();
  EXPECT_EQ(m.active_tick_count(), stock) << "unplugging drops it back out";

  // A console TAP no longer arms the probe: taps fire from the machine's own
  // fetch-edge check (service_taps), not the per-cycle probe. So a tap-only
  // machine keeps the probe dormant / out of the tick list.
  ASSERT_TRUE(m.add_tap(0x0000, +[](void*, uint16_t) {}, nullptr));
  m.run_frame();
  EXPECT_EQ(m.active_tick_count(), stock)
      << "a tap does not arm the probe (it uses the cheap fetch-edge path)";

  // A real execution breakpoint DOES arm the probe, bringing it back in.
  ASSERT_EQ(probe_add_exec(m.probe(), 0x4000), 0);
  m.run_frame();
  EXPECT_EQ(m.active_tick_count(), stock + 1)
      << "an armed execution breakpoint rejoins the probe to the tick list";
}
