/* record_replay_test.cpp — Gate B2 (beads-icz0): deterministic input
 * record/replay. Proves the trace format round-trips, that events apply at
 * EXACTLY their tagged master cycle, that the same trace replayed on two fresh
 * machines yields identical framebuffer + all-device state hashes (the CI
 * corpus guarantee), and that the host-clock/filesystem-coupled devices are
 * excluded from the deterministic device set. */

#include "subcycle/record_replay.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "diff_harness.h"
#include "hw/amx.h"
#include "hw/m4.h"
#include "hw/smartwatch.h"
#include "hw/symbiface.h"
#include "subcycle/machine.h"

namespace {

using recordreplay::EventKind;
using recordreplay::InputEvent;
using recordreplay::Player;
using recordreplay::Recorder;

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

// The real 6128 firmware, if present (run from project root or one level down).
std::vector<uint8_t> load_system_rom() {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  return rom;
}

// A blank 32K ROM: enough for build() to construct every Device (the CPU just
// runs NOPs). Lets the device-level tests avoid needing the real firmware.
std::vector<uint8_t> blank_rom() { return std::vector<uint8_t>(0x8000, 0); }

constexpr size_t kFbLen =
    static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3;

// A representative trace exercising every input kind.
Recorder make_trace() {
  Recorder rec;
  rec.key_row(50000, 9, 0xEF);        // joystick/keyboard matrix row 9
  rec.key(300000, 0x45, true);        // packed key code, press
  rec.amx_mouse(300000, 7, -3, 0x1);  // same cycle as the key (order kept)
  rec.sym_mouse(700000, 0x2A);        // Symbiface mouse packet
  return rec;
}

}  // namespace

TEST(RecordReplayTrace, SerializeDeserializeRoundTrip) {
  const Recorder rec = make_trace();
  const std::vector<uint8_t> blob = rec.serialize();

  std::vector<InputEvent> back;
  ASSERT_TRUE(recordreplay::deserialize(blob.data(), blob.size(), &back));
  ASSERT_EQ(back.size(), rec.events().size());
  for (size_t i = 0; i < back.size(); ++i) EXPECT_EQ(back[i], rec.events()[i]);

  // Serialisation is canonical: re-encoding the parsed events reproduces bytes.
  Recorder round;
  for (const InputEvent& ev : back) round.add(ev);
  EXPECT_EQ(round.serialize(), blob);
}

TEST(RecordReplayTrace, EmptyTraceRoundTrips) {
  const Recorder rec;  // no events
  const std::vector<uint8_t> blob = rec.serialize();
  EXPECT_EQ(blob.size(), recordreplay::kHeaderBytes);
  std::vector<InputEvent> back{InputEvent{}};  // pre-fill to prove it clears
  ASSERT_TRUE(recordreplay::deserialize(blob.data(), blob.size(), &back));
  EXPECT_TRUE(back.empty());
}

TEST(RecordReplayTrace, RejectsMalformed) {
  std::vector<InputEvent> out;
  // Truncated header.
  const uint8_t tiny[4] = {'K', 'R', 'P', 'L'};
  EXPECT_FALSE(recordreplay::deserialize(tiny, sizeof(tiny), &out));

  // Good length, wrong magic.
  std::vector<uint8_t> bad = make_trace().serialize();
  bad[0] = 'X';
  EXPECT_FALSE(recordreplay::deserialize(bad.data(), bad.size(), &out));

  // Right magic, unsupported version.
  bad = make_trace().serialize();
  bad[4] = 0xFF;
  EXPECT_FALSE(recordreplay::deserialize(bad.data(), bad.size(), &out));

  // Count claims more records than the blob carries.
  bad = make_trace().serialize();
  bad[8] = 0xFF;
  EXPECT_FALSE(recordreplay::deserialize(bad.data(), bad.size(), &out));

  // Out-of-range event kind in the first record.
  bad = make_trace().serialize();
  bad[recordreplay::kHeaderBytes + 8] = 0x7F;
  EXPECT_FALSE(recordreplay::deserialize(bad.data(), bad.size(), &out));

  EXPECT_TRUE(recordreplay::deserialize(nullptr, 0, &out) == false);
}

// Temp-file round-trip; the fixture removes the file in TearDown so no handle
// outlives the test (Windows locks open files against remove).
class RecordReplayFile : public ::testing::Test {
 protected:
  void TearDown() override { std::remove(path_.c_str()); }
  std::string path_ = (::testing::TempDir() + "koncepcja_rr_test.krpl");
};

TEST_F(RecordReplayFile, SaveLoadRoundTrip) {
  const Recorder rec = make_trace();
  ASSERT_TRUE(recordreplay::save_trace(path_.c_str(), rec));
  std::vector<InputEvent> back;
  ASSERT_TRUE(recordreplay::load_trace(path_.c_str(), &back));
  ASSERT_EQ(back.size(), rec.events().size());
  for (size_t i = 0; i < back.size(); ++i) EXPECT_EQ(back[i], rec.events()[i]);
}

TEST(RecordReplayPlayer, AppliesAtExactCycleInOrder) {
  Player player({
      InputEvent{100, EventKind::kSymMouse, 0, 0x11, 0, 0},
      InputEvent{200, EventKind::kSymMouse, 0, 0x22, 0, 0},
  });
  subcycle::Machine machine;
  const std::vector<uint8_t> rom = blank_rom();
  ASSERT_TRUE(machine.build(rom.data(), rom.size()));

  // Before either tag: nothing applied.
  player.apply_pending(machine, 99);
  EXPECT_EQ(player.applied(), 0u);
  // Reaching the first tag applies exactly one.
  player.apply_pending(machine, 150);
  EXPECT_EQ(player.applied(), 1u);
  // Reaching the second applies the rest.
  player.apply_pending(machine, 1000);
  EXPECT_EQ(player.applied(), 2u);
  EXPECT_TRUE(player.done());
}

TEST(RecordReplayPlayer, EventReachesTheDevice) {
  subcycle::Machine machine;
  const std::vector<uint8_t> rom = blank_rom();
  ASSERT_TRUE(machine.build(rom.data(), rom.size()));
  machine.set_amx_mouse(true);  // plug the AMX so the mouse packet lands

  Player player({InputEvent{10, EventKind::kAmxMouse, 0, /*buttons=*/0x5,
                            /*dx=*/9, /*dy=*/-4}});
  player.attach(machine);
  player.apply_pending(machine, 10);
  ASSERT_TRUE(player.done());

  AmxRegs regs;
  amx_peek(machine.amx(), &regs);
  // The firmware never ran (blank ROM), so the fed mickeys are still pending.
  EXPECT_EQ(regs.buttons, 0x5);
  EXPECT_NE(regs.mickeys_x, 0);
  EXPECT_NE(regs.mickeys_y, 0);
}

TEST(RecordReplayCorpus, HostClockDevicesExcluded) {
  // The documented exclusion list is exactly the three host-coupled devices.
  ASSERT_EQ(recordreplay::kHostCoupledCount, 3u);
  EXPECT_STREQ(recordreplay::kHostCoupledDevices[0], "smartwatch");
  EXPECT_STREQ(recordreplay::kHostCoupledDevices[1], "symbiface");
  EXPECT_STREQ(recordreplay::kHostCoupledDevices[2], "m4");

  subcycle::Machine machine;
  const std::vector<uint8_t> rom = blank_rom();
  ASSERT_TRUE(machine.build(rom.data(), rom.size()));
  // Even if a caller had plugged them, the deterministic set unplugs them.
  machine.set_smartwatch(true);
  machine.set_symbiface(true);
  machine.set_m4(true);
  recordreplay::apply_deterministic_device_set(machine);

  SmartwatchRegs sw;
  smartwatch_peek(machine.smartwatch(), &sw);
  EXPECT_EQ(sw.plugged, 0) << "smartwatch must be excluded (reads host time)";
  Sf2Regs sf;
  sf2_peek(machine.symbiface(), &sf);
  EXPECT_EQ(sf.plugged, 0) << "symbiface must be excluded (RTC + IDE host FS)";
  M4Regs m4;
  m4_peek(machine.m4(), &m4);
  EXPECT_EQ(m4.plugged, 0) << "m4 must be excluded (host FS + wall clock)";
}

// Isolate "a freshly built machine's state hash is deterministic" from all
// replay logic: build two machines from the SAME blank ROM, hash immediately
// (no frames, no input), and require byte-for-byte agreement. This is the
// tightest tripwire for the uninitialized-padding / uninitialized-field class
// of bug — a device save() that copies indeterminate struct padding makes these
// hashes disagree (and vary run-to-run) even before any emulation runs. It
// needs no firmware ROM, so it runs everywhere the corpus test would SKIP.
TEST(RecordReplayCorpus, FreshMachineStateIsDeterministic) {
  const std::vector<uint8_t> rom = blank_rom();

  subcycle::Machine a;
  ASSERT_TRUE(a.build(rom.data(), rom.size()));
  subcycle::Machine b;
  ASSERT_TRUE(b.build(rom.data(), rom.size()));

  const std::vector<uint64_t> ha = diffharness::per_device_state_hashes(a);
  const std::vector<uint64_t> hb = diffharness::per_device_state_hashes(b);
  const std::vector<diffharness::NamedDevice> names =
      diffharness::board_devices(a);
  ASSERT_EQ(ha.size(), hb.size());
  for (size_t i = 0; i < ha.size(); ++i)
    EXPECT_EQ(ha[i], hb[i]) << "device '" << names[i].name
                            << "' has a non-deterministic construction hash "
                               "(likely uninitialized struct padding or field)";
  EXPECT_EQ(diffharness::machine_state_hash(a),
            diffharness::machine_state_hash(b));
}

TEST(RecordReplayCorpus, ReplayIsDeterministic) {
  const std::vector<uint8_t> rom = load_system_rom();
  if (rom.size() < 0x8000)
    GTEST_SKIP() << "rom/cpc6128.rom not found (run from project root)";

  const Recorder rec = make_trace();
  constexpr int kFrames = 6;  // ~1.9M master cycles: covers every tag above

  // Two independently-built machines, same ROM, same trace, exact-cycle replay.
  subcycle::Machine a;
  Player pa(rec.events());
  std::vector<uint8_t> fba(kFbLen, 0);
  ASSERT_TRUE(recordreplay::run_corpus(a, rom.data(), rom.size(), fba.data(),
                                       subcycle::kFbWidth, subcycle::kFbHeight,
                                       pa, kFrames));

  subcycle::Machine b;
  Player pb(rec.events());
  std::vector<uint8_t> fbb(kFbLen, 0);
  ASSERT_TRUE(recordreplay::run_corpus(b, rom.data(), rom.size(), fbb.data(),
                                       subcycle::kFbWidth, subcycle::kFbHeight,
                                       pb, kFrames));

  // Every tagged event fired at its cycle in both runs.
  EXPECT_EQ(pa.applied(), rec.events().size());
  EXPECT_EQ(pb.applied(), rec.events().size());

  // Identical observation AND identical logical state (both harness tiers).
  EXPECT_EQ(diffharness::fb_hash(fba.data(), kFbLen),
            diffharness::fb_hash(fbb.data(), kFbLen));
  // Name the culprit device on failure: a bare digest mismatch is opaque, and
  // the classic cause is one device's save() blob carrying non-deterministic
  // bytes (uninitialized struct PADDING copied by a raw memcpy, or an
  // uninitialized live field). Compare per-device so a regression points
  // straight at the device instead of leaving a whole-machine hash to bisect.
  {
    const std::vector<uint64_t> ha = diffharness::per_device_state_hashes(a);
    const std::vector<uint64_t> hb = diffharness::per_device_state_hashes(b);
    const std::vector<diffharness::NamedDevice> names =
        diffharness::board_devices(a);
    for (size_t i = 0; i < ha.size(); ++i)
      EXPECT_EQ(ha[i], hb[i]) << "device '" << names[i].name
                              << "' state hash is non-deterministic";
  }
  EXPECT_EQ(diffharness::machine_state_hash(a),
            diffharness::machine_state_hash(b));
}

// Build + deterministic-device-set + attach fb + plug AMX + replay, run frames.
// (run_corpus can't plug the AMX itself — it builds the machine internally — so
// the effect test drives the same steps by hand.)
void run_with_amx(subcycle::Machine& machine, const std::vector<uint8_t>& rom,
                  std::vector<uint8_t>& fb, Player& player, int frames) {
  ASSERT_TRUE(machine.build(rom.data(), rom.size()));
  recordreplay::apply_deterministic_device_set(machine);
  machine.set_amx_mouse(true);
  machine.attach_framebuffer(fb.data(), subcycle::kFbWidth,
                             subcycle::kFbHeight);
  player.attach(machine);
  for (int f = 0; f < frames; ++f) machine.run_frame();
}

TEST(RecordReplayCorpus, TraceChangesObservations) {
  const std::vector<uint8_t> rom = load_system_rom();
  if (rom.size() < 0x8000)
    GTEST_SKIP() << "rom/cpc6128.rom not found (run from project root)";
  constexpr int kFrames = 8;

  // Same machine both ways (AMX plugged in each — identical device set); only a
  // replayed mouse feed differs. A large mickey deflection outlives the boot's
  // once-per-frame row-9 scan, so its residual pending mickeys make the fed run
  // diverge in HASHED state — proving replayed input reaches the corpus, not
  // just that identical traces agree.
  Recorder moved;
  moved.amx_mouse(50000, 120, 120, 0x0);

  subcycle::Machine withInput;
  Player p_in(moved.events());
  std::vector<uint8_t> fb_in(kFbLen, 0);
  run_with_amx(withInput, rom, fb_in, p_in, kFrames);

  subcycle::Machine noInput;
  Player p_empty(std::vector<InputEvent>{});
  std::vector<uint8_t> fb_empty(kFbLen, 0);
  run_with_amx(noInput, rom, fb_empty, p_empty, kFrames);

  EXPECT_EQ(p_in.applied(), moved.events().size());
  EXPECT_NE(diffharness::machine_state_hash(withInput),
            diffharness::machine_state_hash(noInput))
      << "replayed mouse input did not affect machine state";
}
