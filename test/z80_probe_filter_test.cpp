// konCePCja — probe hits must be judged honestly by the host post-filters.
//
// WHY THIS FILE EXISTS
//
// beads-tib2, the other half. Two post-filter defects made the debugger
// swallow real hits:
//
// 1. z80_probe_watch_should_break() ignored every argument and returned
//    `watchpoints.empty()` — with ANY watchpoint armed, EVERY memory hit the
//    probe latched was silently resumed. `wp add` armed cleanly; nothing
//    ever fired. (This also produced the false "zero writes during boot"
//    reading that misdirected the beads-qgxr investigation.)
//
// 2. z80_probe_exec_should_break(pc) evaluated conditions against the
//    register VIEW as the machine parked it — mid-fetch, PC already past the
//    opcode — while the ack path set the view's PC to the hit identity only
//    AFTER filtering. `bp add X if pc == X` was false at its own breakpoint.
//
// These tests drive the post-filters directly with the host lists set up the
// way the IPC server sets them up. Both fail on the pre-fix tree.

#include <gtest/gtest.h>

#include <string>

#include "expr_parser.h"
#include "z80_view.h"

extern t_z80regs z80;

namespace {

class ProbeFilterTest : public testing::Test {
 protected:
  void SetUp() override {
    z80_clear_breakpoints();
    z80_clear_watchpoints();
  }
  void TearDown() override {
    z80_clear_breakpoints();
    z80_clear_watchpoints();
  }

  static std::unique_ptr<ExprNode> parse(const std::string& s) {
    std::string err;
    auto ast = expr_parse(s, err);
    EXPECT_NE(ast, nullptr) << s << ": " << err;
    return ast;
  }
};

}  // namespace

// The swallow: a plain armed watchpoint must break on its own hit.
TEST_F(ProbeFilterTest, APlainWatchpointBreaksOnItsHit) {
  z80_add_watchpoint(0xB7F8, 1, WatchpointType::WRITE);
  EXPECT_TRUE(z80_probe_watch_should_break(0xB7F8, 0x08, /*is_write=*/true,
                                           /*old_val=*/0x09))
      << "an armed watchpoint's own hit was refused — the pre-fix filter "
         "returned watchpoints.empty()";
}

// A hit outside every armed range is not ours: resume it (the step machinery
// arms the probe without a host list, and an empty list must keep breaking).
TEST_F(ProbeFilterTest, WatchFilterHonoursAddressRangeAndKind) {
  z80_add_watchpoint(0x4000, 2, WatchpointType::WRITE);
  EXPECT_TRUE(z80_probe_watch_should_break(0x4001, 0xAA, true, 0x00))
      << "inside [addr, addr+len)";
  EXPECT_FALSE(z80_probe_watch_should_break(0x4002, 0xAA, true, 0x00))
      << "one past the range";
  EXPECT_FALSE(z80_probe_watch_should_break(0x4000, 0xAA, false, 0x00))
      << "a READ hit on a WRITE-only watchpoint";
  EXPECT_FALSE(z80_probe_watch_should_break(0x9000, 0xAA, true, 0x00))
      << "unrelated address with a non-empty list";
}

TEST_F(ProbeFilterTest, EmptyWatchListKeepsBreaking) {
  // Step-machinery parity: probe armed with no host list → break.
  EXPECT_TRUE(z80_probe_watch_should_break(0x1234, 0x00, true, 0x00));
}

// Conditions on watchpoints must be consulted — with the hit's own values.
TEST_F(ProbeFilterTest, WatchConditionsSeeTheHitValues) {
  z80_add_watchpoint_cond(0xB727, 1, WatchpointType::WRITE,
                          parse("value == 0x20"), "value == 0x20", 0);
  EXPECT_TRUE(z80_probe_watch_should_break(0xB727, 0x20, true, 0x00))
      << "condition true on the hit's value";
  EXPECT_FALSE(z80_probe_watch_should_break(0xB727, 0x21, true, 0x00))
      << "condition false on a different value";
}

// The identity lie: at post-filter time the view's PC is mid-fetch (past the
// opcode). `if pc == <bp addr>` must still be true at its own breakpoint.
TEST_F(ProbeFilterTest, ExecConditionSeesTheHitAddressAsPc) {
  z80.PC.w.l = 0x1BDA;  // what the parked machine's view publishes: mid-fetch
  z80_add_breakpoint_cond(0x1BD9, parse("pc == 0x1BD9"), "pc == 0x1BD9", 0);
  EXPECT_TRUE(z80_probe_exec_should_break(0x1BD9))
      << "the condition evaluated a stale mid-fetch PC instead of the hit "
         "identity";
  EXPECT_EQ(z80.PC.w.l, 0x1BD9)
      << "on a real break the hit identity stays published for the ack path";
}

// A refused condition must not leave the host view parked at the hit address.
TEST_F(ProbeFilterTest, FilteredExecResumeRestoresMidFetchPc) {
  z80.PC.w.l = 0x1BDA;
  z80_add_breakpoint_cond(0x1BD9, parse("pc != 0x1BD9"), "pc != 0x1BD9", 0);
  EXPECT_FALSE(z80_probe_exec_should_break(0x1BD9));
  EXPECT_EQ(z80.PC.w.l, 0x1BDA)
      << "filter-false must restore the mid-fetch PC the machine parked";
}

// Pass counts gate through the shared fire predicates: a pass_count of 2
// means the first latched hit resumes and the second pauses — for both
// breakpoints and watchpoints (review finding #8).
TEST_F(ProbeFilterTest, ExecPassCountFiresOnTheNthHit) {
  z80.PC.w.l = 0x1BDA;
  z80_add_breakpoint_cond(0x1BD9, nullptr, "", /*pass_count=*/2);
  EXPECT_FALSE(z80_probe_exec_should_break(0x1BD9)) << "first hit resumes";
  EXPECT_EQ(z80.PC.w.l, 0x1BDA) << "and restores the mid-fetch view";
  EXPECT_TRUE(z80_probe_exec_should_break(0x1BD9)) << "second hit pauses";
}

TEST_F(ProbeFilterTest, WatchPassCountFiresOnTheNthHit) {
  z80_add_watchpoint_cond(0xB7F8, 1, WatchpointType::WRITE, nullptr, "",
                          /*pass_count=*/2);
  EXPECT_FALSE(z80_probe_watch_should_break(0xB7F8, 0x08, true, 0x09))
      << "first hit resumes";
  EXPECT_TRUE(z80_probe_watch_should_break(0xB7F8, 0x08, true, 0x09))
      << "second hit pauses";
}

// Several armed watchpoints judge a hit independently: the matching one
// breaks, an unrelated hit resumes, and overlapping ranges discriminate by
// their own conditions (review finding #8).
TEST_F(ProbeFilterTest, MultipleWatchpointsJudgeIndependently) {
  z80_add_watchpoint(0x4000, 1, WatchpointType::WRITE);
  z80_add_watchpoint(0x8000, 1, WatchpointType::WRITE);
  EXPECT_TRUE(z80_probe_watch_should_break(0x8000, 0x01, true, 0x00));
  EXPECT_TRUE(z80_probe_watch_should_break(0x4000, 0x01, true, 0x00));
  EXPECT_FALSE(z80_probe_watch_should_break(0x6000, 0x01, true, 0x00))
      << "a hit between two armed ranges is nobody's";
}

TEST_F(ProbeFilterTest, OverlappingRangesDiscriminateByCondition) {
  z80_add_watchpoint_cond(0x4000, 2, WatchpointType::WRITE,
                          parse("value == 0xAA"), "value == 0xAA", 0);
  z80_add_watchpoint_cond(0x4001, 2, WatchpointType::WRITE,
                          parse("value == 0xBB"), "value == 0xBB", 0);
  EXPECT_TRUE(z80_probe_watch_should_break(0x4001, 0xAA, true, 0x00))
      << "the first range's condition claims it";
  EXPECT_TRUE(z80_probe_watch_should_break(0x4001, 0xBB, true, 0x00))
      << "the second range's condition claims it";
  EXPECT_FALSE(z80_probe_watch_should_break(0x4001, 0xCC, true, 0x00))
      << "no condition claims it";
}

// Flag conditions read the parked flags — the original beads-tib2 repro.
TEST_F(ProbeFilterTest, ExecConditionSeesTheParkedFlags) {
  z80.AF.b.l = 0x29;  // carry set (the measured hit-time F at 1BD9)
  z80.PC.w.l = 0x1BDA;
  z80_add_breakpoint_cond(0x1BD9, parse("carry"), "carry", 0);
  EXPECT_TRUE(z80_probe_exec_should_break(0x1BD9));
  z80.AF.b.l = 0x28;  // carry clear
  EXPECT_FALSE(z80_probe_exec_should_break(0x1BD9));
}
