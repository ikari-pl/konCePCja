/* fast_tier_machine_test.cpp — RunTier::Fast at MACHINE level (F6,
 * beads-dv6r): a full 6128 boots the real firmware and types a SOUND command
 * under the Fast tier, in lockstep with a Wake-tier twin.
 *
 * Equality contract (plan §5, with the F6-documented frame-cut caveat): the
 * two tiers cut frames at different micro-points — per-cycle stops the master
 * video completes a frame on (usually mid-instruction), Fast stops at the
 * first instruction boundary after the frame's VSYNC rise — so per-frame
 * STATE hashes are not comparable. What is exact:
 *   - the FRAMEBUFFER at every frame boundary (the CPU-ahead margin's cells
 *     sit inside VSYNC and paint nothing), and
 *   - the CONCATENATED audio stream (sample values are produced by the
 *     machine-persistent decimation window; only the frame-bucket assignment
 *     of a boundary sample can shift by one).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "hw/psg.h"
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

struct Twin {
  subcycle::Machine m;
  std::vector<uint8_t> fb = std::vector<uint8_t>(kFbLen, 0);
  std::vector<int16_t> audio;  // concatenated across every frame

  void frame() {
    m.run_frame();
    audio.insert(audio.end(), m.audio().begin(), m.audio().end());
  }
};

void boot(Twin& t, const std::vector<uint8_t>& rom,
          subcycle::Machine::RunTier tier) {
  ASSERT_TRUE(t.m.build(rom.data(), rom.size()));
  t.m.attach_framebuffer(t.fb.data(), subcycle::kFbWidth, subcycle::kFbHeight);
  t.m.set_run_tier(tier);
}

// One key tap on BOTH twins, comparing the framebuffer after every frame.
void tap_both(Twin& fast, Twin& wake, uint8_t code, int* frame_no) {
  for (int phase = 0; phase < 2; ++phase) {
    fast.m.key(code, phase == 0);
    wake.m.key(code, phase == 0);
    for (int i = 0; i < 4; ++i) {
      fast.frame();
      wake.frame();
      ++*frame_no;
      ASSERT_EQ(fnv1a(fast.fb.data(), kFbLen), fnv1a(wake.fb.data(), kFbLen))
          << "framebuffer diverged at frame " << *frame_no << " (key "
          << int(code) << ")";
    }
  }
}

}  // namespace

// The AY register file must track write-for-write — the first place an AY
// event-relay bug shows, and a far sharper needle than the audio stream.
TEST(FastTierMachine, PsgRegisterFileLockstep) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  Twin fast, wake;
  boot(fast, rom, subcycle::Machine::RunTier::Fast);
  boot(wake, rom, subcycle::Machine::RunTier::Wake);
  for (int f = 0; f < 20; ++f) {
    fast.frame();
    wake.frame();
    PsgRegs pf{}, pw{};
    psg_peek(fast.m.psg(), &pf);
    psg_peek(wake.m.psg(), &pw);
    for (int i = 0; i < 16; ++i)
      ASSERT_EQ(int(pf.reg[i]), int(pw.reg[i]))
          << "AY reg " << i << " diverged at frame " << f;
    ASSERT_EQ(int(pf.sel), int(pw.sel)) << "AY selection diverged, frame " << f;
  }
}

TEST(FastTierMachine, BootTypesAndSoundsInLockstepWithWake) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  Twin fast, wake;
  boot(fast, rom, subcycle::Machine::RunTier::Fast);
  boot(wake, rom, subcycle::Machine::RunTier::Wake);
  ASSERT_EQ(fast.m.effective_run_tier(), subcycle::Machine::RunTier::Fast)
      << "stock 6128: Fast must not degrade";
  ASSERT_EQ(wake.m.effective_run_tier(), subcycle::Machine::RunTier::Wake);

  int frame_no = 0;
  for (int i = 0; i < 120; ++i) {  // to the Ready screen
    fast.frame();
    wake.frame();
    ++frame_no;
    ASSERT_EQ(fnv1a(fast.fb.data(), kFbLen), fnv1a(wake.fb.data(), kFbLen))
        << "framebuffer diverged at boot frame " << frame_no;
  }
  // Screenshot guard: a doubly-black pair must not pass.
  int nonzero = 0;
  for (uint8_t v : fast.fb)
    if (v) nonzero++;
  ASSERT_GT(nonzero, 100000) << "the Fast tier painted the boot screen";

  // Type: sound 1,239,50,15 <RETURN> — the keyboard path (PPI/PSG events)
  // and then the beep (AY tone through the batch audio pipeline).
  const uint8_t seq[] = {0x74, 0x42, 0x52, 0x56, 0x75, 0x57,  // "sound "
                         0x80, 0x47,                          // "1,"
                         0x81, 0x71, 0x41, 0x47,              // "239,"
                         0x61, 0x40, 0x47,                    // "50,"
                         0x80, 0x61,                          // "15"
                         0x22};                               // RETURN
  for (uint8_t code : seq) tap_both(fast, wake, code, &frame_no);

  int peak = 0;
  for (int i = 0; i < 50; ++i) {  // the note plays for half a second
    fast.frame();
    wake.frame();
    ++frame_no;
    ASSERT_EQ(fnv1a(fast.fb.data(), kFbLen), fnv1a(wake.fb.data(), kFbLen))
        << "framebuffer diverged at frame " << frame_no;
    for (int16_t s : fast.m.audio()) peak = std::max<int>(peak, s < 0 ? -s : s);
  }
  EXPECT_GT(peak, 2000) << "SOUND played through the Fast audio path";

  // The concatenated audio streams: byte-identical over the common prefix,
  // with at most a boundary sample pair of length skew (the frame-cut
  // bucketing — see the header).
  const size_t common = std::min(fast.audio.size(), wake.audio.size());
  const size_t skew = fast.audio.size() > wake.audio.size()
                          ? fast.audio.size() - wake.audio.size()
                          : wake.audio.size() - fast.audio.size();
  EXPECT_LE(skew, 4u) << "audio stream lengths drifted apart";
  ASSERT_GT(common, 100000u) << "audio streams suspiciously short";
  size_t first_diff = common;
  for (size_t i = 0; i < common; ++i) {
    if (fast.audio[i] != wake.audio[i]) {
      first_diff = i;
      break;
    }
  }
  EXPECT_EQ(first_diff, common)
      << "concatenated audio diverged at sample " << first_diff << " (~frame "
      << first_diff / (2 * 882) << "): fast=" << fast.audio[first_diff]
      << " wake=" << wake.audio[first_diff]
      << " next: fast=" << fast.audio[first_diff + 1]
      << " wake=" << wake.audio[first_diff + 1];
}

// F9 (beads-3wyl): tier switches land at frame boundaries over the SAME
// device state — a machine that alternates Fast and Wake every 20 frames
// stays frame-hash-identical (and audio-identical, concatenated) to a
// pure-Wake twin. This exercises the same state-handoff contract a snapshot
// restore does: the device blobs are tier-agnostic, and a switch is exactly
// a "save here, resume under the other dispatch" at the boundary
// (state_roundtrip_test covers the blobs themselves).
TEST(FastTierMachine, TierSwitchesAtFrameBoundariesMatchPureWake) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  Twin sw, wake;
  boot(sw, rom, subcycle::Machine::RunTier::Fast);
  boot(wake, rom, subcycle::Machine::RunTier::Wake);
  for (int f = 0; f < 120; ++f) {
    if (f % 20 == 0)
      sw.m.set_run_tier((f / 20) % 2 == 0 ? subcycle::Machine::RunTier::Fast
                                          : subcycle::Machine::RunTier::Wake);
    sw.frame();
    wake.frame();
    ASSERT_EQ(fnv1a(sw.fb.data(), kFbLen), fnv1a(wake.fb.data(), kFbLen))
        << "switching twin diverged at frame " << f;
  }
  ASSERT_TRUE(sw.audio == wake.audio)
      << "concatenated audio diverged across tier switches";
}

// Regression guard for the Fast-tier frame bound (kMaxFastChars). Context: a
// Machine→Reset in the GUI wedged the emulator with ~36 GB RSS and PC frozen at
// 0x0000. Root cause was a race — the render thread called emulator_reset()
// without quiescing the engine=1 Z80 thread (fixed by quiescing emulator_reset;
// the app-level threaded case isn't reachable from this single-threaded test).
// The *consequence* was in run_frame_fast: with no VSYNC to cut the frame, a
// HALT with interrupts off free-ran the char clock and produced ~20M audio
// samples per "frame". This test exercises the machine-level reset path under
// Fast and asserts the batch stays bounded and the machine recovers — so any
// future change that lets a fast frame run past one frame of chars (the cap's
// invariant) is caught here.
TEST(FastTierMachine, ResetUnderFastDoesNotRunAway) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  Twin t;
  boot(t, rom, subcycle::Machine::RunTier::Fast);
  for (int i = 0; i < 250; ++i) t.frame();  // boot to the BASIC prompt

  t.m.reset();  // the operation that used to hang

  // Every post-reset frame must stay bounded. A real frame is <1000 samples;
  // the pre-fix runaway produced ~20M. A capped-and-bailed frame is a couple
  // thousand at most, so this threshold cleanly separates healthy from runaway.
  constexpr size_t kSaneMaxSamples = 100000;
  for (int i = 0; i < 400; ++i) {
    t.m.run_frame();
    ASSERT_LT(t.m.audio().size(), kSaneMaxSamples)
        << "audio runaway at post-reset frame " << i;
  }

  // Recovered: once the firmware has rebooted, frames VSYNC normally again and
  // audio is back to the ~882-samples/frame steady state.
  t.m.run_frame();
  EXPECT_LT(t.m.audio().size(), 4000u)
      << "machine did not recover to normal framing after reset";
}
