/* serial_pair_machine_test.cpp — the RS232 card + HP 7470A plotter pair at
 * MACHINE level (beads-5q4v milestone C, plotter-device.md acid 5).
 *
 * The pair has no wake contract yet, so plugging it must degrade the
 * effective tier to Faithful (the Symbiface precedent in
 * recompose_active) — and the machine must stay byte-identical across
 * REQUESTED tiers while degraded. The plot itself is driven the honest way:
 * a Z80 program on the bus programs the 8253 divisor and feeds HP-GL bytes
 * through the DART data port, polling RR0 TX-buffer-empty between bytes. */

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "hw/plotter_hp7470a.h"
#include "subcycle/machine.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

uint64_t fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ULL;
  return h;
}

constexpr size_t kFbLen =
    static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3;

struct Rig {
  subcycle::Machine m;
  std::vector<uint8_t> fb = std::vector<uint8_t>(kFbLen, 0);
};

void boot(Rig& r, const std::vector<uint8_t>& rom,
          subcycle::Machine::RunTier tier) {
  ASSERT_TRUE(r.m.build(rom.data(), rom.size()));
  r.m.attach_framebuffer(r.fb.data(), subcycle::kFbWidth,
                         subcycle::kFbHeight);
  r.m.set_run_tier(tier);
}

std::vector<uint8_t> load_rom() {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  return rom;
}

// DI; program the 8253 (ctr 0, LSB/MSB, mode 3) to `divisor`; then stream the
// NUL-terminated string at 0x8030 through the DART data port, polling RR0
// TX-buffer-empty (bit 3) before each byte; spin when done.
//
// `divisor == 0` NOPs out the 8253 programming entirely — the card is left in
// its power-up (unprogrammed) state, which is exactly what the CP/M flow does:
// DDHP7470.PRL emits through the BDOS, never touching the 8253, so the RS232 TX
// runs at bit_time's default (div 1) while the plotter samples at its DIP rate.
void inject_plot_program(subcycle::Machine& m, const char* hpgl,
                         uint16_t divisor = 1) {
  uint8_t prog[] = {
      0xF3,              // 8000 DI
      0x01, 0xDF, 0xFB,  // 8001 LD BC,#FBDF   (8253 control)
      0x3E, 0x36,        // 8004 LD A,#36      (ctr0, LSB/MSB, mode 3)
      0xED, 0x79,        // 8006 OUT (C),A
      0x01, 0xDC, 0xFB,  // 8008 LD BC,#FBDC   (ctr0 data)
      0x3E, 0x01,        // 800B LD A,#lsb     (divisor low  — [0x0C])
      0xED, 0x79,        // 800D OUT (C),A
      0x3E, 0x00,        // 800F LD A,#msb     (divisor high — [0x10])
      0xED, 0x79,        // 8011 OUT (C),A
      0x21, 0x30, 0x80,  // 8013 LD HL,#8030
      0x7E,              // 8016 loop: LD A,(HL)
      0xB7,              // 8017 OR A
      0x28, 0x14,        // 8018 JR Z,done (#802E)
      0x01, 0xDE, 0xFA,  // 801A wait: LD BC,#FADE
      0xED, 0x78,        // 801D IN A,(C)
      0xE6, 0x08,        // 801F AND #08
      0x28, 0xF7,        // 8021 JR Z,wait
      0x7E,              // 8023 LD A,(HL)
      0x01, 0xDC, 0xFA,  // 8024 LD BC,#FADC
      0xED, 0x79,        // 8027 OUT (C),A
      0x23,              // 8029 INC HL
      0x18, 0xEA,        // 802A JR loop (#8016)
      0x00, 0x00,        // 802C pad
      0x18, 0xFE,        // 802E done: JR done
  };
  if (divisor == 0) {
    for (int i = 1; i <= 0x12; ++i) prog[i] = 0x00;  // NOP the 8253 setup
  } else {
    prog[0x0C] = static_cast<uint8_t>(divisor & 0xFF);
    prog[0x10] = static_cast<uint8_t>(divisor >> 8);
  }
  uint16_t addr = 0x8000;
  for (uint8_t b : prog) m.poke_mem(addr++, b);
  addr = 0x8030;
  for (const char* p = hpgl; *p; ++p)
    m.poke_mem(addr++, static_cast<uint8_t>(*p));
  m.poke_mem(addr, 0);
  Z80Regs regs = m.regs();
  regs.pc = 0x8000;
  m.set_regs(regs);
}

size_t plotter_segment_count(const subcycle::Machine& m) {
  const PlotSeg* segs = nullptr;
  return plotter_hp7470a_segments(m.plotter(), &segs);
}

}  // namespace

TEST(SerialPairMachine, PluggedPairStaysFastValidAndIdleRunsFast) {
  std::vector<uint8_t> rom = load_rom();
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";
  Rig r;
  boot(r, rom, subcycle::Machine::RunTier::Fast);
  r.m.run_frame();
  ASSERT_EQ(r.m.effective_run_tier(), subcycle::Machine::RunTier::Fast)
      << "sanity: unplugged machine runs the requested tier";

  r.m.set_serial_plotter(true, 9600);
  r.m.run_frame();  // quiet at the frame boundary
  // The pair is fast-valid: it no longer degrades the tier at all. An idle
  // frame runs full Fast (the serial is a no-op); a frame that actually starts a
  // transmission bails to the per-cycle remainder internally (fs_io_write_event)
  // — that path is covered by the plot-delivery tests below.
  EXPECT_EQ(r.m.effective_run_tier(), subcycle::Machine::RunTier::Fast)
      << "serial pair no longer caps the tier; a quiet frame runs Fast";

  r.m.set_serial_plotter(false, 9600);
  r.m.run_frame();
  EXPECT_EQ(r.m.effective_run_tier(), subcycle::Machine::RunTier::Fast)
      << "unplugging is a no-op for the tier now";
}

TEST(SerialPairMachine, Z80PlotsOverTheWireIdenticallyAcrossRequestedTiers) {
  std::vector<uint8_t> rom = load_rom();
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";
  const char* kHpgl = "IN;SP1;PD;PA1000,1000;";

  Rig a, b;
  boot(a, rom, subcycle::Machine::RunTier::Fast);
  boot(b, rom, subcycle::Machine::RunTier::Wake);
  for (Rig* r : {&a, &b}) {
    // divisor from baud: 2e6/(125000*16) = 1 → bit time 128 cycles
    r->m.set_serial_plotter(true, 125000);
    for (int i = 0; i < 3; ++i) r->m.run_frame();  // settle the boot
    inject_plot_program(r->m, kHpgl);
  }
  for (int f = 0; f < 6; ++f) {
    a.m.run_frame();
    b.m.run_frame();
    ASSERT_EQ(fnv1a(a.fb.data(), kFbLen), fnv1a(b.fb.data(), kFbLen))
        << "framebuffer diverged across requested tiers at frame " << f;
  }
  // Both degraded runs produced the identical page: one pen-down line.
  ASSERT_GE(plotter_segment_count(a.m), 1u) << "the plot never arrived";
  EXPECT_EQ(plotter_segment_count(a.m), plotter_segment_count(b.m));

  const PlotSeg* segs = nullptr;
  plotter_hp7470a_segments(a.m.plotter(), &segs);
  EXPECT_EQ(segs[0].type, 0) << "a Line";
  EXPECT_EQ(segs[0].pen, 1);
  EXPECT_FLOAT_EQ(segs[0].x1, 0.0f);
  EXPECT_FLOAT_EQ(segs[0].y1, 0.0f);
  EXPECT_FLOAT_EQ(segs[0].x2, 1000.0f);
  EXPECT_FLOAT_EQ(segs[0].y2, 1000.0f);
}

// beads-q9kx — reproduce the CP/M-flow failure at MACHINE level, deterministically.
//
// The GUI plugs the pair at 9600 baud, so set_serial_plotter sets the plotter's
// DIP divisor to 2e6/(9600*16) = 13. But the DDHP7470.PRL driver emits every
// HP-GL byte through the BDOS — it never programs the SI card's 8253 — so the
// RS232 TX runs at bit_time's unprogrammed default (div 1 → 128 master cycles
// per bit) while the plotter samples at div 13 (1664 cycles/bit). The far end
// mis-frames every byte: ZERO segments. This is the exact wire-rate mismatch
// that yields "0 bytes reach plotter" in the live emulator, with no CP/M banking
// in the way.
TEST(SerialPairMachine, UnprogrammedBaudAt9600MisframesEverythingNoPlot) {
  std::vector<uint8_t> rom = load_rom();
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";
  const char* kHpgl = "IN;SP1;PD;PA1000,1000;";

  Rig r;
  boot(r, rom, subcycle::Machine::RunTier::Fast);
  r.m.set_serial_plotter(true, 9600);              // plotter DIP divisor = 13
  for (int i = 0; i < 3; ++i) r.m.run_frame();     // settle the boot
  inject_plot_program(r.m, kHpgl, /*divisor=*/0);  // 8253 left UNPROGRAMMED → div 1
  for (int f = 0; f < 8; ++f) r.m.run_frame();

  EXPECT_EQ(plotter_segment_count(r.m), 0u)
      << "wire-rate mismatch (RS232 div 1 vs plotter div 13) must lose the plot";
}

// The fix criterion: when the TX side IS clocked to the plotter's rate (the SI
// card's 8253 programmed to the same divisor the DIP is set to), the identical
// HP-GL byte-stream lands as a clean pen-down line. This is what the CP/M path
// must arrange — either the activator (SERIAL.COM) programs the 8253 to the
// configured baud, or set_serial_plotter clocks both ends alike.
TEST(SerialPairMachine, MatchedBaudAt9600DeliversThePlot) {
  std::vector<uint8_t> rom = load_rom();
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";
  const char* kHpgl = "IN;SP1;PD;PA1000,1000;";

  Rig r;
  boot(r, rom, subcycle::Machine::RunTier::Fast);
  r.m.set_serial_plotter(true, 9600);               // plotter DIP divisor = 13
  for (int i = 0; i < 3; ++i) r.m.run_frame();       // settle the boot
  inject_plot_program(r.m, kHpgl, /*divisor=*/13);   // 8253 → div 13, matches plotter
  for (int f = 0; f < 8; ++f) r.m.run_frame();

  ASSERT_GE(plotter_segment_count(r.m), 1u)
      << "matched wire rates must deliver the plot";
  const PlotSeg* segs = nullptr;
  plotter_hp7470a_segments(r.m.plotter(), &segs);
  EXPECT_EQ(segs[0].type, 0);
  EXPECT_EQ(segs[0].pen, 1);
  EXPECT_FLOAT_EQ(segs[0].x2, 1000.0f);
  EXPECT_FLOAT_EQ(segs[0].y2, 1000.0f);
}
