// The sim rigs' shared argument parsing (sim/rig_args.h): a numeric option
// value is either a number in range or a hard error -- never a silent 0, which
// would let a rig run nothing and still exit 0.

#include "../sim/rig_args.h"

#include <gtest/gtest.h>

TEST(RigArgs, ParsesDecimalHexAndRangeEnds) {
  unsigned long long v = 0;
  EXPECT_TRUE(rig::parse_u64("42", 0, 100, v));
  EXPECT_EQ(42u, v);
  EXPECT_TRUE(rig::parse_u64("0x10", 0, 100, v));
  EXPECT_EQ(16u, v);
  EXPECT_TRUE(rig::parse_u64("0", 0, 100, v));
  EXPECT_EQ(0u, v);
  EXPECT_TRUE(rig::parse_u64("100", 0, 100, v));
  EXPECT_EQ(100u, v);
}

TEST(RigArgs, RejectsGarbageInsteadOfYieldingZero) {
  unsigned long long v = 12345;
  EXPECT_FALSE(rig::parse_u64("abc", 1, 100, v));
  EXPECT_FALSE(rig::parse_u64("20k", 1, 100, v));
  EXPECT_FALSE(rig::parse_u64("12.5", 1, 100, v));
  EXPECT_FALSE(rig::parse_u64("", 1, 100, v));
  EXPECT_FALSE(rig::parse_u64(nullptr, 1, 100, v));
  EXPECT_FALSE(rig::parse_u64("-1", 1, 100, v));
  EXPECT_EQ(12345u, v) << "a rejected value must not be written out";
}

TEST(RigArgs, RejectsOutOfRangeAndOverflow) {
  unsigned long long v = 0;
  EXPECT_FALSE(rig::parse_u64("0", 1, 100, v));
  EXPECT_FALSE(rig::parse_u64("101", 1, 100, v));
  EXPECT_FALSE(rig::parse_u64("99999999999999999999999", 1, ~0ULL, v));
}

TEST(RigArgs, BadArgReportsTheRigsBadArgumentStatus) {
  EXPECT_EQ(2, rig::bad_arg("psg_oracle_rig", "--us", "20k"));
  EXPECT_EQ(2, rig::bad_arg("cpct_tap_rig", "--cycles", nullptr));
}
