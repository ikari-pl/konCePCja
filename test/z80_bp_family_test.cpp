/* The three breakpoint lists as one family (beads-3o1.5).
 *
 * The KCPC protocol names an entry by its kind and address (and mask, for
 * I/O), and accepts one only if the probe will arm it: the probe has 32
 * execution, 16 watch and 8 I/O comparators, and an entry beyond those used
 * to be listed and never fire (beads-zqf3).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hw/probe.h"
#include "koncepcja.h"
#include "z80_view.h"

extern t_z80regs z80;

namespace {

std::vector<uint64_t>* g_changes = nullptr;

void changed(uint64_t generation) { g_changes->push_back(generation); }

Z80BpSpec exec_at(word addr) {
  Z80BpSpec spec;
  spec.addr = addr;
  return spec;
}

Z80BpSpec watch_at(word addr, word len, uint8_t access) {
  Z80BpSpec spec;
  spec.kind = Z80BpKind::Watch;
  spec.addr = addr;
  spec.len_or_mask = len;
  spec.access = access;
  return spec;
}

Z80BpSpec io_at(word port, word mask, uint8_t access) {
  Z80BpSpec spec;
  spec.kind = Z80BpKind::Io;
  spec.addr = port;
  spec.len_or_mask = mask;
  spec.access = access;
  return spec;
}

class Z80BpFamilyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    clear_all();
    g_changes = &changes_;
    z80_set_breakpoints_changed_hook(&changed);
  }
  void TearDown() override {
    z80_set_breakpoints_changed_hook(nullptr);
    g_changes = nullptr;
    clear_all();
  }
  static void clear_all() {
    z80_remove_ephemeral_breakpoints();
    z80_clear_breakpoints();
    z80_clear_watchpoints();
    z80_clear_io_breakpoints();
  }

  Z80BpEdit put(const Z80BpSpec& spec) {
    return z80_bp_put(spec, error_, generation_);
  }
  std::vector<Z80BpSpec> list() { return z80_bp_list(generation_); }

  std::vector<uint64_t> changes_;
  std::string error_;
  uint64_t generation_ = 0;
};

}  // namespace

TEST_F(Z80BpFamilyTest, AnEntryIsNamedByItsKindAndAddress) {
  uint64_t const before = z80_breakpoint_generation();
  Z80BpSpec conditional = exec_at(0x4000);
  conditional.condition = "a = 5";
  conditional.pass_count = 3;
  ASSERT_EQ(put(conditional), Z80BpEdit::Ok);
  EXPECT_EQ(generation_, before + 1);
  EXPECT_EQ(generation_, z80_breakpoint_generation());

  // The same address again is the same entry: replaced, not doubled.
  ASSERT_EQ(put(exec_at(0x4000)), Z80BpEdit::Ok);
  std::vector<Z80BpSpec> const entries = list();
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].kind, Z80BpKind::Exec);
  EXPECT_EQ(entries[0].addr, 0x4000);
  EXPECT_TRUE(entries[0].condition.empty());
  EXPECT_EQ(entries[0].pass_count, 0);
  EXPECT_EQ(z80_list_breakpoints_ref().size(), 1U);

  // One generation, and one notice, for each edit.
  EXPECT_EQ(changes_, (std::vector<uint64_t>{before + 1, before + 2}));
}

TEST_F(Z80BpFamilyTest, AConditionThatDoesNotParseAddsNothing) {
  ASSERT_EQ(put(exec_at(0x4000)), Z80BpEdit::Ok);
  uint64_t const before = z80_breakpoint_generation();
  changes_.clear();
  Z80BpSpec bad = exec_at(0x5000);
  bad.condition = "a = = (";
  EXPECT_EQ(put(bad), Z80BpEdit::BadCondition);
  EXPECT_FALSE(error_.empty());
  EXPECT_EQ(generation_, before);
  EXPECT_EQ(z80_breakpoint_generation(), before);
  EXPECT_TRUE(changes_.empty());
  EXPECT_EQ(list().size(), 1U);
  // Nor does it take the entry it would have replaced with it.
  bad.addr = 0x4000;
  EXPECT_EQ(put(bad), Z80BpEdit::BadCondition);
  EXPECT_EQ(list().size(), 1U);
}

TEST_F(Z80BpFamilyTest, TheThirtyThirdExecutionBreakpointIsRefused) {
  for (int i = 0; i < PROBE_MAX_EXEC; ++i) {
    ASSERT_EQ(put(exec_at(static_cast<word>(0x4000 + i))), Z80BpEdit::Ok) << i;
  }
  EXPECT_FALSE(z80_exec_comparator_free());
  uint64_t const before = z80_breakpoint_generation();
  EXPECT_EQ(put(exec_at(0x5000)), Z80BpEdit::LimitReached);
  EXPECT_EQ(z80_breakpoint_generation(), before) << "nothing changed";
  EXPECT_EQ(list().size(), static_cast<size_t>(PROBE_MAX_EXEC));
  // Replacing one of the 32 needs no new comparator.
  Z80BpSpec replacement = exec_at(0x4000);
  replacement.pass_count = 9;
  EXPECT_EQ(put(replacement), Z80BpEdit::Ok);
  EXPECT_EQ(list().size(), static_cast<size_t>(PROBE_MAX_EXEC));
  // And when one goes, there is room for one.
  EXPECT_EQ(z80_bp_remove(Z80BpKind::Exec, 0x4001, 0, generation_),
            Z80BpEdit::Ok);
  EXPECT_TRUE(z80_exec_comparator_free());
  EXPECT_EQ(put(exec_at(0x5000)), Z80BpEdit::Ok);
}

// Every source shares the comparators: the text IPC's and the DevTools'
// breakpoints, and the one-shot breakpoint of a step that runs to an
// address.
TEST_F(Z80BpFamilyTest, EntriesOfEverySourceCountAgainstTheLimit) {
  for (int i = 0; i < PROBE_MAX_EXEC - 2; ++i) {
    ASSERT_EQ(put(exec_at(static_cast<word>(0x4000 + i))), Z80BpEdit::Ok);
  }
  z80_add_breakpoint(0x6000);            // the text IPC's `bp add`
  z80_add_breakpoint_ephemeral(0x7000);  // a walk in progress
  EXPECT_FALSE(z80_exec_comparator_free());
  EXPECT_EQ(put(exec_at(0x5000)), Z80BpEdit::LimitReached);
  // Two entries at one address take one comparator.
  z80_add_breakpoint_cond(0x6000, nullptr, "", 0);
  EXPECT_EQ(put(exec_at(0x6000)), Z80BpEdit::Ok)
      << "an address the probe already watches";
  // The walk's breakpoint is not an entry: not listed, not cleared.
  EXPECT_EQ(list().size(), static_cast<size_t>(PROBE_MAX_EXEC - 1));
  z80_bp_clear(/*exec=*/true, /*watch=*/false, /*io=*/false, generation_);
  EXPECT_TRUE(list().empty());
  ASSERT_EQ(z80_list_breakpoints_ref().size(), 1U);
  EXPECT_EQ(z80_list_breakpoints_ref()[0].type, EPHEMERAL);
}

TEST_F(Z80BpFamilyTest, WatchAndIoEntriesHaveTheirOwnLimits) {
  for (int i = 0; i < PROBE_MAX_WATCH; ++i) {
    ASSERT_EQ(put(watch_at(static_cast<word>(0x9000 + 4 * i), 4, READWRITE)),
              Z80BpEdit::Ok);
  }
  EXPECT_EQ(put(watch_at(0xA000, 1, WRITE)), Z80BpEdit::LimitReached);
  // A watch is named by its address: a new length replaces it.
  EXPECT_EQ(put(watch_at(0x9000, 2, READ)), Z80BpEdit::Ok);
  for (int i = 0; i < PROBE_MAX_IO; ++i) {
    ASSERT_EQ(put(io_at(static_cast<word>(i << 12), 0xFF00, IO_OUT)),
              Z80BpEdit::Ok);
  }
  EXPECT_EQ(put(io_at(0xF800, 0xFF00, IO_IN)), Z80BpEdit::LimitReached);
  // An I/O entry is named by port and mask: the same port under another
  // mask is another entry, and there is no room for it.
  EXPECT_EQ(put(io_at(0x1000, 0xFFFF, IO_IN)), Z80BpEdit::LimitReached);
  EXPECT_EQ(put(io_at(0x1000, 0xFF00, IO_BOTH)), Z80BpEdit::Ok);

  std::vector<Z80BpSpec> const entries = list();
  ASSERT_EQ(entries.size(),
            static_cast<size_t>(PROBE_MAX_WATCH + PROBE_MAX_IO));
  size_t watches = 0;
  for (const Z80BpSpec& entry : entries) {
    if (entry.kind != Z80BpKind::Watch) continue;
    ++watches;
    if (entry.addr == 0x9000) {
      EXPECT_EQ(entry.len_or_mask, 2);
      EXPECT_EQ(entry.access, READ);
    }
  }
  EXPECT_EQ(watches, static_cast<size_t>(PROBE_MAX_WATCH));
}

TEST_F(Z80BpFamilyTest, RemoveAndClearSayWhatTheyDid) {
  ASSERT_EQ(put(exec_at(0x4000)), Z80BpEdit::Ok);
  ASSERT_EQ(put(watch_at(0x9000, 2, WRITE)), Z80BpEdit::Ok);
  ASSERT_EQ(put(io_at(0x7F00, 0xFF00, IO_OUT)), Z80BpEdit::Ok);
  uint64_t before = z80_breakpoint_generation();
  changes_.clear();

  EXPECT_EQ(z80_bp_remove(Z80BpKind::Exec, 0x4001, 0, generation_),
            Z80BpEdit::NotFound);
  EXPECT_EQ(z80_bp_remove(Z80BpKind::Io, 0x7F00, 0xFFFF, generation_),
            Z80BpEdit::NotFound)
      << "the mask is part of an I/O entry's name";
  EXPECT_EQ(generation_, before);
  EXPECT_TRUE(changes_.empty()) << "nothing changed, nothing to announce";

  EXPECT_EQ(z80_bp_remove(Z80BpKind::Io, 0x7F00, 0xFF00, generation_),
            Z80BpEdit::Ok);
  EXPECT_EQ(generation_, before + 1);
  EXPECT_EQ(z80_bp_remove(Z80BpKind::Watch, 0x9000, 0, generation_),
            Z80BpEdit::Ok);
  ASSERT_EQ(list().size(), 1U);

  ASSERT_EQ(put(watch_at(0x9000, 2, WRITE)), Z80BpEdit::Ok);
  before = z80_breakpoint_generation();
  z80_bp_clear(/*exec=*/false, /*watch=*/true, /*io=*/true, generation_);
  EXPECT_EQ(generation_, before + 1) << "one edit, one generation";
  ASSERT_EQ(list().size(), 1U);
  EXPECT_EQ(list()[0].kind, Z80BpKind::Exec);
}

// The text IPC's and the DevTools' edits are announced too: they go through
// the same lists.
TEST_F(Z80BpFamilyTest, EveryEditOfTheListsIsAnnounced) {
  uint64_t const before = z80_breakpoint_generation();
  z80_add_breakpoint(0x4000);
  z80_add_watchpoint(0x9000, 1, WRITE);
  z80_add_io_breakpoint(0x7F00, 0xFF00, IO_OUT);
  z80_del_breakpoint(0x4000);
  ASSERT_EQ(changes_.size(), 4U);
  EXPECT_EQ(changes_.front(), before + 1);
  EXPECT_EQ(changes_.back(), before + 4);
  // A walk's one-shot breakpoint is not an edit.
  changes_.clear();
  z80_add_breakpoint_ephemeral(0x5000);
  z80_remove_ephemeral_breakpoints();
  EXPECT_TRUE(changes_.empty());
  // The entries of every source are listed.
  std::vector<Z80BpSpec> const entries = list();
  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].kind, Z80BpKind::Watch);
  EXPECT_EQ(entries[0].access, WRITE);
  EXPECT_EQ(entries[1].kind, Z80BpKind::Io);
  EXPECT_EQ(entries[1].len_or_mask, 0xFF00);
}

// The probe's comparator knows the port and the direction. The condition
// and the pass count of an I/O breakpoint are judged here, and until this
// filter existed they were not judged at all.
TEST_F(Z80BpFamilyTest, AnIoHitHonoursDirectionConditionAndPassCount) {
  EXPECT_TRUE(z80_probe_io_should_break(0x7F10, 0x10, true))
      << "a probe armed with no list keeps breaking";

  Z80BpSpec conditional = io_at(0x7F00, 0xFF00, IO_OUT);
  conditional.condition = "value = 0x10";
  ASSERT_EQ(put(conditional), Z80BpEdit::Ok);
  EXPECT_FALSE(z80_probe_io_should_break(0x7F10, 0x11, true))
      << "the condition does not hold";
  EXPECT_FALSE(z80_probe_io_should_break(0x7F10, 0x10, false))
      << "an IN, and the entry watches OUT";
  EXPECT_TRUE(z80_probe_io_should_break(0x7F10, 0x10, true));
  EXPECT_EQ(list()[0].hit_count, 1) << "only the hit that counted";

  Z80BpSpec third = io_at(0x7F00, 0xFF00, IO_BOTH);
  third.pass_count = 3;
  ASSERT_EQ(put(third), Z80BpEdit::Ok);
  EXPECT_FALSE(z80_probe_io_should_break(0x7F10, 0, true));
  EXPECT_FALSE(z80_probe_io_should_break(0x7F10, 0, false));
  EXPECT_TRUE(z80_probe_io_should_break(0x7F10, 0, true));
  EXPECT_EQ(list()[0].hit_count, 3);
  EXPECT_EQ(list()[0].pass_count, 3);
}
