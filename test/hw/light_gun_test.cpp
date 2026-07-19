/* light_gun_test.cpp — the light gun Device + the CRTC LPEN latch: the strobe
 * edge latches MA into R16/R17, the beam-match fires the strobe when the
 * trigger is held over the aim, a released trigger latches nothing, and an
 * unplugged gun is neutral. See docs/hardware/light-gun-device.md §6. */

#include "hw/light_gun.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "hw/board.h"
#include "hw/crtc.h"
#include "subcycle/machine.h"

namespace {

// A board with the CRTC (drives vid.frame_line/dispen and owns the LPEN latch)
// plus the gun (drives pen.strobe). clk.crtc is asserted each tick WITHOUT
// wiping the committed bus, so the cross-device pen.strobe survives one cycle
// into the CRTC's next read.
struct GunRig {
  std::vector<uint8_t> crtc_mem = std::vector<uint8_t>(crtc_state_size());
  std::vector<uint8_t> gun_mem = std::vector<uint8_t>(light_gun_state_size());
  Board board;
  Device crtc;
  Device gun;
};

void make_rig(GunRig& rig) {
  rig.crtc = crtc_init(rig.crtc_mem.data());
  rig.gun = light_gun_init(rig.gun_mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.crtc);
  board_add(&rig.board, rig.gun);
  board_reset(&rig.board);
  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&rig.crtc, i, std_regs[i]);
  crtc_poke_reg(&rig.crtc, 12, 0x30);
  crtc_poke_reg(&rig.crtc, 13, 0x00);
}

// Advance one char, preserving cross-device bus lines (only re-assert
// clk.crtc).
CrtcRegs tick(GunRig& rig) {
  rig.board.bus.clk.crtc = true;
  board_tick(&rig.board);
  CrtcRegs r{};
  crtc_peek(&rig.crtc, &r);
  return r;
}

uint16_t latched_ma(const CrtcRegs& r) {
  return static_cast<uint16_t>(((r.reg[16] & 0x3F) << 8) | r.reg[17]);
}

}  // namespace

// A single strobe cycle latches the CRTC's current MA into R16/R17.
TEST(LightGun, CrtcLatchesMaOnStrobeEdge) {
  std::vector<uint8_t> mem(crtc_state_size());
  Board board;
  Device crtc = crtc_init(mem.data());
  board_init(&board);
  board_add(&board, crtc);
  board_reset(&board);
  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&crtc, i, std_regs[i]);
  crtc_poke_reg(&crtc, 12, 0x30);
  crtc_poke_reg(&crtc, 13, 0x00);

  // Advance a few chars so MA is nonzero; freeze it (clk.crtc low) and strobe.
  for (int i = 0; i < 10; ++i) {
    board.bus = bus_resting();
    board.bus.clk.crtc = true;
    board_tick(&board);
  }
  CrtcRegs before{};
  crtc_peek(&crtc, &before);
  ASSERT_GT(before.ma, 0u);

  board.bus = bus_resting();
  board.bus.pen.strobe = true;  // clk.crtc low: MA frozen at `before.ma`
  board_tick(&board);
  CrtcRegs after{};
  crtc_peek(&crtc, &after);
  EXPECT_EQ(after.reg[16], (before.ma >> 8) & 0x3F);
  EXPECT_EQ(after.reg[17], before.ma & 0xFF);
}

// Held HIGH the latch fires ONCE (edge-triggered): after the first latch, MA
// advancing under a still-high strobe does not re-latch.
TEST(LightGun, CrtcLatchesOnceWhileStrobeHeld) {
  std::vector<uint8_t> mem(crtc_state_size());
  Board board;
  Device crtc = crtc_init(mem.data());
  board_init(&board);
  board_add(&board, crtc);
  board_reset(&board);
  const uint8_t std_regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&crtc, i, std_regs[i]);
  crtc_poke_reg(&crtc, 12, 0x30);
  crtc_poke_reg(&crtc, 13, 0x00);

  for (int i = 0; i < 10; ++i) {
    board.bus = bus_resting();
    board.bus.clk.crtc = true;
    board_tick(&board);
  }
  // Rising edge with MA frozen: latch value A.
  board.bus = bus_resting();
  board.bus.pen.strobe = true;
  board_tick(&board);
  CrtcRegs a{};
  crtc_peek(&crtc, &a);
  const uint16_t latch_a = latched_ma(a);

  // Keep strobe HIGH while MA advances — no new rising edge, no re-latch.
  for (int i = 0; i < 5; ++i) {
    board.bus.clk.crtc = true;    // advance MA
    board.bus.pen.strobe = true;  // still held
    board_tick(&board);
  }
  CrtcRegs b{};
  crtc_peek(&crtc, &b);
  EXPECT_GT(b.ma, a.ma) << "MA advanced under the held strobe";
  EXPECT_EQ(latched_ma(b), latch_a) << "no re-latch without a new edge";
}

// Trigger held over the aim: the beam sweeping under it latches R16/R17, and a
// lower/later aim latches a larger MA (raster order).
TEST(LightGun, BeamMatchLatchesTrackingAim) {
  const uint16_t aims[3] = {60, 120, 180};
  uint16_t latched[3] = {0, 0, 0};
  for (int k = 0; k < 3; ++k) {
    GunRig rig;
    make_rig(rig);
    light_gun_set_type(&rig.gun, 1);
    light_gun_set_aim(&rig.gun, aims[k], 20);  // mid-screen column
    light_gun_set_trigger(&rig.gun, 1);
    CrtcRegs last{};
    for (int i = 0; i < 40000; ++i) last = tick(rig);  // ~2 frames
    latched[k] = latched_ma(last);
    EXPECT_GT(latched[k], 0u) << "aim line " << aims[k] << " never latched";
  }
  EXPECT_LT(latched[0], latched[1]) << "MA must grow with the aim line";
  EXPECT_LT(latched[1], latched[2]) << "MA must grow with the aim line";
}

// Trigger released: nothing is latched (no legacy R17 increment).
TEST(LightGun, ReleasedTriggerLatchesNothing) {
  GunRig rig;
  make_rig(rig);
  light_gun_set_type(&rig.gun, 1);
  light_gun_set_aim(&rig.gun, 120, 20);
  light_gun_set_trigger(&rig.gun, 0);  // NOT pressed
  CrtcRegs last{};
  for (int i = 0; i < 40000; ++i) last = tick(rig);
  EXPECT_EQ(last.reg[16], 0) << "R16 untouched with the trigger up";
  EXPECT_EQ(last.reg[17], 0) << "R17 untouched — no legacy increment";
}

// Unplugged (type 0): the gun never drives the strobe, so R16/R17 stay reset.
TEST(LightGun, UnpluggedNeutral) {
  GunRig rig;
  make_rig(rig);
  light_gun_set_type(&rig.gun, 0);  // unplugged
  light_gun_set_aim(&rig.gun, 120, 20);
  light_gun_set_trigger(&rig.gun, 1);  // pressed, but unplugged
  CrtcRegs last{};
  for (int i = 0; i < 40000; ++i) last = tick(rig);
  EXPECT_EQ(last.reg[16], 0);
  EXPECT_EQ(last.reg[17], 0);
  LightGunRegs g{};
  light_gun_peek(&rig.gun, &g);
  EXPECT_EQ(g.plugged, 0);
}

// Gun + CRTC state survives a save/load round-trip (incl. crtc lpen_prev).
TEST(LightGun, SnapshotRoundTrip) {
  GunRig rig;
  make_rig(rig);
  light_gun_set_type(&rig.gun, 2);
  light_gun_set_aim(&rig.gun, 77, 33);
  light_gun_set_trigger(&rig.gun, 1);
  for (int i = 0; i < 500; ++i) tick(rig);

  std::vector<uint8_t> gblob(rig.gun.state_size(rig.gun.self));
  std::vector<uint8_t> cblob(rig.crtc.state_size(rig.crtc.self));
  rig.gun.save(rig.gun.self, gblob.data());
  rig.crtc.save(rig.crtc.self, cblob.data());

  GunRig rig2;
  make_rig(rig2);
  rig2.gun.load(rig2.gun.self, gblob.data());
  rig2.crtc.load(rig2.crtc.self, cblob.data());

  LightGunRegs a{}, b{};
  light_gun_peek(&rig.gun, &a);
  light_gun_peek(&rig2.gun, &b);
  EXPECT_EQ(a.type, b.type);
  EXPECT_EQ(a.aim_line, b.aim_line);
  EXPECT_EQ(a.aim_col, b.aim_col);
  EXPECT_EQ(a.pressed, b.pressed);

  // Both advance identically after the restore (lpen_prev preserved).
  for (int i = 0; i < 2000; ++i) {
    CrtcRegs ra = tick(rig);
    CrtcRegs rb = tick(rig2);
    ASSERT_EQ(ra.reg[16], rb.reg[16]) << "diverged at " << i;
    ASSERT_EQ(ra.reg[17], rb.reg[17]) << "diverged at " << i;
  }
}

// ---------------------------------------------------------------------------
// Tier oracle: the light-pen latch must agree across Soldered, Wake and Fast
// when a gun is plugged and the trigger is held.  This is the integration test
// that catches the wake_slot / run_frame_fast omission (beads-g4jq).
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> load_rom_file(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> load_system_rom() {
  std::vector<uint8_t> rom = load_rom_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = load_rom_file("../rom/cpc6128.rom");
  return rom;
}

constexpr size_t kFbLen =
    static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3;

// Build a 6128, plug the gun, and run enough frames for the beam to sweep the
// aim point.  Returns the latched light-pen address (R16/R17) and the final
// framebuffer hash so we can assert observation-identicality across tiers.
struct LpenResult {
  uint16_t ma;
  uint32_t fast_frames;
};

LpenResult run_with_tier(const std::vector<uint8_t>& rom,
                         subcycle::Machine::RunTier tier, uint16_t aim_line,
                         uint16_t aim_col) {
  subcycle::Machine machine;
  std::vector<uint8_t> fb(kFbLen, 0);
  if (!machine.build(rom.data(), rom.size())) return {0, 0};
  machine.attach_framebuffer(fb.data(), subcycle::kFbWidth,
                             subcycle::kFbHeight);
  machine.set_light_gun(1, aim_line, aim_col, true);
  machine.set_run_tier(tier);

  // Run a handful of frames.  The beam sweeps the whole screen each frame, so
  // the aim point is guaranteed to be visited.  Fast may need a frame or two to
  // find a clean batch entry point.
  for (int frame = 0; frame < 4; ++frame) machine.run_frame();

  CrtcRegs cr{};
  crtc_peek(machine.crtc(), &cr);
  const uint16_t ma =
      static_cast<uint16_t>(((cr.reg[16] & 0x3F) << 8) | cr.reg[17]);
  return {ma, machine.fast_frames_run()};
}

}  // namespace

TEST(LightGun, TierLatchAgreesSolderedWakeFast) {
  std::vector<uint8_t> rom = load_system_rom();
  if (rom.size() < 0x8000)
    GTEST_SKIP() << "rom/cpc6128.rom not found (run from project root)";

  // Aim near the middle of the visible window; the ±2 char/line tolerance makes
  // the exact pixel irrelevant.
  constexpr uint16_t kAimLine = 120;
  constexpr uint16_t kAimCol = 18;

  const auto soldered = run_with_tier(rom, subcycle::Machine::RunTier::Soldered,
                                      kAimLine, kAimCol);
  const auto wake =
      run_with_tier(rom, subcycle::Machine::RunTier::Wake, kAimLine, kAimCol);
  const auto fast =
      run_with_tier(rom, subcycle::Machine::RunTier::Fast, kAimLine, kAimCol);

  EXPECT_GT(soldered.ma, 0u) << "Soldered tier never latched";
  EXPECT_EQ(wake.ma, soldered.ma) << "Wake-tier LPEN latch diverged";
  EXPECT_EQ(fast.ma, soldered.ma) << "Fast-tier LPEN latch diverged";
  EXPECT_GT(fast.fast_frames, 0u) << "Fast tier never engaged";
}
