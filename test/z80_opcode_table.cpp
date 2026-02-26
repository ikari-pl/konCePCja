#include <gtest/gtest.h>
#include "z80_opcode_table.h"
#include <set>
#include <fstream>

// Access the cc_* arrays from z80.cpp for cross-checking
// These are defined static in z80.cpp, so we need to replicate them here
// for the cross-check test. We verify against the known correct values.

namespace {

TEST(Z80OpcodeTable, EntryCount) {
    EXPECT_EQ(g_z80_opcode_count, 1268);
}

TEST(Z80OpcodeTable, Init) {
    z80_opcode_table_init();
    // Should be idempotent
    z80_opcode_table_init();
}

TEST(Z80OpcodeTable, DisasmLookupNONE) {
    z80_opcode_table_init();

    auto* nop = z80_opcode_lookup(OpcodePrefix::NONE, 0x00);
    ASSERT_NE(nop, nullptr);
    EXPECT_STREQ(nop->mnemonic, "nop");
    EXPECT_EQ(nop->length, 1);
    EXPECT_EQ(nop->t_states, 4);

    auto* halt = z80_opcode_lookup(OpcodePrefix::NONE, 0x76);
    ASSERT_NE(halt, nullptr);
    EXPECT_STREQ(halt->mnemonic, "halt");

    auto* ret = z80_opcode_lookup(OpcodePrefix::NONE, 0xC9);
    ASSERT_NE(ret, nullptr);
    EXPECT_STREQ(ret->mnemonic, "ret");
    EXPECT_EQ(ret->length, 1);
    EXPECT_EQ(ret->t_states, 12);

    auto* call = z80_opcode_lookup(OpcodePrefix::NONE, 0xCD);
    ASSERT_NE(call, nullptr);
    EXPECT_STREQ(call->mnemonic, "call **");
    EXPECT_EQ(call->length, 3);
    EXPECT_EQ(call->t_states, 20);
}

TEST(Z80OpcodeTable, DisasmLookupRelative) {
    z80_opcode_table_init();

    auto* jr = z80_opcode_lookup(OpcodePrefix::NONE, 0x18);
    ASSERT_NE(jr, nullptr);
    EXPECT_STREQ(jr->mnemonic, "jr *");
    EXPECT_TRUE(jr->is_relative);
    EXPECT_EQ(jr->length, 2);

    auto* djnz = z80_opcode_lookup(OpcodePrefix::NONE, 0x10);
    ASSERT_NE(djnz, nullptr);
    EXPECT_STREQ(djnz->mnemonic, "djnz *");
    EXPECT_TRUE(djnz->is_relative);

    auto* jr_nz = z80_opcode_lookup(OpcodePrefix::NONE, 0x20);
    ASSERT_NE(jr_nz, nullptr);
    EXPECT_TRUE(jr_nz->is_relative);
    EXPECT_EQ(jr_nz->t_states_extra, 4); // extra when taken
}

TEST(Z80OpcodeTable, DisasmLookupCB) {
    z80_opcode_table_init();

    auto* rlc_b = z80_opcode_lookup(OpcodePrefix::CB, 0x00);
    ASSERT_NE(rlc_b, nullptr);
    EXPECT_STREQ(rlc_b->mnemonic, "rlc b");
    EXPECT_EQ(rlc_b->length, 2);
    EXPECT_EQ(rlc_b->t_states, 8);  // 4 (CB prefix) + 4 (instruction)

    auto* bit_7_a = z80_opcode_lookup(OpcodePrefix::CB, 0x7F);
    ASSERT_NE(bit_7_a, nullptr);
    EXPECT_STREQ(bit_7_a->mnemonic, "bit 7,a");
}

TEST(Z80OpcodeTable, DisasmLookupED) {
    z80_opcode_table_init();

    auto* ldir = z80_opcode_lookup(OpcodePrefix::ED, 0xB0);
    ASSERT_NE(ldir, nullptr);
    EXPECT_STREQ(ldir->mnemonic, "ldir");
    EXPECT_EQ(ldir->length, 2);
    EXPECT_EQ(ldir->t_states, 16);
    EXPECT_EQ(ldir->t_states_extra, 4); // extra when BC != 0

    auto* neg = z80_opcode_lookup(OpcodePrefix::ED, 0x44);
    ASSERT_NE(neg, nullptr);
    EXPECT_STREQ(neg->mnemonic, "neg");
}

TEST(Z80OpcodeTable, DisasmLookupDD) {
    z80_opcode_table_init();

    auto* ld_ix = z80_opcode_lookup(OpcodePrefix::DD, 0x21);
    ASSERT_NE(ld_ix, nullptr);
    EXPECT_STREQ(ld_ix->mnemonic, "ld ix,**");
    EXPECT_EQ(ld_ix->length, 4);
    EXPECT_EQ(ld_ix->operand_bytes, 2);
}

TEST(Z80OpcodeTable, DisasmLookupDDCB) {
    z80_opcode_table_init();

    auto* rlc_ix = z80_opcode_lookup(OpcodePrefix::DDCB, 0x06);
    ASSERT_NE(rlc_ix, nullptr);
    EXPECT_STREQ(rlc_ix->mnemonic, "rlc (ix+*)");
    EXPECT_EQ(rlc_ix->length, 4);

    auto* bit_0_ix = z80_opcode_lookup(OpcodePrefix::DDCB, 0x46);
    ASSERT_NE(bit_0_ix, nullptr);
    EXPECT_STREQ(bit_0_ix->mnemonic, "bit 0,(ix+*)");
}

TEST(Z80OpcodeTable, DisasmLookupNotFound) {
    z80_opcode_table_init();

    // 0xCB prefix byte doesn't have an entry in NONE
    auto* no_entry = z80_opcode_lookup(OpcodePrefix::NONE, 0xCB);
    EXPECT_EQ(no_entry, nullptr);

    // ED prefix byte doesn't have entries below 0x40
    auto* no_ed = z80_opcode_lookup(OpcodePrefix::ED, 0x00);
    EXPECT_EQ(no_ed, nullptr);
}

TEST(Z80OpcodeTable, AsmLookupBasic) {
    z80_opcode_table_init();

    auto& nop = z80_asm_lookup("NOP");
    ASSERT_FALSE(nop.empty());
    EXPECT_STREQ(nop[0]->mnemonic, "nop");

    auto& ld_a_star = z80_asm_lookup("LD A,*");
    ASSERT_FALSE(ld_a_star.empty());
    EXPECT_EQ(ld_a_star[0]->opcode, 0x3E);

    auto& jp = z80_asm_lookup("JP **");
    ASSERT_FALSE(jp.empty());
    EXPECT_EQ(jp[0]->opcode, 0xC3);
}

TEST(Z80OpcodeTable, AsmLookupNotFound) {
    z80_opcode_table_init();
    auto& bad = z80_asm_lookup("NONEXISTENT");
    EXPECT_TRUE(bad.empty());
}

TEST(Z80OpcodeTable, LegacyKeyMapping) {
    z80_opcode_table_init();

    auto* nop = z80_opcode_lookup(OpcodePrefix::NONE, 0x00);
    EXPECT_EQ(z80_opcode_to_legacy_key(*nop), 0x00);

    auto* rlc_b = z80_opcode_lookup(OpcodePrefix::CB, 0x00);
    EXPECT_EQ(z80_opcode_to_legacy_key(*rlc_b), 0xCB00);

    auto* neg = z80_opcode_lookup(OpcodePrefix::ED, 0x44);
    EXPECT_EQ(z80_opcode_to_legacy_key(*neg), 0xED44);

    auto* ld_ix = z80_opcode_lookup(OpcodePrefix::DD, 0x21);
    EXPECT_EQ(z80_opcode_to_legacy_key(*ld_ix), 0xDD21);
}

TEST(Z80OpcodeTable, GeneratedTxtMatchesOriginal) {
    z80_opcode_table_init();

    // Read original file
    std::ifstream infile("resources/z80_opcodes.txt");
    if (!infile.good()) {
        GTEST_SKIP() << "resources/z80_opcodes.txt not found";
    }
    std::string original;
    std::string line;
    while (std::getline(infile, line)) {
        original += line + "\n";
    }

    // Generate from master table
    std::string generated = z80_opcode_table_to_txt();

    EXPECT_EQ(generated, original);
}

TEST(Z80OpcodeTable, NoDuplicateEntries) {
    z80_opcode_table_init();

    // Check that no (prefix, opcode) pair appears more than once
    std::set<int> seen;
    for (int i = 0; i < g_z80_opcode_count; i++) {
        const auto& op = g_z80_opcodes[i];
        int key = z80_opcode_to_legacy_key(op);
        EXPECT_EQ(seen.count(key), 0u) << "duplicate key 0x" << std::hex << key
                                        << " for " << op.mnemonic;
        seen.insert(key);
    }
}

TEST(Z80OpcodeTable, AllPrefixGroupsPresent) {
    z80_opcode_table_init();

    int counts[7] = {};
    for (int i = 0; i < g_z80_opcode_count; i++) {
        int idx = static_cast<int>(g_z80_opcodes[i].prefix);
        ASSERT_GE(idx, 0);
        ASSERT_LT(idx, 7);
        counts[idx]++;
    }

    EXPECT_EQ(counts[0], 252);  // NONE
    EXPECT_EQ(counts[1], 256);  // CB
    EXPECT_EQ(counts[2], 78);   // ED
    EXPECT_EQ(counts[3], 85);   // DD
    EXPECT_EQ(counts[4], 85);   // FD
    EXPECT_EQ(counts[5], 256);  // DDCB
    EXPECT_EQ(counts[6], 256);  // FDCB
}

TEST(Z80OpcodeTable, ConditionalInstructionExtraCycles) {
    z80_opcode_table_init();

    // RET NZ: base 8, extra 8 when taken
    auto* ret_nz = z80_opcode_lookup(OpcodePrefix::NONE, 0xC0);
    ASSERT_NE(ret_nz, nullptr);
    EXPECT_EQ(ret_nz->t_states, 8);
    EXPECT_EQ(ret_nz->t_states_extra, 8);

    // CALL NZ,**: base 12, extra 8 when taken
    auto* call_nz = z80_opcode_lookup(OpcodePrefix::NONE, 0xC4);
    ASSERT_NE(call_nz, nullptr);
    EXPECT_EQ(call_nz->t_states, 12);
    EXPECT_EQ(call_nz->t_states_extra, 8);

    // JR NZ,*: base 8, extra 4 when taken
    auto* jr_nz = z80_opcode_lookup(OpcodePrefix::NONE, 0x20);
    ASSERT_NE(jr_nz, nullptr);
    EXPECT_EQ(jr_nz->t_states, 8);
    EXPECT_EQ(jr_nz->t_states_extra, 4);

    // DJNZ: base 12, extra 4 when taken
    auto* djnz = z80_opcode_lookup(OpcodePrefix::NONE, 0x10);
    ASSERT_NE(djnz, nullptr);
    EXPECT_EQ(djnz->t_states, 12);
    EXPECT_EQ(djnz->t_states_extra, 4);
}

} // namespace
