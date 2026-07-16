/* tape_test.cpp — the cassette deck Device (src/hw/tape): a synthesized CDT
 * plays onto the rdata wire; the test DECODES the wire back (pilot count,
 * sync, bit pulse-pairs) and must recover the bytes exactly, with pulse
 * durations matching the spec's 7/32 time base. Line-in mode follows the
 * host level under motor gating. See docs/hardware/tape-device.md. */

#include "hw/tape.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "hw/board.h"

namespace {

std::vector<uint8_t> cdt_std_block(const std::vector<uint8_t>& payload,
                                   uint16_t pause_ms) {
  std::vector<uint8_t> c = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20};
  c.push_back(0x10);
  c.push_back(pause_ms & 0xFF);
  c.push_back(pause_ms >> 8);
  c.push_back(payload.size() & 0xFF);
  c.push_back(payload.size() >> 8);
  c.insert(c.end(), payload.begin(), payload.end());
  return c;
}

struct Pulse {
  uint8_t level;
  uint32_t cycles;
};

// Run the deck standalone with the motor relay held on; record the wire.
std::vector<Pulse> record(const Device& dev, long max_cycles) {
  std::vector<Pulse> pulses;
  Bus in = bus_resting();
  in.tape.motor = true;
  uint8_t prev = 0;
  uint32_t run = 0;
  for (long i = 0; i < max_cycles; ++i) {
    Bus out = bus_resting();
    dev.tick(dev.self, &in, &out);
    const uint8_t lv = out.tape.rdata ? 1 : 0;
    if (lv != prev) {
      pulses.push_back(Pulse{prev, run});
      prev = lv;
      run = 0;
    }
    run++;
    TapeRegs r{};
    tape_peek(&dev, &r);
    if (!r.playing && i > 1000) break;  // auto-stopped at end of tape
  }
  return pulses;
}

}  // namespace

TEST(Tape, StandardBlockDecodesBackByteExact) {
  const std::vector<uint8_t> payload = {0xA5, 0x5A, 0xFF, 0x00, 0xCC};
  const std::vector<uint8_t> cdt = cdt_std_block(payload, 0);

  std::vector<uint8_t> mem(tape_state_size());
  Device dev = tape_init(mem.data());
  ASSERT_EQ(tape_attach_cdt(&dev, cdt.data(), cdt.size()), 0);
  tape_play(&dev, 1);

  const std::vector<Pulse> pulses = record(dev, 60'000'000);
  // Flag byte 0xA5 >= 0x80 -> data-block pilot: 3223 pulses of 2168 T-states.
  // 2168 ts * 32/7 = 9910.9 master cycles: integer accumulation gives
  // 9910/9911.
  ASSERT_GE(pulses.size(), 3223u + 2 + payload.size() * 16);
  for (int i = 1; i < 3223; ++i)
    EXPECT_NEAR(pulses[i].cycles, 9911, 2) << "pilot pulse " << i;
  // Sync pair: 667 and 735 ts.
  EXPECT_NEAR(pulses[3223].cycles, 667 * 32 / 7, 2);
  EXPECT_NEAR(pulses[3224].cycles, 735 * 32 / 7, 2);
  // Bits: pulse PAIRS, 855 ts (0) vs 1710 ts (1); threshold between them.
  std::vector<uint8_t> decoded;
  size_t p = 3225;
  for (size_t byte = 0; byte < payload.size(); ++byte) {
    uint8_t v = 0;
    for (int bit = 0; bit < 8; ++bit) {
      ASSERT_LT(p + 1, pulses.size());
      const bool one = pulses[p].cycles > 5800;  // midpoint of 3909/7817
      EXPECT_NEAR(pulses[p].cycles, pulses[p + 1].cycles, 3)
          << "pulse pair symmetric";
      v = static_cast<uint8_t>((v << 1) | (one ? 1 : 0));
      p += 2;
    }
    decoded.push_back(v);
  }
  EXPECT_EQ(decoded, payload) << "the wire carries the bytes, byte-exact";
  TapeRegs r{};
  tape_peek(&dev, &r);
  EXPECT_EQ(r.playing, 0) << "auto-stop at end of tape";
  EXPECT_EQ(r.error, 0);
}

// An unknown block id must LATCH the error flag and stop — the deck never
// guesses a block length (tape.cpp start_block default, spec §3). Currently
// only the 0x10 path was exercised.
TEST(Tape, UnknownBlockLatchesErrorAndStops) {
  std::vector<uint8_t> cdt = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20};
  cdt.push_back(0x99);  // no such block id
  cdt.push_back(0x00);
  cdt.push_back(0x00);
  std::vector<uint8_t> mem(tape_state_size());
  Device dev = tape_init(mem.data());
  ASSERT_EQ(tape_attach_cdt(&dev, cdt.data(), cdt.size()), 0);
  tape_play(&dev, 1);
  record(dev, 100'000);  // a few ticks: start_block runs and bails
  TapeRegs r{};
  tape_peek(&dev, &r);
  EXPECT_EQ(r.error, 1) << "unknown block latched the error";
  EXPECT_EQ(r.playing, 0) << "and stopped the deck";
}

// A 0x20 pause block with ms == 0 is the TZX "Stop the tape" marker: clean halt,
// NOT an error (distinguishes it from the unknown-block path above).
TEST(Tape, StopBlockHaltsWithoutError) {
  std::vector<uint8_t> cdt = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20};
  cdt.push_back(0x20);  // pause/stop
  cdt.push_back(0x00);  // ms = 0 → stop
  cdt.push_back(0x00);
  std::vector<uint8_t> mem(tape_state_size());
  Device dev = tape_init(mem.data());
  ASSERT_EQ(tape_attach_cdt(&dev, cdt.data(), cdt.size()), 0);
  tape_play(&dev, 1);
  record(dev, 100'000);
  TapeRegs r{};
  tape_peek(&dev, &r);
  EXPECT_EQ(r.playing, 0) << "the stop marker halted the deck";
  EXPECT_EQ(r.error, 0) << "a clean stop is not an error";
}

// A 0x11 turbo-speed block carries its own timings; the wire must honour them.
// Build one with the standard pilot period (2168 ts) and a short pilot, then
// confirm it plays (pilot pulses at 2168 ts ≈ 9911 master cycles), error-free.
TEST(Tape, TurboBlockHonoursItsOwnTimings) {
  auto w16 = [](std::vector<uint8_t>& c, uint16_t v) {
    c.push_back(v & 0xFF);
    c.push_back(v >> 8);
  };
  std::vector<uint8_t> cdt = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20};
  cdt.push_back(0x11);
  w16(cdt, 2168);  // pilot pulse length
  w16(cdt, 667);   // sync1
  w16(cdt, 735);   // sync2
  w16(cdt, 855);   // bit-0 pulse
  w16(cdt, 1710);  // bit-1 pulse
  w16(cdt, 200);   // pilot pulse count
  cdt.push_back(8);  // last-byte used bits
  w16(cdt, 0);       // pause after
  cdt.push_back(1);  // data length (3 bytes, little-endian)
  cdt.push_back(0);
  cdt.push_back(0);
  cdt.push_back(0xFF);  // one data byte

  std::vector<uint8_t> mem(tape_state_size());
  Device dev = tape_init(mem.data());
  ASSERT_EQ(tape_attach_cdt(&dev, cdt.data(), cdt.size()), 0);
  tape_play(&dev, 1);
  const std::vector<Pulse> pulses = record(dev, 20'000'000);
  ASSERT_GE(pulses.size(), 200u) << "the turbo pilot tone played";
  for (int i = 1; i < 50; ++i)
    EXPECT_NEAR(pulses[i].cycles, 2168 * 32 / 7, 2)
        << "pilot pulse honours the block's own 2168-ts period";
  TapeRegs r{};
  tape_peek(&dev, &r);
  EXPECT_EQ(r.error, 0) << "the turbo block decoded without error";
}

namespace {
void w16(std::vector<uint8_t>& c, uint16_t v) {
  c.push_back(v & 0xFF);
  c.push_back(v >> 8);
}
std::vector<uint8_t> tzx_header() {
  return {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20};
}
std::vector<Pulse> play(const std::vector<uint8_t>& cdt, Device& dev,
                        std::vector<uint8_t>& mem) {
  mem.assign(tape_state_size(), 0);
  dev = tape_init(mem.data());
  EXPECT_EQ(tape_attach_cdt(&dev, cdt.data(), cdt.size()), 0);
  tape_play(&dev, 1);
  return record(dev, 20'000'000);
}
}  // namespace

// A 0x12 pure-tone block: pilot_count pulses of the given period, no data.
TEST(Tape, PureToneBlockPlaysThePilotCount) {
  std::vector<uint8_t> cdt = tzx_header();
  cdt.push_back(0x12);
  w16(cdt, 2168);  // tone pulse length
  w16(cdt, 50);    // number of pulses
  std::vector<uint8_t> mem;
  Device dev{};
  const std::vector<Pulse> pulses = play(cdt, dev, mem);
  ASSERT_GE(pulses.size(), 50u) << "the pure tone played its pulses";
  for (int i = 1; i < 40; ++i)
    EXPECT_NEAR(pulses[i].cycles, 2168 * 32 / 7, 2) << "tone period honoured";
  TapeRegs r{};
  tape_peek(&dev, &r);
  EXPECT_EQ(r.error, 0);
}

// A 0x13 pulse-sequence block: explicit per-pulse lengths appear on the wire.
TEST(Tape, PulseSequenceBlockEmitsEachExplicitPulse) {
  std::vector<uint8_t> cdt = tzx_header();
  cdt.push_back(0x13);
  cdt.push_back(3);  // three pulses
  w16(cdt, 1000);
  w16(cdt, 2000);
  w16(cdt, 3000);
  std::vector<uint8_t> mem;
  Device dev{};
  const std::vector<Pulse> pulses = play(cdt, dev, mem);
  ASSERT_GE(pulses.size(), 3u);
  EXPECT_NEAR(pulses[0].cycles, 1000 * 32 / 7, 2);
  EXPECT_NEAR(pulses[1].cycles, 2000 * 32 / 7, 2);
  EXPECT_NEAR(pulses[2].cycles, 3000 * 32 / 7, 2);
  TapeRegs r{};
  tape_peek(&dev, &r);
  EXPECT_EQ(r.error, 0);
}

// A 0x14 pure-data block: straight into data bits (no pilot/sync). One 0xFF byte
// → eight '1' bits, each a pulse pair of the bit-1 period.
TEST(Tape, PureDataBlockClocksBitsWithoutAPilot) {
  std::vector<uint8_t> cdt = tzx_header();
  cdt.push_back(0x14);
  w16(cdt, 855);      // bit-0 pulse
  w16(cdt, 1710);     // bit-1 pulse
  cdt.push_back(8);   // last-byte used bits
  w16(cdt, 0);        // pause
  cdt.push_back(1);   // data length = 1 (3 bytes LE)
  cdt.push_back(0);
  cdt.push_back(0);
  cdt.push_back(0xFF);  // 8 one-bits
  std::vector<uint8_t> mem;
  Device dev{};
  const std::vector<Pulse> pulses = play(cdt, dev, mem);
  ASSERT_GE(pulses.size(), 16u) << "8 one-bits × 2 pulses each, no pilot";
  EXPECT_NEAR(pulses[0].cycles, 1710 * 32 / 7, 2) << "first bit is a '1'";
  EXPECT_NEAR(pulses[1].cycles, pulses[0].cycles, 3) << "the pair is symmetric";
  TapeRegs r{};
  tape_peek(&dev, &r);
  EXPECT_EQ(r.error, 0);
}

TEST(Tape, MotorGatesPlaybackAndLineInFollowsHostLevel) {
  const std::vector<uint8_t> cdt = cdt_std_block({0x00}, 0);
  std::vector<uint8_t> mem(tape_state_size());
  Device dev = tape_init(mem.data());
  ASSERT_EQ(tape_attach_cdt(&dev, cdt.data(), cdt.size()), 0);
  tape_play(&dev, 1);

  // Motor OFF: the timeline must not advance (firmware owns the relay).
  Bus in = bus_resting();
  in.tape.motor = false;
  Bus out = bus_resting();
  TapeRegs before{}, after{};
  tape_peek(&dev, &before);
  for (int i = 0; i < 100000; ++i) dev.tick(dev.self, &in, &out);
  tape_peek(&dev, &after);
  EXPECT_EQ(before.level, after.level) << "no pulses while the relay is off";

  // Line-in: rdata follows the host level, still motor-gated.
  tape_line_mode(&dev, 1);
  tape_line_level(&dev, 1);
  in.tape.motor = true;
  dev.tick(dev.self, &in, &out);
  EXPECT_TRUE(out.tape.rdata) << "line level passes through with motor on";
  in.tape.motor = false;
  dev.tick(dev.self, &in, &out);
  EXPECT_FALSE(out.tape.rdata) << "a real deck wired to the DIN goes quiet";
}

TEST(Tape, DirectRecordingBlockDrivesLevelsSampleExact) {
  // 0x15 direct recording (the .voc mirror path): each data bit IS the line
  // level, held for the block's t-states-per-sample — no pulse pairs.
  std::vector<uint8_t> cdt = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20};
  const uint16_t tst = 500;
  const std::vector<uint8_t> samples = {0xF0, 0xAA};  // 11110000 10101010
  cdt.push_back(0x15);
  cdt.push_back(tst & 0xFF);
  cdt.push_back(tst >> 8);
  cdt.push_back(0);  // no trailing pause
  cdt.push_back(0);
  cdt.push_back(8);  // all bits of the last byte used
  cdt.push_back(samples.size() & 0xFF);
  cdt.push_back(0);
  cdt.push_back(0);
  cdt.insert(cdt.end(), samples.begin(), samples.end());

  std::vector<uint8_t> mem(tape_state_size());
  Device dev = tape_init(mem.data());
  ASSERT_EQ(tape_attach_cdt(&dev, cdt.data(), cdt.size()), 0);
  tape_play(&dev, 1);

  const std::vector<Pulse> pulses = record(dev, 1'000'000);
  TapeRegs r{};
  tape_peek(&dev, &r);
  EXPECT_EQ(r.error, 0) << "0x15 must play, not stop the deck in error";

  const uint32_t one = tst * 32 / 7;  // one sample on the wire
  ASSERT_GE(pulses.size(), 10u);
  EXPECT_EQ(pulses[1].level, 1);  // 1111 — one long high run
  EXPECT_NEAR(pulses[1].cycles, 4 * one, 12);
  EXPECT_EQ(pulses[2].level, 0);  // 0000 — one long low run
  EXPECT_NEAR(pulses[2].cycles, 4 * one, 12);
  for (int i = 3; i < 10; ++i) {  // 10101010 alternates sample by sample
    EXPECT_EQ(pulses[i].level, (i % 2) ? 1 : 0) << "sample " << i;
    EXPECT_NEAR(pulses[i].cycles, one, 6) << "sample " << i;
  }
}
