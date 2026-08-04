// konCePCja — the debugger's condition language must not lie.
//
// WHY THIS FILE EXISTS
//
// beads-tib2: `bp add 0x1BD9 if carry` armed cleanly and never fired, at a
// moment when stepping proved carry set — because `carry` was an unknown
// identifier and unknown identifiers silently evaluated to 0. A debugger
// that accepts a condition it cannot evaluate, and then judges it constant
// false, poisons investigations with false negatives. These tests pin the
// three language-level repairs: flag names exist, unknown names are a parse
// error, and `&`/`%` after a complete operand mean AND/MOD (not the CPC hex
// and binary literal prefixes they mean in value position).

#include "expr_parser.h"

#include <gtest/gtest.h>

#include "z80_view.h"

namespace {

int32_t eval_str(const std::string& s, t_z80regs& regs, bool* ok = nullptr) {
  std::string err;
  auto ast = expr_parse(s, err);
  if (ok != nullptr) *ok = ast != nullptr;
  if (!ast) return 0;
  ExprContext ctx;
  ctx.z80 = &regs;
  return expr_eval(ast.get(), ctx);
}

}  // namespace

// The flags, by name. F bit layout: C=0, N=1, P/V=2, H=4, Z=6, S=7.
TEST(ExprParser, FlagNamesReadTheFRegister) {
  t_z80regs regs{};
  regs.AF.b.l = 0x01;  // carry only
  EXPECT_NE(eval_str("carry", regs), 0);
  EXPECT_EQ(eval_str("zero", regs), 0);

  regs.AF.b.l = 0x40;  // zero only
  EXPECT_EQ(eval_str("carry", regs), 0);
  EXPECT_NE(eval_str("zero", regs), 0);

  regs.AF.b.l = 0x80;  // sign only
  EXPECT_NE(eval_str("sign", regs), 0);
  EXPECT_EQ(eval_str("carry", regs), 0);

  regs.AF.b.l = 0x04;  // parity/overflow
  EXPECT_NE(eval_str("parity", regs), 0);
  EXPECT_NE(eval_str("overflow", regs), 0);

  regs.AF.b.l = 0x10;  // half-carry
  EXPECT_NE(eval_str("halfcarry", regs), 0);
}

// The lie itself: an unknown identifier must be a PARSE ERROR, not a silent
// constant 0. `bp add ... if carry` on the pre-fix tree armed OK and never
// fired; `bp add ... if typo` must be refused loudly.
TEST(ExprParser, UnknownIdentifiersAreParseErrors) {
  std::string err;
  EXPECT_EQ(expr_parse("carrry", err), nullptr) << "typo accepted silently";
  EXPECT_NE(err.find("carrry"), std::string::npos)
      << "the error must name the offender: " << err;
  EXPECT_EQ(expr_parse("pc == bogus_name", err), nullptr);
}

// `f & 1` was rejected ("unexpected token after expression") because `&`
// only ever lexed as the CPC hex prefix. After a complete operand it must be
// the AND operator; in value position it stays the hex prefix.
TEST(ExprParser, AmpersandIsAndAfterAnOperandAndHexPrefixBefore) {
  t_z80regs regs{};
  regs.AF.b.l = 0x41;  // Z + C
  EXPECT_EQ(eval_str("f & 1", regs), 1) << "AND after an operand";
  EXPECT_EQ(eval_str("f & &41", regs), 0x41) << "prefix in value position";
  regs.HL.w.l = 0x4000;
  EXPECT_NE(eval_str("hl == &4000", regs), 0) << "bare CPC hex still parses";
}

TEST(ExprParser, PercentIsModAfterAnOperandAndBinaryPrefixBefore) {
  t_z80regs regs{};
  regs.AF.b.h = 7;
  EXPECT_EQ(eval_str("a % 2", regs), 1) << "MOD after an operand";
  EXPECT_EQ(eval_str("%101", regs), 5) << "binary literal in value position";
}

// Regression guards for what already worked — the fix must not break the
// existing dialect.
TEST(ExprParser, ExistingDialectStillParses) {
  t_z80regs regs{};
  regs.PC.w.l = 0x1BD9;
  regs.HL.w.l = 0x0100;
  EXPECT_NE(eval_str("pc == 0x1BD9", regs), 0);
  EXPECT_NE(eval_str("hl < 0xFFFF", regs), 0);
  EXPECT_EQ(eval_str("0", regs), 0);
  EXPECT_NE(eval_str("1", regs), 0);
  EXPECT_NE(eval_str("pc >= $1BD9", regs), 0) << "$ hex prefix";
  bool ok = false;
  eval_str("byte(pc)", regs, &ok);
  EXPECT_TRUE(ok) << "function calls";
}
