#include <gtest/gtest.h>
#include "z80_assembler.h"
#include "z80_opcode_table.h"
#include "koncepcja.h"

extern byte *membank_read[4], *membank_write[4];
extern t_CPC CPC;

namespace {

// ── Expression evaluator tests ──

class ExprEvalTest : public ::testing::Test {
protected:
    std::map<std::string, word> symbols;
    void SetUp() override {
        z80_opcode_table_init();
        symbols["LABEL1"] = 0x4000;
        symbols["FOO"] = 42;
    }

    int32_t eval(const std::string& expr, word addr = 0) {
        int32_t result;
        std::string error;
        bool ok = Z80Assembler::eval_expr(expr, symbols, addr, result, error);
        EXPECT_TRUE(ok) << "eval_expr failed for '" << expr << "': " << error;
        return result;
    }

    void expect_fail(const std::string& expr) {
        int32_t result;
        std::string error;
        bool ok = Z80Assembler::eval_expr(expr, symbols, 0, result, error);
        EXPECT_FALSE(ok) << "expected failure for '" << expr << "' but got " << result;
    }
};

TEST_F(ExprEvalTest, DecimalLiterals) {
    EXPECT_EQ(eval("0"), 0);
    EXPECT_EQ(eval("42"), 42);
    EXPECT_EQ(eval("255"), 255);
    EXPECT_EQ(eval("65535"), 65535);
}

TEST_F(ExprEvalTest, HexLiterals) {
    EXPECT_EQ(eval("#FF"), 0xFF);
    EXPECT_EQ(eval("$FF"), 0xFF);
    EXPECT_EQ(eval("&FF"), 0xFF);
    EXPECT_EQ(eval("0xFF"), 0xFF);
    EXPECT_EQ(eval("0x4000"), 0x4000);
    EXPECT_EQ(eval("#C000"), 0xC000);
}

TEST_F(ExprEvalTest, HexSuffix) {
    EXPECT_EQ(eval("0FFh"), 0xFF);
    EXPECT_EQ(eval("38h"), 0x38);
}

TEST_F(ExprEvalTest, BinaryLiterals) {
    EXPECT_EQ(eval("%10110011"), 0xB3);
    EXPECT_EQ(eval("%11111111"), 0xFF);
    EXPECT_EQ(eval("%00000000"), 0);
}

TEST_F(ExprEvalTest, CharLiterals) {
    EXPECT_EQ(eval("'A'"), 65);
    EXPECT_EQ(eval("'Z'"), 90);
    EXPECT_EQ(eval("' '"), 32);
}

TEST_F(ExprEvalTest, CurrentAddress) {
    EXPECT_EQ(eval("$", 0x4000), 0x4000);
    EXPECT_EQ(eval("$+2", 0x1000), 0x1002);
}

TEST_F(ExprEvalTest, Symbols) {
    EXPECT_EQ(eval("LABEL1"), 0x4000);
    EXPECT_EQ(eval("label1"), 0x4000);  // case insensitive
    EXPECT_EQ(eval("FOO"), 42);
}

TEST_F(ExprEvalTest, BasicArithmetic) {
    EXPECT_EQ(eval("1+2"), 3);
    EXPECT_EQ(eval("10-3"), 7);
    EXPECT_EQ(eval("4*5"), 20);
    EXPECT_EQ(eval("20/4"), 5);
}

TEST_F(ExprEvalTest, LeftToRightEval) {
    // Maxam evaluates left-to-right: 3+2*4 = (3+2)*4 = 20, NOT 3+(2*4)=11
    EXPECT_EQ(eval("3+2*4"), 20);
    EXPECT_EQ(eval("10-3+2"), 9);
}

TEST_F(ExprEvalTest, Parentheses) {
    EXPECT_EQ(eval("(3+2)*4"), 20);
    EXPECT_EQ(eval("3+(2*4)"), 11);
}

TEST_F(ExprEvalTest, BitwiseOps) {
    EXPECT_EQ(eval("0xFF&0x0F"), 0x0F);
    EXPECT_EQ(eval("0xF0|0x0F"), 0xFF);
    EXPECT_EQ(eval("0xFF^0x0F"), 0xF0);
    EXPECT_EQ(eval("1<<4"), 16);
    EXPECT_EQ(eval("256>>4"), 16);
}

TEST_F(ExprEvalTest, UnaryMinus) {
    EXPECT_EQ(eval("-1"), -1);
    EXPECT_EQ(eval("-5+10"), 5);
}

TEST_F(ExprEvalTest, BitwiseNot) {
    // ~0 = -1 (all bits set in 32-bit)
    EXPECT_EQ(eval("~0"), -1);
    EXPECT_EQ(eval("~0xFF"), ~0xFF);
}

TEST_F(ExprEvalTest, Errors) {
    expect_fail("");
    expect_fail("UNDEFINED_SYMBOL");
}

TEST_F(ExprEvalTest, DivisionByZero) {
    expect_fail("1/0");
    expect_fail("1%0");
}

// ── Assembler tests ──

class Z80AssemblerTest : public ::testing::Test {
protected:
    byte ram[65536];
    Z80Assembler asm_;

    void SetUp() override {
        z80_opcode_table_init();
        memset(ram, 0, sizeof(ram));
        // Point memory banks to our test RAM
        for (int i = 0; i < 4; i++) {
            membank_read[i] = ram + (i * 0x4000);
            membank_write[i] = ram + (i * 0x4000);
        }
    }

    AsmResult assemble(const std::string& source) {
        return asm_.assemble(source);
    }

    AsmResult check(const std::string& source) {
        return asm_.check(source);
    }

    void expect_bytes(word addr, std::initializer_list<byte> expected) {
        int i = 0;
        for (byte b : expected) {
            EXPECT_EQ(ram[addr + i], b)
                << "byte mismatch at $" << std::hex << (addr + i)
                << ": expected $" << (int)b << " got $" << (int)ram[addr + i];
            i++;
        }
    }
};

TEST_F(Z80AssemblerTest, EmptySource) {
    auto r = assemble("");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.bytes_written, 0);
}

TEST_F(Z80AssemblerTest, NOP) {
    auto r = assemble("org &4000\nnop");
    EXPECT_TRUE(r.success) << r.errors[0].message;
    EXPECT_EQ(r.bytes_written, 1);
    expect_bytes(0x4000, {0x00});
}

TEST_F(Z80AssemblerTest, SimpleInstructions) {
    auto r = assemble(
        "org &4000\n"
        "nop\n"
        "halt\n"
        "ret\n"
    );
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.bytes_written, 3);
    expect_bytes(0x4000, {0x00, 0x76, 0xC9});
}

TEST_F(Z80AssemblerTest, LdImmediate8) {
    auto r = assemble("org &4000\nld a,&42\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0x3E, 0x42});
}

TEST_F(Z80AssemblerTest, LdImmediate16) {
    auto r = assemble("org &4000\nld bc,&1234\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0x01, 0x34, 0x12}); // little-endian
}

TEST_F(Z80AssemblerTest, LdRegReg) {
    auto r = assemble("org &4000\nld a,b\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0x78});
}

TEST_F(Z80AssemblerTest, JpAbsolute) {
    auto r = assemble("org &4000\njp &C000\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xC3, 0x00, 0xC0});
}

TEST_F(Z80AssemblerTest, JrRelativeForward) {
    auto r = assemble(
        "org &4000\n"
        "jr target\n"
        "nop\n"
        "nop\n"
        "target:\n"
        "ret\n"
    );
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    // JR at $4000, target at $4004, offset = $4004 - ($4000 + 2) = 2
    expect_bytes(0x4000, {0x18, 0x02, 0x00, 0x00, 0xC9});
}

TEST_F(Z80AssemblerTest, JrRelativeBackward) {
    auto r = assemble(
        "org &4000\n"
        "loop:\n"
        "nop\n"
        "jr loop\n"
    );
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    // JR at $4001, target at $4000, offset = $4000 - ($4001 + 2) = -3
    expect_bytes(0x4000, {0x00, 0x18, 0xFD}); // FD = -3 signed
}

TEST_F(Z80AssemblerTest, CallAbsolute) {
    auto r = assemble("org &4000\ncall &BB5A\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xCD, 0x5A, 0xBB});
}

TEST_F(Z80AssemblerTest, CBprefix) {
    auto r = assemble("org &4000\nrlc b\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xCB, 0x00});
}

TEST_F(Z80AssemblerTest, BitInstruction) {
    auto r = assemble("org &4000\nbit 7,a\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xCB, 0x7F});
}

TEST_F(Z80AssemblerTest, EDprefix) {
    auto r = assemble("org &4000\nldir\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xED, 0xB0});
}

TEST_F(Z80AssemblerTest, DDprefix) {
    auto r = assemble("org &4000\nld ix,&1234\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xDD, 0x21, 0x34, 0x12});
}

TEST_F(Z80AssemblerTest, IndexedAddress) {
    auto r = assemble("org &4000\nld a,(ix+5)\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xDD, 0x7E, 0x05});
}

TEST_F(Z80AssemblerTest, DDCBprefix) {
    auto r = assemble("org &4000\nrlc (ix+3)\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    // DD CB 03 06
    expect_bytes(0x4000, {0xDD, 0xCB, 0x03, 0x06});
}

TEST_F(Z80AssemblerTest, PushPop) {
    auto r = assemble("org &4000\npush af\npop bc\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xF5, 0xC1});
}

TEST_F(Z80AssemblerTest, RstInstruction) {
    auto r = assemble("org &4000\nrst 38h\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xFF});
}

TEST_F(Z80AssemblerTest, InOut) {
    auto r = assemble("org &4000\nin a,(&FE)\nout (&FE),a\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xDB, 0xFE, 0xD3, 0xFE});
}

// ── Directive tests ──

TEST_F(Z80AssemblerTest, DefbBytes) {
    auto r = assemble("org &4000\ndefb 1,2,3\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0x01, 0x02, 0x03});
}

TEST_F(Z80AssemblerTest, DefbString) {
    auto r = assemble("org &4000\ndefb \"AB\",0\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0x41, 0x42, 0x00});
}

TEST_F(Z80AssemblerTest, DefwWords) {
    auto r = assemble("org &4000\ndefw &1234,&5678\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0x34, 0x12, 0x78, 0x56}); // little-endian
}

TEST_F(Z80AssemblerTest, DefsReserve) {
    auto r = assemble("org &4000\ndefs 4,&FF\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xFF, 0xFF, 0xFF, 0xFF});
}

TEST_F(Z80AssemblerTest, EquDirective) {
    auto r = assemble(
        "txt_output equ &BB5A\n"
        "org &4000\n"
        "call txt_output\n"
    );
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xCD, 0x5A, 0xBB});
    EXPECT_EQ(r.symbols["TXT_OUTPUT"], 0xBB5A);
}

TEST_F(Z80AssemblerTest, EndDirective) {
    auto r = assemble(
        "org &4000\n"
        "nop\n"
        "end\n"
        "halt\n"  // should not be assembled
    );
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.bytes_written, 1);
}

// ── Label tests ──

TEST_F(Z80AssemblerTest, ForwardReference) {
    auto r = assemble(
        "org &4000\n"
        "jp target\n"
        "target:\n"
        "ret\n"
    );
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xC3, 0x03, 0x40, 0xC9});
}

TEST_F(Z80AssemblerTest, LabelWithColon) {
    auto r = assemble(
        "org &4000\n"
        "start: nop\n"
        "ret\n"
    );
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.symbols["START"], 0x4000);
}

TEST_F(Z80AssemblerTest, SymbolsExported) {
    auto r = assemble(
        "org &4000\n"
        "start:\n"
        "nop\n"
        "middle:\n"
        "ret\n"
    );
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.symbols["START"], 0x4000);
    EXPECT_EQ(r.symbols["MIDDLE"], 0x4001);
}

// ── Comment & multi-statement tests ──

TEST_F(Z80AssemblerTest, Comments) {
    auto r = assemble(
        "org &4000\n"
        "; this is a comment\n"
        "nop ; inline comment\n"
    );
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.bytes_written, 1);
}

// ── Error tests ──

TEST_F(Z80AssemblerTest, UnknownInstruction) {
    // A standalone unknown word is treated as a label in Maxam-style assemblers.
    // But an unknown word used as a mnemonic (followed by operands) should fail.
    auto r = assemble("org &4000\nfoobar a,b\n");
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.errors.empty());
}

TEST_F(Z80AssemblerTest, JrOutOfRange) {
    // JR can only jump -128..+127 bytes. Create a gap > 127 bytes.
    std::string source = "org &4000\njr target\n";
    for (int i = 0; i < 130; i++) source += "nop\n";
    source += "target: ret\n";
    auto r = assemble(source);
    EXPECT_FALSE(r.success);
}

// ── Check (dry run) test ──

TEST_F(Z80AssemblerTest, CheckDoesNotWriteMemory) {
    memset(ram, 0xAA, sizeof(ram)); // fill with sentinel
    auto r = check("org &4000\nnop\nret\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.bytes_written, 2);
    // Memory should still be 0xAA (not written)
    EXPECT_EQ(ram[0x4000], 0xAA);
    EXPECT_EQ(ram[0x4001], 0xAA);
}

// ── Full program test ──

TEST_F(Z80AssemblerTest, FullProgram) {
    auto r = assemble(
        "        org &4000\n"
        "start:  ld a,&41        ; 'A'\n"
        "        call &BB5A      ; TXT_OUTPUT\n"
        "        jr start\n"
    );
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.bytes_written, 7);  // 2 + 3 + 2 = 7
    expect_bytes(0x4000, {
        0x3E, 0x41,             // LD A,&41
        0xCD, 0x5A, 0xBB,      // CALL &BB5A
        0x18, 0xF9              // JR -7 (back to &4000)
    });
    EXPECT_EQ(r.symbols["START"], 0x4000);
}

TEST_F(Z80AssemblerTest, DbAlternativeMnemonics) {
    auto r = assemble("org &4000\ndb 1,2,3\nbyte 4,5\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {1, 2, 3, 4, 5});
}

TEST_F(Z80AssemblerTest, DwAlternativeMnemonics) {
    auto r = assemble("org &4000\ndw &1234\nword &5678\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0x34, 0x12, 0x78, 0x56});
}

TEST_F(Z80AssemblerTest, IndirectAddress) {
    auto r = assemble("org &4000\nld (&C000),a\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0x32, 0x00, 0xC0});
}

TEST_F(Z80AssemblerTest, ConditionalJp) {
    auto r = assemble("org &4000\njp nz,&C000\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xC2, 0x00, 0xC0});
}

TEST_F(Z80AssemblerTest, ConditionalCall) {
    auto r = assemble("org &4000\ncall z,&BB06\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xCC, 0x06, 0xBB});
}

TEST_F(Z80AssemblerTest, ConditionalRet) {
    auto r = assemble("org &4000\nret nz\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0xC0});
}

TEST_F(Z80AssemblerTest, AddSubWithA) {
    auto r = assemble("org &4000\nadd a,b\nsub c\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0x80, 0x91});
}

TEST_F(Z80AssemblerTest, ExchangeInstructions) {
    auto r = assemble("org &4000\nex af,af'\nexx\nex de,hl\n");
    EXPECT_TRUE(r.success) << (r.errors.empty() ? "" : r.errors[0].message);
    expect_bytes(0x4000, {0x08, 0xD9, 0xEB});
}

} // namespace
