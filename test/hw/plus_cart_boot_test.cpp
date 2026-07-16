/* plus_cart_boot_test.cpp — boot the 6128+ system cartridge through the
 * clean-room Machine (the SAME engine the GUI runs) and prove the CPU reaches
 * the cartridge menu instead of derailing into low memory or HALTing on a
 * starved interrupt. This is the deterministic, headless repro for the
 * intermittent GUI boot freeze: the engine is fully zero-initialised, so one
 * run here settles whether the freeze lives in the engine or in the GUI's
 * threaded driving of it. Skipped if rom/system.cpr is absent. */

#include <gtest/gtest.h>

#include <sstream>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "diff_harness.h"
#include "hw/gate_array.h"
#include "hw/video.h"
#include "hw/z80.h"
#include "subcycle/machine.h"

namespace {

std::vector<uint8_t> read_file(const char* a, const char* b) {
  std::ifstream f(a, std::ios::binary);
  if (!f) f = std::ifstream(b, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

// Minimal CPR (RIFF/AMS!) parser: 12-byte header, then 8-byte-header cbXX
// chunks, each up to 16K, placed at consecutive 16K bank slots. Returns a flat
// 32-bank (512K) image, unused banks zeroed. Empty on failure.
std::vector<uint8_t> parse_cpr(const std::vector<uint8_t>& raw) {
  constexpr size_t kBank = 0x4000, kBanks = 32;
  std::vector<uint8_t> flat(kBank * kBanks, 0);
  if (raw.size() < 12 || std::memcmp(raw.data(), "RIFF", 4) != 0 ||
      std::memcmp(raw.data() + 8, "AMS!", 4) != 0)
    return {};
  size_t off = 12;
  int placed = 0;
  while (off + 8 <= raw.size()) {
    const uint32_t sz = static_cast<uint32_t>(raw[off + 4]) |
                        (raw[off + 5] << 8) | (raw[off + 6] << 16) |
                        (static_cast<uint32_t>(raw[off + 7]) << 24);
    // Place cbNN chunks at bank NN; skip any other chunk (e.g. the leading
    // zero-length "fmt "). Getting this wrong shifts the OS off 0x0000.
    if (raw[off] == 'c' && raw[off + 1] == 'b') {
      const int bank = (raw[off + 2] - '0') * 10 + (raw[off + 3] - '0');
      const size_t keep = std::min<size_t>(sz, kBank);
      if (bank >= 0 && bank < static_cast<int>(kBanks) &&
          off + 8 + keep <= raw.size()) {
        std::memcpy(&flat[static_cast<size_t>(bank) * kBank], &raw[off + 8],
                    keep);
        placed++;
      }
    }
    off += 8 + ((sz + 1) & ~1u);  // chunks are word-padded
  }
  return placed ? flat : std::vector<uint8_t>{};
}

}  // namespace

// The whole boot: no keys, just power-on. A healthy machine reaches the
// cartridge menu and its firmware sweeps a wide PC range every frame. A frozen
// one collapses to a HALT or a tight spin (the derail-to-low-memory signature).
TEST(PlusCartBoot, ReachesMenuWithoutDerailing) {
  std::vector<uint8_t> raw =
      read_file("rom/system.cpr", "../rom/system.cpr");
  if (raw.size() < 0x8000) GTEST_SKIP() << "rom/system.cpr not found";
  std::vector<uint8_t> cart = parse_cpr(raw);
  ASSERT_FALSE(cart.empty()) << "system.cpr is not a valid RIFF/AMS! container";

  std::vector<uint8_t> amsdos = read_file("rom/amsdos.rom", "../rom/amsdos.rom");
  // cb00 is the OS (boots at 0x0000): a misparse that shifts it off 0x0000
  // sends the CPU into NOPs → garbage, the exact "big blue nothing" symptom.
  ASSERT_EQ(cart[0], 0x01) << "cb00[0] must be the OS boot opcode (LD BC,nn)";

  subcycle::Machine m;
  ASSERT_TRUE(m.build(cart.data(), 0x8000)) << "build from cartridge bank 0+1";
  m.attach_cartridge(cart.data(), cart.size());
  m.set_asic(true);  // model 3: the ASIC is on the board
  if (amsdos.size() >= 0x4000) m.attach_amsdos(amsdos.data(), amsdos.size());

  std::vector<uint8_t> fb(
      static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3, 0);
  m.attach_framebuffer(fb.data(), subcycle::kFbWidth, subcycle::kFbHeight);

  // Run ~250 frames (~5 s CPC time) — well past the menu draw. Track the PC
  // range over the LAST 40 frames: a live firmware loop wanders over hundreds
  // of bytes; a derailed one is pinned in a handful.
  uint16_t pc_lo = 0xFFFF, pc_hi = 0;
  int halted_frames = 0, first_paint = -1;
  const int kTotal = 250, kWindow = 40;
  for (int frame = 0; frame < kTotal; ++frame) {
    for (uint8_t row = 0; row < 16; ++row)
      m.set_key_row(row, 0xFF);  // no keys pressed, as the GUI feeds every frame
    m.run_frame();
    if (first_paint < 0) {  // when did anything first hit the framebuffer?
      for (uint8_t v : fb)
        if (v) {
          first_paint = frame;
          break;
        }
    }
    if (frame >= kTotal - kWindow) {
      const Z80Regs r = m.regs();
      pc_lo = std::min(pc_lo, r.pc);
      pc_hi = std::max(pc_hi, r.pc);
      if (r.halted) halted_frames++;
    }
  }
  const int pc_span = static_cast<int>(pc_hi) - static_cast<int>(pc_lo);

  VideoRegs vr{};
  video_peek(m.video(), &vr);
  GateArrayRegs g{};
  ga_peek(m.gate_array(), &g);

  int nonzero = 0;
  for (uint8_t v : fb)
    if (v) nonzero++;

  std::fprintf(stderr,
               "PLUSBOOT: frames=%u mode=%u ink0=%u ink1=%u pc_span=%d "
               "[%04X..%04X] halted=%d/%d first_paint=%d fb_nonzero=%d\n",
               vr.frames, g.mode, g.ink[0], g.ink[1], pc_span, pc_lo, pc_hi,
               halted_frames, kWindow, first_paint, nonzero);

  // Dump the final frame as a PPM so the screen can be inspected.
  if (FILE* out = std::fopen("/tmp/plus_cart_boot.ppm", "wb")) {
    std::fprintf(out, "P6\n%d %d\n255\n", subcycle::kFbWidth,
                 subcycle::kFbHeight);
    std::fwrite(fb.data(), 1, fb.size(), out);
    std::fclose(out);
  }

  EXPECT_EQ(halted_frames, 0)
      << "CPU HALTed waiting for an interrupt that never came (freeze)";
  EXPECT_GT(pc_span, 64)
      << "CPU pinned in a tiny PC window — derailed/spin-frozen, not running "
         "the menu loop";
  EXPECT_GT(nonzero, 1000) << "the menu never painted (blank screen)";
}


// Thin integration smoke check (NOT the primary correctness guard — the
// hardware invariants live in Device-level unit tests):
//   - the RMR2 register-page mapping gate: asic_test.cpp
//     RegisterPageNeedsRmr2MapNotJustUnlock,
//   - the per-scanline mode latch (raster split): gate_array_test.cpp
//     ModeLatchesPerScanlineForRasterSplit,
//   - RMR2/mode gating + mode-0 decode: plus_rmr2_mode_test.cpp, video_test.cpp.
//
// This test proves the whole clean-room boots the real cartridge and the
// Burnin' Rubber title SURVIVES: it used to render for ~2 s and then derail back
// to the cartridge menu, because a bulk copy through &6xxx (while the register
// page was RMR2-paged OUT) scribbled the ASIC DMA control, spuriously enabling
// DMA whose INT flags corrupted the title's per-frame interrupt script. A
// healthy title keeps its full-colour screen many seconds; a derail collapses it
// to a near-blank menu.
TEST(PlusCartBoot, BurninTitleHoldsWithoutDerailing) {
  std::vector<uint8_t> raw = read_file("rom/system.cpr", "../rom/system.cpr");
  if (raw.size() < 0x8000) GTEST_SKIP() << "rom/system.cpr not found";
  std::vector<uint8_t> cart = parse_cpr(raw);
  ASSERT_FALSE(cart.empty());
  std::vector<uint8_t> amsdos = read_file("rom/amsdos.rom", "../rom/amsdos.rom");

  subcycle::Machine m;
  ASSERT_TRUE(m.build(cart.data(), 0x8000));
  m.attach_cartridge(cart.data(), cart.size());
  m.set_asic(true);
  if (amsdos.size() >= 0x4000) m.attach_amsdos(amsdos.data(), amsdos.size());
  std::vector<uint8_t> fb(
      static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3, 0);
  m.attach_framebuffer(fb.data(), subcycle::kFbWidth, subcycle::kFbHeight);

  auto idle_frame = [&] {
    for (uint8_t r = 0; r < 16; ++r) m.set_key_row(r, 0xFF);
    m.run_frame();
  };
  auto distinct_colors = [&]() {
    std::vector<uint32_t> seen;
    for (size_t i = 0; i + 2 < fb.size(); i += 3) {
      const uint32_t c = (fb[i] << 16) | (fb[i + 1] << 8) | fb[i + 2];
      if (std::find(seen.begin(), seen.end(), c) == seen.end()) {
        seen.push_back(c);
        if (seen.size() > 40) break;
      }
    }
    return static_cast<int>(seen.size());
  };

  // Boot to the cartridge menu, select Burnin' Rubber (F2 = row 1 bit 6), then
  // run well past the ~2 s point where the derail used to strike.
  for (int f = 0; f < 150; ++f) idle_frame();
  for (int f = 0; f < 6; ++f) {
    for (uint8_t r = 0; r < 16; ++r) m.set_key_row(r, 0xFF);
    m.set_key_row(1, 0xBF);  // F2 held
    m.run_frame();
  }
  for (int f = 0; f < 40; ++f) idle_frame();  // let the title paint
  const int colors_early = distinct_colors();
  EXPECT_GT(colors_early, 8) << "the multi-colour title never painted";

  for (int f = 0; f < 250; ++f) idle_frame();  // ~5 s — long past the derail
  EXPECT_GT(distinct_colors(), 8)
      << "the title collapsed to a near-blank screen — it derailed back to the "
         "cartridge menu instead of holding";

  // The derail's mechanism: a bulk copy through &6xxx must never enable DMA on
  // this DMA-less title. Any enabled channel here is the regression.
  uint8_t en0 = 0, en1 = 0, en2 = 0;
  asic_dma_regs(m.asic(), 0, nullptr, nullptr, &en0);
  asic_dma_regs(m.asic(), 1, nullptr, nullptr, &en1);
  asic_dma_regs(m.asic(), 2, nullptr, nullptr, &en2);
  EXPECT_EQ(en0 | en1 | en2, 0)
      << "a bulk copy through &6xxx spuriously enabled ASIC DMA (the derail)";
}

// Thin smoke check for the RACE-START derail (root cause unit-tested in
// asic_test.cpp Rmr2MembankSelectsLowRomSlot). Pressing FIRE 1 from the title
// runs Burnin' Rubber's RAM-LAM restart, which uses RMR2 membank 2 to park the
// cartridge at &8000 while the CPU fetches a RET from RAM at &004E. When the
// clean-room forced the RMR2 low-ROM bank to &0000 instead of the membank slot,
// that fetch hit cartridge data and the Z80 ran off into low memory (screen
// black, PC pinned in a ~20-byte window). A healthy race keeps a live, wide PC
// range in game code and a painted screen.
TEST(PlusCartBoot, BurninRaceStartsOnFireWithoutDerailing) {
  std::vector<uint8_t> raw = read_file("rom/system.cpr", "../rom/system.cpr");
  if (raw.size() < 0x8000) GTEST_SKIP() << "rom/system.cpr not found";
  std::vector<uint8_t> cart = parse_cpr(raw);
  ASSERT_FALSE(cart.empty());
  std::vector<uint8_t> amsdos = read_file("rom/amsdos.rom", "../rom/amsdos.rom");

  subcycle::Machine m;
  ASSERT_TRUE(m.build(cart.data(), 0x8000));
  m.attach_cartridge(cart.data(), cart.size());
  m.set_asic(true);
  if (amsdos.size() >= 0x4000) m.attach_amsdos(amsdos.data(), amsdos.size());
  std::vector<uint8_t> fb(
      static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3, 0);
  m.attach_framebuffer(fb.data(), subcycle::kFbWidth, subcycle::kFbHeight);

  auto idle_frame = [&] {
    for (uint8_t r = 0; r < 16; ++r) m.set_key_row(r, 0xFF);
    m.run_frame();
  };
  auto distinct_colors = [&]() {
    std::vector<uint32_t> seen;
    for (size_t i = 0; i + 2 < fb.size(); i += 3) {
      const uint32_t c = (fb[i] << 16) | (fb[i + 1] << 8) | fb[i + 2];
      if (std::find(seen.begin(), seen.end(), c) == seen.end()) {
        seen.push_back(c);
        if (seen.size() > 40) break;
      }
    }
    return static_cast<int>(seen.size());
  };

  // Boot -> select Burnin' (F2 = row 1 bit 6) -> let the title paint.
  for (int f = 0; f < 150; ++f) idle_frame();
  for (int f = 0; f < 6; ++f) {
    for (uint8_t r = 0; r < 16; ++r) m.set_key_row(r, 0xFF);
    m.set_key_row(1, 0xBF);  // F2
    m.run_frame();
  }
  for (int f = 0; f < 60; ++f) idle_frame();
  ASSERT_GT(distinct_colors(), 8) << "the title never painted";

  // Press FIRE 1 (joystick 0 = keyboard matrix row 9; FIRE1 = bit 4, active-low)
  // for ~10 frames, then release and let the race run.
  for (int f = 0; f < 10; ++f) {
    for (uint8_t r = 0; r < 16; ++r) m.set_key_row(r, 0xFF);
    m.set_key_row(9, 0xEF);  // FIRE1 pressed
    m.run_frame();
  }
  uint16_t pc_lo = 0xFFFF, pc_hi = 0;
  int low_rom_frames = 0;
  const int kFrames = 120;
  for (int f = 0; f < kFrames; ++f) {
    idle_frame();
    const uint16_t pc = m.regs().pc;
    pc_lo = std::min(pc_lo, pc);
    pc_hi = std::max(pc_hi, pc);
    if (pc < 0x0140) low_rom_frames++;  // the derail pins the PC in low ROM
  }

  EXPECT_GT(static_cast<int>(pc_hi) - static_cast<int>(pc_lo), 256)
      << "the CPU is pinned in a tiny PC window — the race derailed to low ROM";
  EXPECT_LT(low_rom_frames, kFrames / 2)
      << "the CPU spends the race stuck in low ROM (the RST-20 runaway)";
  EXPECT_GT(distinct_colors(), 8)
      << "the race screen is blank/black — it derailed on FIRE";
}

// The wake tier on a PLUS: the ASIC is plugged (so the GA stays awake and the
// AY bus has a second master), and the audio stream — not just the framebuffer
// — must match Faithful byte-for-byte. This is the guard for the PSG's
// AY-line-compare wake contract: a heuristic that only tracked PPI wakes would
// sleep the PSG through a bus-master write and desync the sound. Runs the same
// boot → menu → F2 → title script as BurninTitleHoldsWithoutDerailing.
TEST(PlusCartBoot, WakeTierMatchesFaithfulIncludingAudio) {
  std::vector<uint8_t> raw = read_file("rom/system.cpr", "../rom/system.cpr");
  if (raw.size() < 0x8000) GTEST_SKIP() << "rom/system.cpr not found";
  std::vector<uint8_t> cart = parse_cpr(raw);
  ASSERT_FALSE(cart.empty());
  std::vector<uint8_t> amsdos = read_file("rom/amsdos.rom", "../rom/amsdos.rom");

  subcycle::Machine faithful, wake;
  std::vector<std::vector<uint8_t>> fbs(2);
  subcycle::Machine* machines[2] = {&faithful, &wake};
  for (int i = 0; i < 2; ++i) {
    subcycle::Machine& m = *machines[i];
    ASSERT_TRUE(m.build(cart.data(), 0x8000));
    m.attach_cartridge(cart.data(), cart.size());
    m.set_asic(true);
    if (amsdos.size() >= 0x4000) m.attach_amsdos(amsdos.data(), amsdos.size());
    fbs[i].assign(
        static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3, 0);
    m.attach_framebuffer(fbs[i].data(), subcycle::kFbWidth,
                         subcycle::kFbHeight);
  }
  faithful.set_wake(false);  // wake is the default now: pin the reference side
  wake.set_wake(true);
  ASSERT_TRUE(wake.wake_active()) << "canonical Plus composition must qualify";

  int frame = 0;
  auto diverged_names = [&] {  // names the culprit on a deep divergence
    std::vector<uint64_t> lhs = diffharness::per_device_state_hashes(faithful);
    std::vector<uint64_t> rhs = diffharness::per_device_state_hashes(wake);
    std::vector<diffharness::NamedDevice> names =
        diffharness::board_devices(faithful);
    std::string out;
    for (size_t i = 0; i < lhs.size(); ++i)
      if (lhs[i] != rhs[i]) out += std::string(" ") + names[i].name;
    return out;
  };
  auto lockstep = [&](uint8_t held_row, uint8_t held_cols) {
    for (subcycle::Machine* m : machines) {
      for (uint8_t r = 0; r < 16; ++r) m->set_key_row(r, 0xFF);
      if (held_cols != 0xFF) m->set_key_row(held_row, held_cols);
      m->run_frame();
    }
    ASSERT_EQ(0, std::memcmp(fbs[0].data(), fbs[1].data(), fbs[0].size()))
        << "framebuffer diverged at frame " << frame << " —"
        << diverged_names();
    ASSERT_EQ(diffharness::per_device_state_hashes(faithful),
              diffharness::per_device_state_hashes(wake))
        << "device state diverged at frame " << frame << " —"
        << diverged_names();
    ASSERT_EQ(faithful.audio().size(), wake.audio().size())
        << "audio sample count diverged at frame " << frame;
    ASSERT_EQ(0, std::memcmp(faithful.audio().data(), wake.audio().data(),
                             faithful.audio().size() * sizeof(int16_t)))
        << "audio samples diverged at frame " << frame << " —"
        << diverged_names();
    frame++;
  };

  for (int f = 0; f < 150; ++f) lockstep(0, 0xFF);  // boot to the menu
  for (int f = 0; f < 6; ++f) lockstep(1, 0xBF);    // F2: select the title
  for (int f = 0; f < 80; ++f) lockstep(0, 0xFF);   // title paints + music
}


// --- F7 (beads-3boi): the Plus under RunTier::Fast ------------------------
// A Fast-tier machine boots the system cartridge, presses F2, and reaches the
// title in lockstep with a Wake twin: framebuffer hash per frame (the Plus
// compositor, 12-bit palette, PRI timing and register page all feed it) and
// the concatenated audio stream (frame cuts are tier-quantized, so buckets
// may shift by a boundary sample — the stream itself must be byte-equal).
// Frames with a DMA channel enabled degrade to the per-cycle path inside
// run_frame (the F7 scope note in asic.h) — byte-exact either way, and this
// scenario exercises the bail/degrade seams too if the firmware uses DMA.
namespace {
uint64_t fnv1a_fb(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ULL;
  return h;
}
}  // namespace

TEST(PlusCartBoot, FastTierMatchesWakeIncludingAudio) {
  std::vector<uint8_t> raw = read_file("rom/system.cpr", "../rom/system.cpr");
  if (raw.size() < 0x8000) GTEST_SKIP() << "rom/system.cpr not found";
  std::vector<uint8_t> cart = parse_cpr(raw);
  ASSERT_FALSE(cart.empty());
  std::vector<uint8_t> amsdos =
      read_file("rom/amsdos.rom", "../rom/amsdos.rom");

  constexpr size_t kFbLen =
      static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3;
  struct Side {
    subcycle::Machine m;
    std::vector<uint8_t> fb = std::vector<uint8_t>(kFbLen, 0);
    std::vector<int16_t> audio;
  };
  Side fast, wake;
  for (Side* s : {&fast, &wake}) {
    ASSERT_TRUE(s->m.build(cart.data(), 0x8000));
    s->m.attach_cartridge(cart.data(), cart.size());
    s->m.set_asic(true);
    if (amsdos.size() >= 0x4000)
      s->m.attach_amsdos(amsdos.data(), amsdos.size());
    s->m.attach_framebuffer(s->fb.data(), subcycle::kFbWidth,
                            subcycle::kFbHeight);
  }
  fast.m.set_run_tier(subcycle::Machine::RunTier::Fast);
  wake.m.set_run_tier(subcycle::Machine::RunTier::Wake);
  ASSERT_EQ(fast.m.effective_run_tier(), subcycle::Machine::RunTier::Fast)
      << "a Plus board must no longer degrade the Fast tier (F7)";

  // Boot to the menu (150f), hold F2 (6f), release, run into the title (80f).
  auto frames = [&](int n, uint8_t row1) {
    for (int f = 0; f < n; ++f) {
      for (Side* s : {&fast, &wake}) {
        for (uint8_t row = 0; row < 16; ++row)
          s->m.set_key_row(row, row == 1 ? row1 : 0xFF);
        s->m.run_frame();
        s->audio.insert(s->audio.end(), s->m.audio().begin(),
                        s->m.audio().end());
      }
      ASSERT_EQ(fnv1a_fb(fast.fb.data(), kFbLen),
                fnv1a_fb(wake.fb.data(), kFbLen))
          << "Plus framebuffer diverged (phase row1=" << int(row1)
          << ", frame " << f << ")";
    }
  };
  frames(150, 0xFF);  // boot → menu
  frames(6, 0xBF);    // F2 held (row 1 bit 6 low)
  frames(80, 0xFF);   // released → the title sequence

  int nonzero = 0;
  for (uint8_t v : fast.fb)
    if (v) nonzero++;
  ASSERT_GT(nonzero, 100000) << "the Plus screen painted (screenshot guard)";

  const size_t common = std::min(fast.audio.size(), wake.audio.size());
  const size_t skew = fast.audio.size() > wake.audio.size()
                          ? fast.audio.size() - wake.audio.size()
                          : wake.audio.size() - fast.audio.size();
  EXPECT_LE(skew, 4u) << "audio stream lengths drifted";
  ASSERT_GT(common, 100000u);
  size_t first_diff = common;
  for (size_t i = 0; i < common; ++i) {
    if (fast.audio[i] != wake.audio[i]) {
      first_diff = i;
      break;
    }
  }
  EXPECT_EQ(first_diff, common)
      << "Plus audio diverged at sample " << first_diff << " (~frame "
      << first_diff / (2 * 882) << ")";
}

// beads-agha oracle: mid-frame GA mode splits under Plus rendering. The GA
// mode latch moves at HSYNCs INSIDE a batch render run, so render_cell_plus
// must consume the chain-stamped per-char mode exactly like the classic
// path — a mode peeked once per run goes stale for every cell between the
// latching HSYNC and the next I/O event. The injected loop flips mode 0/1
// with co-prime busy-waits (~2.4 and ~3.6 scanlines), so the latch moves
// drift across scanline phases frame over frame and land inside batch runs
// at many different chars. The boot CKSUMs are blind to this (no mid-frame
// mode change); this lockstep is the guard.
TEST(PlusCartBoot, FastTierMatchesWakeOnMidFrameModeSplits) {
  // KNOWN RESIDUAL (beads-agha, open): with view.mode threaded through
  // render_cell_plus the boot workloads are lockstep-exact, but a drifting
  // mid-frame mode-flip loop still diverges one scanline every few frames —
  // a GA write-vs-HSYNC-edge ordering case at some T-slot alignment. A
  // coarse "slot-0 GA writes apply before the char" split fixed THIS oracle
  // but broke the boot lockstep (overcorrection): the true per-cycle
  // ordering needs a board-level write-vs-edge truth table first. Skipped
  // until that lands; run it manually when working the bead.
  // (On failure the dump below prints the differing row span and the first
  // twelve pixel triplets of each side — run widths identify which mode
  // each shape latched: 4-wide runs = mode 0, 2-wide = mode 1.)
  std::vector<uint8_t> raw = read_file("rom/system.cpr", "../rom/system.cpr");
  if (raw.size() < 0x8000) GTEST_SKIP() << "rom/system.cpr not found";
  std::vector<uint8_t> cart = parse_cpr(raw);
  ASSERT_FALSE(cart.empty());

  constexpr size_t kFbLen =
      static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3;
  struct Side {
    subcycle::Machine m;
    std::vector<uint8_t> fb = std::vector<uint8_t>(kFbLen, 0);
  };
  Side fast, wake;
  for (Side* s : {&fast, &wake}) {
    ASSERT_TRUE(s->m.build(cart.data(), 0x8000));
    s->m.attach_cartridge(cart.data(), cart.size());
    s->m.set_asic(true);
    s->m.attach_framebuffer(s->fb.data(), subcycle::kFbWidth,
                            subcycle::kFbHeight);
  }
  fast.m.set_run_tier(subcycle::Machine::RunTier::Fast);
  wake.m.set_run_tier(subcycle::Machine::RunTier::Wake);
  ASSERT_EQ(fast.m.effective_run_tier(), subcycle::Machine::RunTier::Fast);

  // Boot to the menu: the screen RAM then holds non-uniform bytes whose
  // mode-0 and mode-1 decodes differ — a stale mode is visible.
  for (int f = 0; f < 150; ++f) {
    for (Side* s : {&fast, &wake}) {
      for (uint8_t row = 0; row < 16; ++row) s->m.set_key_row(row, 0xFF);
      s->m.run_frame();
    }
  }

  const uint8_t prog[] = {
      0xF3,              // DI            (the firmware ISR must not interfere)
      0x01, 0x8C, 0x7F,  // LD BC,#7F8C   (GA RMR: mode 0, both ROMs off)
      0xED, 0x49,        // OUT (C),C
      0x06, 0x30,        // LD B,#30
      0x10, 0xFE,        // DJNZ $        (~2.4 scanlines)
      0x01, 0x8D, 0x7F,  // LD BC,#7F8D   (GA RMR: mode 1)
      0xED, 0x49,        // OUT (C),C
      0x06, 0x47,        // LD B,#47
      0x10, 0xFE,        // DJNZ $        (~3.6 scanlines — co-prime drift)
      0x18, 0xEC,        // JR -20        (back to the mode-0 OUT)
  };
  for (Side* s : {&fast, &wake}) {
    for (size_t i = 0; i < sizeof(prog); ++i)
      s->m.poke_mem(static_cast<uint16_t>(0xA000 + i), prog[i]);
    Z80Regs r = s->m.regs();
    r.pc = 0xA000;
    s->m.set_regs(r);
  }

  for (int f = 0; f < 100; ++f) {
    for (Side* s : {&fast, &wake}) {
      for (uint8_t row = 0; row < 16; ++row) s->m.set_key_row(row, 0xFF);
      s->m.run_frame();
    }
    if (fnv1a_fb(fast.fb.data(), kFbLen) != fnv1a_fb(wake.fb.data(), kFbLen)) {
      int lo = -1, hi = -1;
      size_t first = 0;
      for (size_t i = 0; i < kFbLen; ++i)
        if (fast.fb[i] != wake.fb[i]) {
          const int row = static_cast<int>((i / 3) / subcycle::kFbWidth);
          if (lo < 0) {
            lo = row;
            first = (i / 3) * 3;
          }
          hi = row;
        }
      std::ostringstream dump;
      dump << "frame " << f << " rows " << lo << ".." << hi << " x0 "
           << (first / 3) % subcycle::kFbWidth << "\nfast:";
      for (int k = 0; k < 12; ++k)
        dump << " " << int(fast.fb[first + k * 3]) << ","
             << int(fast.fb[first + k * 3 + 1]) << ","
             << int(fast.fb[first + k * 3 + 2]);
      dump << "\nwake:";
      for (int k = 0; k < 12; ++k)
        dump << " " << int(wake.fb[first + k * 3]) << ","
             << int(wake.fb[first + k * 3 + 1]) << ","
             << int(wake.fb[first + k * 3 + 2]);
      FAIL() << dump.str();
    }
  }
}
