/* irq_banking_invariants_test.cpp — INVARIANT coverage for the interrupt-vs-
 * memory-banking crash class (beads-pood): under CP/M Plus a raster interrupt
 * is taken while a TPA/data RAM bank is paged at 0xC000+, so the &0038 vector's
 * JP target reads a bank-dependent byte and the CPU runs off into garbage. The
 * live symptom was non-deterministic (the interrupt landed at a different PC
 * each real-time boot).
 *
 * There is NO real-hardware oracle for this scenario (the user has no physical
 * serial interface / CP/M-Plus-under-RSX rig), so we CANNOT assert "Faithful
 * must not crash here" — the real machine's behaviour is unknown. What a
 * faithful, deterministic emulator MUST satisfy regardless of the unknown
 * ground truth are three internal invariants (user decision 2026-07-11,
 * "invariants only"):
 *
 *   1. Cross-tier equivalence — the per-cycle tiers (Faithful, Soldered, Wake)
 *      are three dispatch shapes over ONE machine. Running the same cycle-exact
 *      workload (banking churned while interrupts fire) must leave byte-
 *      identical device state at every frame boundary. A tier that accepts the
 *      interrupt one cycle early/late relative to a bank OUT would map a
 *      different bank at &C000 and diverge here — the pood family.
 *   2. Determinism — the same tier, same workload, from the same reset, twice,
 *      must be byte-identical. Catches uninitialised-read / pointer-identity
 *      non-determinism in the interrupt+banking path (the live "3E FF one run,
 *      18 the next" smell), which a fixed cycle budget must NOT reproduce.
 *   3. Per-config fetch/peek consistency — for every one of the 8 PAL RAM
 *      configs, the value the CPU *executes* a read of at a banked slot equals
 *      what the debugger peek resolves there, equals the bank table's page.
 *      mem_read is the single path the M1 fetch, the data read, the interrupt
 *      vector fetch, AND peek all ride; this pins that they cannot disagree per
 *      config — the structural precondition the pood garbage-read violates only
 *      via TIMING, never via a split mapping.
 *
 * These are guards, not a reproduction: on a correct engine they are GREEN.
 */

#include "subcycle/machine.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "hw/z80.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

std::vector<uint8_t> load_rom() {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  return rom;
}

constexpr size_t kFbLen =
    static_cast<size_t>(subcycle::kFbWidth) * subcycle::kFbHeight * 3;

// The 6128 PAL bank table (memory.cpp kBankTable) — CPU slot → physical page.
constexpr uint8_t kBankTable[8][4] = {
    {0, 1, 2, 3}, {0, 1, 2, 7}, {4, 5, 6, 7}, {0, 3, 2, 7},
    {0, 4, 2, 3}, {0, 5, 2, 3}, {0, 6, 2, 3}, {0, 7, 2, 3},
};

struct Rig {
  subcycle::Machine m;
  std::vector<uint8_t> fb = std::vector<uint8_t>(kFbLen, 0);
};

void boot(Rig& r, const std::vector<uint8_t>& rom,
          subcycle::Machine::RunTier tier) {
  ASSERT_TRUE(r.m.build(rom.data(), rom.size()));
  r.m.attach_framebuffer(r.fb.data(), subcycle::kFbWidth, subcycle::kFbHeight);
  r.m.set_run_tier(tier);
}

// The pood scenario as a forever loop at 0x8000 (slot 2, page 2 in configs
// 0/1, so the CODE stays resident): with the firmware's IM 1 interrupts live,
// alternate the PAL config between 0 ({0,1,2,3}) and 1 ({0,1,2,7}). Both leave
// slots 0-2 at pages 0-2 (code + low RAM present) and differ ONLY at slot 3 —
// so the bank behind 0xC000 (the CP/M vector target region AND the stack) flips
// under live interrupts, exactly the pood shape, without ever unmapping the
// code. A bank OUT and an interrupt acceptance race on the same bus.
void inject_banking_churn(subcycle::Machine& m) {
  const uint8_t prog[] = {
      0xFB,              // 8000 EI              (keep the firmware IRQs live)
      0x01, 0x00, 0x7F,  // 8001 LD BC,#7F00     (PAL config latch)
      0x3E, 0xC0,        // 8004 loop: LD A,#C0  (config 0 → slot3 = page 3)
      0xED, 0x79,        // 8006 OUT (C),A
      0x21, 0x00, 0x03,  // 8008 LD HL,#0300     (delay)
      0x2B,              // 800B d1: DEC HL
      0x7C,              // 800C LD A,H
      0xB5,              // 800D OR L
      0x20, 0xFB,        // 800E JR NZ,d1
      0x3E, 0xC1,        // 8010 LD A,#C1        (config 1 → slot3 = page 7)
      0xED, 0x79,        // 8012 OUT (C),A
      0x21, 0x00, 0x03,  // 8014 LD HL,#0300
      0x2B,              // 8017 d2: DEC HL
      0x7C,              // 8018 LD A,H
      0xB5,              // 8019 OR L
      0x20, 0xFB,        // 801A JR NZ,d2
      0x18, 0xE6,        // 801C JR loop
  };
  uint16_t addr = 0x8000;
  for (uint8_t b : prog) m.poke_mem(addr++, b);
  Z80Regs regs = m.regs();
  regs.pc = 0x8000;
  m.set_regs(regs);
}

// A DETERMINISTIC, reproducible fingerprint of the machine: the full Z80
// architectural state + the 128K physical RAM + the framebuffer. Deliberately
// NOT save_devices() — the SmartWatch/Symbiface RTC samples the HOST wall clock
// at build, so device blobs differ across a clock tick (a real property, not an
// engine bug; snapshots normalise the RTC). Everything hashed here is a pure
// function of the emulated execution, so two correct runs match exactly.
uint64_t arch_hash(subcycle::Machine& m, const std::vector<uint8_t>& fb) {
  uint64_t h = 1469598103934665603ULL;
  auto mix = [&h](const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ULL;
  };
  const Z80Regs r = m.regs();
  const uint16_t words[] = {r.af, r.bc, r.de, r.hl, r.af_, r.bc_, r.de_,
                            r.hl_, r.ix, r.iy, r.sp, r.pc, r.wz};
  mix(words, sizeof(words));
  const uint8_t bytes[] = {r.i, r.r, r.im, r.iff1, r.iff2, r.q, r.halted};
  mix(bytes, sizeof(bytes));
  mix(&r.tstates, sizeof(r.tstates));
  mix(&r.instr_count, sizeof(r.instr_count));
  std::vector<uint8_t> ram(0x20000);
  for (size_t i = 0; i < ram.size(); ++i) ram[i] = m.ram_read(i);
  mix(ram.data(), ram.size());
  mix(fb.data(), fb.size());
  return h;
}

}  // namespace

// Invariant 1 — Faithful ≡ Soldered ≡ Wake, at every frame boundary, over a
// banking-churn-under-interrupts workload. The per-cycle tiers are three
// dispatch shapes over ONE machine and cut frames at the same master-video
// point (fast_tier_machine_test's header: only Fast has the frame-cut skew), so
// their architectural state IS comparable each frame. A tier that accepts the
// interrupt one cycle off relative to a bank OUT would map a different bank at
// slot 3 and diverge here — the pood family.
TEST(IrqBankingInvariants, PerCycleTiersStayIdenticalUnderBankingChurn) {
  std::vector<uint8_t> rom = load_rom();
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  Rig faithful, soldered, wake;
  boot(faithful, rom, subcycle::Machine::RunTier::Faithful);
  boot(soldered, rom, subcycle::Machine::RunTier::Soldered);
  boot(wake, rom, subcycle::Machine::RunTier::Wake);

  // Settle the firmware (IM 1 + EI + valid SP established), then drop all three
  // onto the identical banking-churn loop.
  for (int f = 0; f < 40; ++f) {
    faithful.m.run_frame();
    soldered.m.run_frame();
    wake.m.run_frame();
  }
  inject_banking_churn(faithful.m);
  inject_banking_churn(soldered.m);
  inject_banking_churn(wake.m);

  ASSERT_EQ(faithful.m.effective_run_tier(),
            subcycle::Machine::RunTier::Faithful);
  ASSERT_EQ(wake.m.effective_run_tier(), subcycle::Machine::RunTier::Wake);
  // Soldered may degrade to Faithful on some boards; then it trivially equals
  // Faithful and the Faithful≡Wake comparison carries the signal.

  for (int f = 0; f < 60; ++f) {
    faithful.m.run_frame();
    soldered.m.run_frame();
    wake.m.run_frame();

    const uint64_t hf = arch_hash(faithful.m, faithful.fb);
    const uint64_t hs = arch_hash(soldered.m, soldered.fb);
    const uint64_t hw = arch_hash(wake.m, wake.fb);
    const Z80Regs rf = faithful.m.regs();
    const Z80Regs rw = wake.m.regs();

    ASSERT_EQ(hf, hw) << "Faithful vs Wake diverged at frame " << f
                      << " — an interrupt was accepted at a different point "
                         "relative to a bank OUT (pc F=0x"
                      << std::hex << rf.pc << " W=0x" << rw.pc << " sp F=0x"
                      << rf.sp << " W=0x" << rw.sp << ")";
    ASSERT_EQ(hf, hs) << "Faithful vs Soldered diverged at frame " << f;
  }
}

// Invariant 2 — the same tier, same workload, twice, is identical. A fixed
// cycle budget removes the real-time interrupt-timing jitter, so the live "3E FF
// one run / 18 the next" garbage MUST NOT reappear here; if it does, the
// non-determinism is in the engine, not the wall clock. (Architectural hash
// only: save_devices() legitimately embeds the host-clock RTC — see arch_hash.)
TEST(IrqBankingInvariants, BankingChurnIsDeterministicAcrossRuns) {
  std::vector<uint8_t> rom = load_rom();
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  Rig a, b;
  boot(a, rom, subcycle::Machine::RunTier::Faithful);
  boot(b, rom, subcycle::Machine::RunTier::Faithful);
  for (int f = 0; f < 40; ++f) {
    a.m.run_frame();
    b.m.run_frame();
  }
  inject_banking_churn(a.m);
  inject_banking_churn(b.m);

  for (int f = 0; f < 60; ++f) {
    a.m.run_frame();
    b.m.run_frame();
    const Z80Regs ra = a.m.regs();
    ASSERT_EQ(arch_hash(a.m, a.fb), arch_hash(b.m, b.fb))
        << "two identical Faithful runs diverged at frame " << f
        << " — non-deterministic interrupt/banking path (pc=0x" << std::hex
        << ra.pc << " ic=" << std::dec << ra.instr_count << ")";
  }
}

// Invariant 3 — for each of the 8 PAL configs, the byte the CPU EXECUTES a read
// of at a banked slot equals the debugger peek there, equals the bank table's
// page. Slot 1 (0x4000) is banked RAM in every config with no ROM overlay, so
// it is the clean discriminator. This pins that the fetch path (which the
// interrupt vector fetch shares) and the peek path resolve banking identically
// per config — the split the pood garbage-read would need can only be a TIMING
// artefact, never a per-config mapping disagreement.
TEST(IrqBankingInvariants, PerConfigExecutedReadMatchesPeekAndBankTable) {
  std::vector<uint8_t> rom = load_rom();
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  Rig r;
  boot(r, rom, subcycle::Machine::RunTier::Faithful);
  ASSERT_GE(r.m.ram_size(), size_t{0x20000})
      << "a 128K 6128 (expansion present) is required to bank pages 4-7";

  // Seed a page-unique marker (0xA0 | page) at offset 0x100 of each of the 8
  // physical pages: base pages 0-3 in the 64K core, pages 4-7 in expansion
  // bank 0 (bank bits are 0 for OUT data 0xC0|cfg).
  for (int page = 0; page < 4; ++page)
    r.m.ram_write((static_cast<size_t>(page) << 14) | 0x100,
                  static_cast<uint8_t>(0xA0 | page));
  for (int page = 4; page < 8; ++page)
    r.m.ram_write((0x10000 + (static_cast<size_t>(page - 4) << 14)) | 0x100,
                  static_cast<uint8_t>(0xA0 | page));

  // LD A,(0x4100); HALT — reads the marker at slot 1 (0x4000) offset 0x100.
  const uint8_t prog[] = {0x3A, 0x00, 0x41, 0x76};

  for (int cfg = 0; cfg < 8; ++cfg) {
    r.m.io_write(0x7F00, static_cast<uint8_t>(0xC0 | cfg));
    // Re-lay the program under THIS config so it is resident at 0x8000 whatever
    // page slot 2 now resolves to (config 2 remaps every slot to pages 4-7).
    uint16_t pa = 0x8000;
    for (uint8_t byte : prog) r.m.poke_mem(pa++, byte);

    const uint8_t page = kBankTable[cfg][1];  // slot 1 → physical page
    const uint8_t expected = static_cast<uint8_t>(0xA0 | page);

    EXPECT_EQ(r.m.peek_mem(0x4100), expected)
        << "peek path: config " << cfg << " must resolve slot 1 to page "
        << int(page);

    // Execute the read on a clean, interrupt-masked boundary.
    Z80Regs regs = r.m.regs();
    regs.pc = 0x8000;
    regs.iff1 = regs.iff2 = 0;  // mask IRQs so the single step is exactly LD A
    r.m.set_regs(regs);
    r.m.step_instruction();  // LD A,(0x4100)

    const uint8_t executed = static_cast<uint8_t>(r.m.regs().af >> 8);
    EXPECT_EQ(executed, expected)
        << "execution path: config " << cfg << " read 0x" << std::hex
        << int(executed) << " but the bank table maps slot 1 to page "
        << std::dec << int(page) << " (marker 0x" << std::hex << int(expected)
        << ")";
    EXPECT_EQ(executed, r.m.peek_mem(0x4100))
        << "fetch and peek disagree under config " << cfg;
  }
}
