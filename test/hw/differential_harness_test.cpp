/* differential_harness_test.cpp — Gate B3 (beads-t907), first consumer of the
 * differential-harness engine: a cold-boot DETERMINISM gate.
 *
 * Two independently-built Machines, same ROM, same (empty) keyboard input, must
 * agree frame-for-frame on BOTH tiers (displayed framebuffer AND all-device
 * logical state). This proves the faithful simulator is deterministic — no
 * uninitialised-memory bleed, no host-clock/RNG leak into observations — which
 * is the substrate the future mode-vs-mode comparison (B4-B7) stands on. When a
 * fast mode lands, the second machine is built in that mode and this same test
 * becomes reference-vs-fast equivalence.
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "diff_harness.h"
#include "subcycle/machine.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> load_system_rom() {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  return rom;
}

constexpr size_t kFbLen =
    static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3;

// Cold-boot a 6128 with its framebuffer attached. `fbuf` is caller-owned and
// must outlive the machine (one extraction seam — the harness never owns it).
bool cold_boot(subcycle::Machine& machine, const std::vector<uint8_t>& rom,
               std::vector<uint8_t>& fbuf) {
  if (!machine.build(rom.data(), rom.size())) return false;
  fbuf.assign(kFbLen, 0);
  machine.attach_framebuffer(fbuf.data(), subcycle::kFbWidth,
                             subcycle::kFbHeight);
  return true;
}

// Space-joined names of the devices whose per-device state hash differs between
// two machines. Names the culprit on a deep-tier divergence instead of leaving
// an opaque folded digest (kept out of the test body to hold its cognitive
// complexity down).
std::string diverged_devices(const subcycle::Machine& lhs,
                             const subcycle::Machine& rhs) {
  std::vector<uint64_t> per_lhs = diffharness::per_device_state_hashes(lhs);
  std::vector<uint64_t> per_rhs = diffharness::per_device_state_hashes(rhs);
  std::vector<diffharness::NamedDevice> names = diffharness::board_devices(lhs);
  std::string out;
  for (size_t i = 0; i < per_lhs.size(); ++i) {
    if (per_lhs[i] != per_rhs[i]) out += std::string(" ") + names[i].name;
  }
  return out;
}

// Feed one frame's worth of "no keys pressed" (every CPC matrix row released),
// exactly as the GUI does each frame. Kept out of the test body to remove a
// nesting level from its cognitive complexity.
void feed_no_keys(subcycle::Machine& machine) {
  for (int row = 0; row < 10; ++row) machine.set_key_row(row, 0xFF);
}

// Assert both tiers match for one frame; return whether they did so the caller
// can stop at the first divergence. Holding the two assertions here keeps the
// test body's cognitive complexity under threshold.
bool frames_match(int frame, const diffharness::FrameHashes& lhs_h,
                  const diffharness::FrameHashes& rhs_h,
                  const subcycle::Machine& lhs, const subcycle::Machine& rhs) {
  EXPECT_EQ(lhs_h.fb, rhs_h.fb) << "framebuffer diverged at frame " << frame;
  EXPECT_EQ(lhs_h.state, rhs_h.state)
      << "device state diverged at frame " << frame << " —"
      << diverged_devices(lhs, rhs);
  return lhs_h.fb == rhs_h.fb && lhs_h.state == rhs_h.state;
}

}  // namespace

// Two machines from the same seed must produce byte-identical observations AND
// logical state every frame. A divergence means the simulator carries hidden
// non-determinism — the exact fault the fast modes must never introduce.
TEST(DifferentialHarness, ColdBootIsDeterministic) {
  std::vector<uint8_t> rom = load_system_rom();
  if (rom.size() < 0x8000)
    GTEST_SKIP() << "rom/cpc6128.rom not found (run from project root)";

  subcycle::Machine lhs, rhs;
  std::vector<uint8_t> fb_lhs, fb_rhs;
  ASSERT_TRUE(cold_boot(lhs, rom, fb_lhs));
  ASSERT_TRUE(cold_boot(rhs, rom, fb_rhs));

  constexpr int kFrames = 120;  // ~2.4 s CPC time — well past the boot banner
  uint64_t last_fb = 0;
  int changed_frames = 0;
  for (int frame = 0; frame < kFrames; ++frame) {
    feed_no_keys(lhs);
    feed_no_keys(rhs);
    diffharness::FrameHashes hash_lhs =
        diffharness::step_and_hash(lhs, fb_lhs.data(), kFbLen);
    diffharness::FrameHashes hash_rhs =
        diffharness::step_and_hash(rhs, fb_rhs.data(), kFbLen);

    if (!frames_match(frame, hash_lhs, hash_rhs, lhs, rhs)) break;

    changed_frames += (hash_lhs.fb != last_fb) ? 1 : 0;
    last_fb = hash_lhs.fb;
  }

  // Liveness guard: a machine stuck black/frozen is trivially "deterministic".
  // Require the observation to actually evolve during boot so this gate cannot
  // pass on a dead machine (see the Plus black-framebuffer investigation for
  // why a stuck-but-consistent frame is a real failure mode, not a pass).
  EXPECT_GT(changed_frames, 1)
      << "framebuffer never changed across " << kFrames
      << " frames — machine may be frozen, not deterministic";
}

// Faithful (board_tick) and Fast (tick_soldered) are two dispatch shapes over
// the SAME devices in the SAME order across the SAME two-phase bus, so they
// must be observation-identical. This is the mode-vs-mode comparison the
// harness was built for (Gate B3) — activatable now that B5 gives the second
// tier. A divergence means tick_soldered and board_tick disagree: a real bug,
// not a tolerance.
TEST(DifferentialHarness, FaithfulMatchesSolderedTier) {
  std::vector<uint8_t> rom = load_system_rom();
  if (rom.size() < 0x8000)
    GTEST_SKIP() << "rom/cpc6128.rom not found (run from project root)";

  subcycle::Machine faithful, fast;
  std::vector<uint8_t> fb_faithful, fb_fast;
  ASSERT_TRUE(cold_boot(faithful, rom, fb_faithful));
  ASSERT_TRUE(cold_boot(fast, rom, fb_fast));
  // Wake is the DEFAULT tier now — both sides of this comparison must opt out
  // so the per-cycle dispatchers under test are the ones actually running.
  faithful.set_wake(false);
  fast.set_wake(false);
  faithful.set_run_tier(subcycle::Machine::RunTier::Faithful);
  fast.set_run_tier(subcycle::Machine::RunTier::Soldered);
  // Both tiers must be the ones we asked for (stock 6128 = canonical
  // composition, so Fast does not degrade).
  ASSERT_TRUE(faithful.effective_run_tier() ==
              subcycle::Machine::RunTier::Faithful);
  ASSERT_TRUE(fast.effective_run_tier() ==
              subcycle::Machine::RunTier::Soldered);

  constexpr int kFrames = 120;
  for (int frame = 0; frame < kFrames; ++frame) {
    feed_no_keys(faithful);
    feed_no_keys(fast);
    diffharness::FrameHashes hash_faithful =
        diffharness::step_and_hash(faithful, fb_faithful.data(), kFbLen);
    diffharness::FrameHashes hash_fast =
        diffharness::step_and_hash(fast, fb_fast.data(), kFbLen);
    if (!frames_match(frame, hash_faithful, hash_fast, faithful, fast)) break;
  }
}

// Faithful (board_tick, every device every cycle) and Wake (tick_wake, each
// device only on its clock-wake contract cycles with held bus lines) must be
// observation-identical: the wake contracts claim a sleeping chip could neither
// change state nor drive differently, and this is where that claim is proven —
// framebuffer AND all-device logical state, every frame (Gate B6, beads-708f).
TEST(DifferentialHarness, FaithfulMatchesWakeTier) {
  std::vector<uint8_t> rom = load_system_rom();
  if (rom.size() < 0x8000)
    GTEST_SKIP() << "rom/cpc6128.rom not found (run from project root)";

  subcycle::Machine faithful, wake;
  std::vector<uint8_t> fb_faithful, fb_wake;
  ASSERT_TRUE(cold_boot(faithful, rom, fb_faithful));
  ASSERT_TRUE(cold_boot(wake, rom, fb_wake));
  faithful.set_wake(false);  // wake is the default now: pin the reference side
  wake.set_wake(true);       // all per-device wake predicates armed
  ASSERT_TRUE(wake.wake_active())
      << "stock 6128 is the canonical composition; wake must not degrade";

  constexpr int kFrames = 120;
  for (int frame = 0; frame < kFrames; ++frame) {
    feed_no_keys(faithful);
    feed_no_keys(wake);
    diffharness::FrameHashes hash_faithful =
        diffharness::step_and_hash(faithful, fb_faithful.data(), kFbLen);
    diffharness::FrameHashes hash_wake =
        diffharness::step_and_hash(wake, fb_wake.data(), kFbLen);
    if (!frames_match(frame, hash_faithful, hash_wake, faithful, wake)) break;
  }
}

// The RS232 + HP7470 plotter pair now carries a wake contract (idle-skip, awake
// on $FAxx/$FBxx iorq or a byte in flight). With the pair PLUGGED, Wake must
// (a) stay active — NOT degrade to Faithful the way an uncontracted peripheral
// does — and (b) remain observation-identical to Faithful every frame, incl.
// the two extra devices' logical state. This is the guard for beads-ymdj: the
// contract is what stops the plotter dragging the whole machine to Faithful
// (and thence onto E-cores at ~18 FPS). Cold-boot exercises the QUIET path (the
// pair idle end-to-end); the active bit-shifting path is covered by the
// machine-level serial-pair tests.
TEST(DifferentialHarness, FaithfulMatchesWakeTier_SerialPlotter) {
  std::vector<uint8_t> rom = load_system_rom();
  if (rom.size() < 0x8000)
    GTEST_SKIP() << "rom/cpc6128.rom not found (run from project root)";

  subcycle::Machine faithful, wake;
  std::vector<uint8_t> fb_faithful, fb_wake;
  ASSERT_TRUE(cold_boot(faithful, rom, fb_faithful));
  ASSERT_TRUE(cold_boot(wake, rom, fb_wake));
  faithful.set_serial_plotter(true, 9600);
  wake.set_serial_plotter(true, 9600);
  faithful.set_wake(false);
  wake.set_wake(true);
  ASSERT_TRUE(wake.wake_active())
      << "serial+plotter carry a wake contract now; wake must not degrade";

  constexpr int kFrames = 120;
  for (int frame = 0; frame < kFrames; ++frame) {
    feed_no_keys(faithful);
    feed_no_keys(wake);
    diffharness::FrameHashes hash_faithful =
        diffharness::step_and_hash(faithful, fb_faithful.data(), kFbLen);
    diffharness::FrameHashes hash_wake =
        diffharness::step_and_hash(wake, fb_wake.data(), kFbLen);
    if (!frames_match(frame, hash_faithful, hash_wake, faithful, wake)) break;
  }
}
