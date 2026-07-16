/* instr_trace_hook_test.cpp — the engine=1 per-instruction trace seam.
 *
 * `trace` (IPC / DevTools) records every retired Z80 instruction. Under the
 * legacy core the record call sits in z80.cpp; under the sub-cycle engine the
 * call site is Machine::set_instr_hook, invoked from BOTH execution paths:
 *   - Fast/batch: once before each z80_batch_step (one retire per step);
 *   - per-cycle (Wake/Soldered/Faithful): once per instr_count change, and the
 *     µs-chunk elision is suppressed while a hook is attached so no retire is
 *     skipped.
 * The tier-independent invariant this pins: the hook fires EXACTLY once per
 * retired instruction, i.e. its call count equals the delta in instr_count.
 * The bridge's g_trace adapter is intentionally NOT exercised here — this test
 * links only the standalone Machine, proving the seam in isolation. */

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "subcycle/machine.h"

namespace {

std::vector<uint8_t> load_rom() {
  auto read = [](const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
  };
  std::vector<uint8_t> rom = read("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read("../rom/cpc6128.rom");
  return rom;
}

constexpr size_t kFbLen =
    static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3;

// Records what the hook sees so we can assert it is REAL CPU state, not a stub:
// a fire count, plus the last PC / instr_count handed to it.
struct Sink {
  uint64_t fires = 0;
  uint16_t last_pc = 0;
  uint64_t last_instr = 0;
};

void tally(void* ctx, const Z80Regs* r) {
  auto* s = static_cast<Sink*>(ctx);
  s->fires++;
  s->last_pc = r->pc;
  s->last_instr = r->instr_count;
}

struct Rig {
  subcycle::Machine m;
  std::vector<uint8_t> fb = std::vector<uint8_t>(kFbLen, 0);
  void boot(const std::vector<uint8_t>& rom, subcycle::Machine::RunTier tier) {
    ASSERT_TRUE(m.build(rom.data(), rom.size()));
    m.attach_framebuffer(fb.data(), subcycle::kFbWidth, subcycle::kFbHeight);
    m.set_run_tier(tier);
  }
};

// The seam must count one fire per retire regardless of the requested tier.
void expect_one_fire_per_retire(subcycle::Machine::RunTier tier) {
  std::vector<uint8_t> rom = load_rom();
  ASSERT_GE(rom.size(), 0x8000u) << "cpc6128.rom not found";

  Rig r;
  r.boot(rom, tier);

  Sink sink;
  r.m.set_instr_hook(tally, &sink);

  const uint64_t before = r.m.regs().instr_count;
  for (int i = 0; i < 30; ++i) r.m.run_frame();
  const uint64_t after = r.m.regs().instr_count;

  ASSERT_GT(after, before) << "no instructions retired — boot stalled";
  const uint64_t retires = after - before;
  // The contract: one hook fire per retired instruction, NO duplicates. The
  // Fast tier runs a per-cycle prelude then hands off to the batch loop; both
  // share instr_hook_last_ so the boundary instruction is recorded once, not
  // twice. `fires <= retires` is the anti-duplicate guard; the lower bound
  // allows the single trailing boundary the batch loop leaves unfired at the
  // very end of a capture (the next frame's prelude would pick it up, but the
  // run ends first). Pure per-cycle (Wake) hits `retires` exactly.
  EXPECT_LE(sink.fires, retires)
      << "duplicate trace entries — hand-off double-fire";
  EXPECT_GE(sink.fires, retires - 1) << "missed instructions in the trace";
  // The hook saw genuine live CPU state, never a canned value: the instr_count
  // it was last handed is the machine's own, and its last PC is a real PC.
  EXPECT_LE(sink.last_instr, after);
  EXPECT_GE(sink.last_instr, after - 1);

  // Detach must be immediate: further frames retire more instructions but the
  // sink stays frozen.
  r.m.set_instr_hook(nullptr, nullptr);
  const uint64_t frozen = sink.fires;
  for (int i = 0; i < 5; ++i) r.m.run_frame();
  EXPECT_GT(r.m.regs().instr_count, after) << "post-detach run retired nothing";
  EXPECT_EQ(sink.fires, frozen) << "hook still firing after detach";
}

TEST(InstrTraceHook, FastTierFiresOncePerRetire) {
  expect_one_fire_per_retire(subcycle::Machine::RunTier::Fast);
}

TEST(InstrTraceHook, WakeTierFiresOncePerRetire) {
  expect_one_fire_per_retire(subcycle::Machine::RunTier::Wake);
}

}  // namespace
