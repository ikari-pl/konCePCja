#include <gtest/gtest.h>
#include "data_areas.h"
#include "symfile.h"
#include <sstream>
#include <cstring>

// Test that format_at produces valid assembler source directives
// (the disasm export IPC command uses format_at for data areas)

namespace {

class DisasmExportDataTest : public ::testing::Test {
protected:
    void SetUp() override {
        mgr_.clear_all();
        memset(mem_, 0, sizeof(mem_));
    }
    DataAreaManager mgr_;
    uint8_t mem_[0x10000];
};

TEST_F(DisasmExportDataTest, BytesDataAreaFormatsAsDb) {
    mgr_.mark(0x4000, 0x4003, DataType::BYTES, "sprite_data");
    mem_[0x4000] = 0xAA;
    mem_[0x4001] = 0xBB;
    mem_[0x4002] = 0xCC;
    mem_[0x4003] = 0xDD;
    int consumed = 0;
    std::string result = mgr_.format_at(0x4000, mem_, sizeof(mem_), &consumed);
    EXPECT_EQ(consumed, 4);
    EXPECT_EQ(result, "db $AA,$BB,$CC,$DD");
}

TEST_F(DisasmExportDataTest, WordsDataAreaFormatsAsDw) {
    mgr_.mark(0x5000, 0x5003, DataType::WORDS);
    mem_[0x5000] = 0x34;
    mem_[0x5001] = 0x12;
    mem_[0x5002] = 0x78;
    mem_[0x5003] = 0x56;
    int consumed = 0;
    std::string result = mgr_.format_at(0x5000, mem_, sizeof(mem_), &consumed);
    EXPECT_EQ(consumed, 4);
    EXPECT_EQ(result, "dw $1234,$5678");
}

TEST_F(DisasmExportDataTest, TextDataAreaFormatsAsDbWithQuotes) {
    mgr_.mark(0x6000, 0x6004, DataType::TEXT);
    mem_[0x6000] = 'H';
    mem_[0x6001] = 'e';
    mem_[0x6002] = 'l';
    mem_[0x6003] = 'l';
    mem_[0x6004] = 'o';
    int consumed = 0;
    std::string result = mgr_.format_at(0x6000, mem_, sizeof(mem_), &consumed);
    EXPECT_EQ(consumed, 5);
    EXPECT_EQ(result, "db \"Hello\"");
}

TEST_F(DisasmExportDataTest, TextWithNonPrintableFormatsAsMixed) {
    mgr_.mark(0x7000, 0x7003, DataType::TEXT);
    mem_[0x7000] = 'A';
    mem_[0x7001] = 'B';
    mem_[0x7002] = 0x00;  // null terminator
    mem_[0x7003] = 'C';
    int consumed = 0;
    std::string result = mgr_.format_at(0x7000, mem_, sizeof(mem_), &consumed);
    EXPECT_EQ(consumed, 4);
    // "AB" followed by $00 then "C"
    EXPECT_EQ(result, "db \"AB\",$00,\"C\"");
}

TEST_F(DisasmExportDataTest, FormatAtReturnsEmptyForNonDataArea) {
    int consumed = 0;
    std::string result = mgr_.format_at(0x8000, mem_, sizeof(mem_), &consumed);
    EXPECT_EQ(consumed, 0);
    EXPECT_TRUE(result.empty());
}

TEST_F(DisasmExportDataTest, BytesAreaPartialFormat) {
    // Mark a 16-byte area; format_at should emit up to 8 bytes per line
    mgr_.mark(0x4000, 0x400F, DataType::BYTES);
    for (int i = 0; i < 16; i++) mem_[0x4000 + i] = static_cast<uint8_t>(i);

    int consumed1 = 0;
    std::string line1 = mgr_.format_at(0x4000, mem_, sizeof(mem_), &consumed1);
    EXPECT_EQ(consumed1, 8);
    EXPECT_EQ(line1, "db $00,$01,$02,$03,$04,$05,$06,$07");

    // Second call at 0x4008 should produce next 8 bytes
    int consumed2 = 0;
    std::string line2 = mgr_.format_at(0x4008, mem_, sizeof(mem_), &consumed2);
    EXPECT_EQ(consumed2, 8);
    EXPECT_EQ(line2, "db $08,$09,$0A,$0B,$0C,$0D,$0E,$0F");
}

// Test that Symfile provides labels for the export
TEST_F(DisasmExportDataTest, SymfileLookupForLabels) {
    Symfile sym;
    sym.addSymbol(0x4000, "game_start");
    sym.addSymbol(0x4100, "main_loop");

    EXPECT_EQ(sym.lookupAddr(0x4000), "game_start");
    EXPECT_EQ(sym.lookupAddr(0x4100), "main_loop");
    EXPECT_EQ(sym.lookupAddr(0x4050), "");
}

TEST_F(DisasmExportDataTest, WordsAreaOddRemainder) {
    // Mark 3 bytes as WORDS — only 1 complete word fits
    mgr_.mark(0x5000, 0x5002, DataType::WORDS);
    mem_[0x5000] = 0x34;
    mem_[0x5001] = 0x12;
    mem_[0x5002] = 0xFF;
    int consumed = 0;
    std::string result = mgr_.format_at(0x5000, mem_, sizeof(mem_), &consumed);
    EXPECT_EQ(consumed, 2);  // one word = 2 bytes
    EXPECT_EQ(result, "dw $1234");
}

} // namespace
