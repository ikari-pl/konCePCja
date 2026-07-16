#include "z80_disassembly.h"

#include <gtest/gtest.h>

#include "koncepcja.h"

extern byte* membank_read[4];
extern t_CPC CPC;

namespace {

TEST(DisassembledLine, Comparisons) {
  DisassembledLine l1(1, 0, "nop");
  DisassembledLine l1_identical(1, 0, "nop");
  DisassembledLine l1bis(1, 0xc9, "ret");
  DisassembledLine l2(2, 0x1802, "jr 2", 4);
  DisassembledLine l3(3, 0xc9, "ret");

  EXPECT_TRUE(l1 < l2);
  EXPECT_TRUE(l2 < l3);
  EXPECT_TRUE(l1 < l3);
  EXPECT_FALSE(l1 < l1bis);
  EXPECT_FALSE(l1 == l1bis);
  EXPECT_TRUE(l1 == l1_identical);
}

TEST(Z80DisassemblyTest, DisassembleNOPRET) {
  byte membank0[2] = {0, 0xc9};

  CPC.resources_path = "resources";
  membank_read[0] = membank0;

  auto code = disassemble({0});

  DisassembledCode want;
  want.lines.emplace(0, 0, "nop");
  want.lines.emplace(1, 0xc9, "ret");

  EXPECT_EQ(want.lines, code.lines);
}

TEST(Z80DisassemblyTest, DisassembleDecALoop) {
  byte membank0[10] = {0x00, 0x00, 0x00, 0x3E, 0x10,
                       0x3D, 0xC2, 0x5,  0x0,  0xC9};

  CPC.resources_path = "resources";
  membank_read[0] = membank0;

  auto code = disassemble({3});

  DisassembledCode want;
  want.lines.emplace(3, 0x3E10, "ld a,$10");
  want.lines.emplace(5, 0x3D, "dec a");
  want.lines.emplace(6, 0xC20500, "jp nz,$0005");
  want.lines.emplace(9, 0xC9, "ret");

  EXPECT_EQ(want.lines, code.lines);
}

TEST(Z80DisassemblyTest, SqrtRoutine) {
  byte membank0[16] = {0x21, 0x64, 0x00, 0x11, 0x01, 0x00, 0xAF, 0x3D,
                       0xED, 0x52, 0x13, 0x13, 0x3C, 0x30, 0xF9, 0xC9};

  CPC.resources_path = "resources";
  membank_read[0] = membank0;

  auto code = disassemble({0});

  DisassembledCode want;
  want.lines.emplace(0, 0x216400, "ld hl,$0064");
  want.lines.emplace(3, 0x110100, "ld de,$0001");
  want.lines.emplace(6, 0xAF, "xor a");
  want.lines.emplace(7, 0x3D, "dec a");
  want.lines.emplace(8, 0xED52, "sbc hl,de");
  want.lines.emplace(10, 0x13, "inc de");
  want.lines.emplace(11, 0x13, "inc de");
  want.lines.emplace(12, 0x3C, "inc a");
  want.lines.emplace(13, 0x30F9, "jr nc,$f9  ; $0008");
  want.lines.emplace(15, 0xC9, "ret");

  EXPECT_EQ(want.lines, code.lines);
}

TEST(Z80DisassemblyTest, DisassembleOne) {
  byte membank0[16] = {0x21, 0x64, 0x00, 0x11, 0x01, 0x00, 0xAF, 0x3D,
                       0xED, 0x52, 0x13, 0x13, 0x3C, 0x30, 0xF9, 0xC9};

  CPC.resources_path = "resources";
  membank_read[0] = membank0;

  std::vector<dword> unused_entry_points;
  DisassembledCode unused_code;

  auto line = disassemble_one(10, unused_code, unused_entry_points);
  EXPECT_EQ(DisassembledLine(10, 0x13, "inc de"), line);

  line = disassemble_one(13, unused_code, unused_entry_points);
  EXPECT_EQ(DisassembledLine(13, 0x30F9, "jr nc,$f9  ; $0008"), line);
}

// Table-driven decode coverage: opcode bytes -> expected mnemonic text and
// instruction length. Exercises every prefix group (NONE, CB, ED, DD, FD,
// DDCB, FDCB), immediate byte/word operands, relative jumps, RST and the db
// fallback. DDCB/FDCB place the displacement byte *before* the opcode byte,
// which the previous byte-accumulator decoder mis-keyed.
struct DisasmCase {
  const char* name;
  std::vector<byte> bytes;
  const char* want_text;
  int want_length;
};

class Z80DisasmTableTest : public ::testing::TestWithParam<DisasmCase> {};

TEST_P(Z80DisasmTableTest, DecodesInstruction) {
  const DisasmCase& tc = GetParam();
  // Pad so decode_at can always read a full 4-byte window.
  byte membank0[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < tc.bytes.size() && i < sizeof(membank0); ++i)
    membank0[i] = tc.bytes[i];

  CPC.resources_path = "resources";
  membank_read[0] = membank0;

  std::vector<dword> eps;
  DisassembledCode code;
  auto line = disassemble_one(0, code, eps);

  EXPECT_EQ(std::string(tc.want_text), line.instruction_) << tc.name;
  EXPECT_EQ(tc.want_length, line.Size()) << tc.name << " (Size)";
  EXPECT_EQ(tc.want_length, z80_instruction_length(0)) << tc.name << " (len)";
}

INSTANTIATE_TEST_SUITE_P(
    AllPrefixGroups, Z80DisasmTableTest,
    ::testing::Values(
        // NONE prefix
        DisasmCase{"nop", {0x00}, "nop", 1},
        DisasmCase{"ld_a_imm", {0x3E, 0x42}, "ld a,$42", 2},
        DisasmCase{"ld_hl_imm16", {0x21, 0x34, 0x12}, "ld hl,$1234", 3},
        DisasmCase{"jp_abs", {0xC3, 0x00, 0xC0}, "jp $c000", 3},
        DisasmCase{"call_cc_abs", {0xCC, 0x05, 0x40}, "call z,$4005", 3},
        DisasmCase{"out_imm", {0xD3, 0x7F}, "out ($7f),a", 2},
        DisasmCase{"rst38", {0xFF}, "rst 38h", 1},
        // Relative jumps: raw displacement byte + absolute target comment.
        DisasmCase{"jr_fwd", {0x18, 0x05}, "jr $05  ; $0007", 2},
        DisasmCase{"jr_back", {0x18, 0xFE}, "jr $fe  ; $0000", 2},
        DisasmCase{"djnz", {0x10, 0x10}, "djnz $10  ; $0012", 2},
        DisasmCase{"jr_cc", {0x20, 0x02}, "jr nz,$02  ; $0004", 2},
        // CB prefix
        DisasmCase{"cb_bit", {0xCB, 0x7C}, "bit 7,h", 2},
        DisasmCase{"cb_rlc_hl", {0xCB, 0x06}, "rlc (hl)", 2},
        // ED prefix
        DisasmCase{"ed_sbc", {0xED, 0x52}, "sbc hl,de", 2},
        DisasmCase{"ed_ld_abs", {0xED, 0x43, 0x00, 0x80}, "ld ($8000),bc", 4},
        // DD / FD prefix with displacement and immediate
        DisasmCase{"dd_ld_a_ixd", {0xDD, 0x7E, 0x04}, "ld a,(ix+$04)", 3},
        DisasmCase{
            "dd_ld_ixd_imm", {0xDD, 0x36, 0x02, 0x99}, "ld (ix+$02),$99", 4},
        DisasmCase{"fd_ld_a_iyd", {0xFD, 0x7E, 0xFB}, "ld a,(iy+$fb)", 3},
        DisasmCase{"dd_jp_ix", {0xDD, 0xE9}, "jp (ix)", 2},
        // DDCB / FDCB: displacement precedes the opcode byte.
        DisasmCase{"ddcb_bit", {0xDD, 0xCB, 0x03, 0x46}, "bit 0,(ix+$03)", 4},
        DisasmCase{"ddcb_rlc", {0xDD, 0xCB, 0x7F, 0x06}, "rlc (ix+$7f)", 4},
        DisasmCase{"fdcb_set", {0xFD, 0xCB, 0x01, 0xC6}, "set 0,(iy+$01)", 4}),
    [](const ::testing::TestParamInfo<DisasmCase>& info) {
      return info.param.name;
    });

TEST(Z80DisassemblyTest, CallAndRstClassification) {
  byte membank0[8] = {0xCD, 0x00, 0xC0, 0xC3, 0x00, 0xC0, 0xFF, 0x00};
  CPC.resources_path = "resources";
  membank_read[0] = membank0;

  EXPECT_TRUE(z80_is_call_or_rst(0));   // call $c000
  EXPECT_FALSE(z80_is_call_or_rst(3));  // jp $c000
  EXPECT_TRUE(z80_is_call_or_rst(6));   // rst 38h
}

TEST(Z80DisassemblyTest, LineAt) {
  byte membank0[16] = {0x21, 0x64, 0x00, 0x11, 0x01, 0x00, 0xAF, 0x3D,
                       0xED, 0x52, 0x13, 0x13, 0x3C, 0x30, 0xF9, 0xC9};

  CPC.resources_path = "resources";
  membank_read[0] = membank0;

  auto code = disassemble({0});

  EXPECT_FALSE(code.LineAt(9).has_value());
  EXPECT_TRUE(code.LineAt(10).has_value());
  EXPECT_EQ(DisassembledLine(10, 0x13, "inc de"), code.LineAt(10).value());
}

}  // namespace
