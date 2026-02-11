#include <gtest/gtest.h>
#include "data_areas.h"

namespace {

class DataAreaManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        mgr_.clear_all();
    }
    DataAreaManager mgr_;
};

// --- find() tests ---

TEST_F(DataAreaManagerTest, FindReturnsAreaContainingAddress) {
    mgr_.mark(0x1000, 0x100F, DataType::BYTES, "table1");
    const DataArea* a = mgr_.find(0x1005);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->start, 0x1000);
    EXPECT_EQ(a->end, 0x100F);
    EXPECT_EQ(a->type, DataType::BYTES);
    EXPECT_EQ(a->label, "table1");
}

TEST_F(DataAreaManagerTest, FindReturnsNullptrForAddressOutside) {
    mgr_.mark(0x1000, 0x100F, DataType::BYTES);
    EXPECT_EQ(mgr_.find(0x0FFF), nullptr);
    EXPECT_EQ(mgr_.find(0x1010), nullptr);
}

TEST_F(DataAreaManagerTest, FindAtBoundaries) {
    mgr_.mark(0x2000, 0x2003, DataType::WORDS);
    EXPECT_NE(mgr_.find(0x2000), nullptr);
    EXPECT_NE(mgr_.find(0x2003), nullptr);
    EXPECT_EQ(mgr_.find(0x1FFF), nullptr);
    EXPECT_EQ(mgr_.find(0x2004), nullptr);
}

// --- Overlapping regions ---

TEST_F(DataAreaManagerTest, OverlappingRegionOverwritesFirst) {
    mgr_.mark(0x1000, 0x100F, DataType::BYTES, "first");
    mgr_.mark(0x1008, 0x101F, DataType::WORDS, "second");
    // First area should have been removed due to overlap
    const DataArea* a = mgr_.find(0x1000);
    // Address 0x1000 is no longer covered (old area removed, new starts at 0x1008)
    EXPECT_EQ(a, nullptr);
    // Address 0x1010 should be in the new area
    a = mgr_.find(0x1010);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->start, 0x1008);
    EXPECT_EQ(a->type, DataType::WORDS);
}

// --- clear() ---

TEST_F(DataAreaManagerTest, ClearRemovesSpecificArea) {
    mgr_.mark(0x1000, 0x100F, DataType::BYTES);
    mgr_.mark(0x2000, 0x200F, DataType::WORDS);
    mgr_.clear(0x1000);
    EXPECT_EQ(mgr_.find(0x1005), nullptr);
    EXPECT_NE(mgr_.find(0x2005), nullptr);
}

// --- clear_all() ---

TEST_F(DataAreaManagerTest, ClearAllRemovesEverything) {
    mgr_.mark(0x1000, 0x100F, DataType::BYTES);
    mgr_.mark(0x2000, 0x200F, DataType::WORDS);
    mgr_.mark(0x3000, 0x300F, DataType::TEXT);
    mgr_.clear_all();
    EXPECT_EQ(mgr_.find(0x1005), nullptr);
    EXPECT_EQ(mgr_.find(0x2005), nullptr);
    EXPECT_EQ(mgr_.find(0x3005), nullptr);
}

// --- list() ---

TEST_F(DataAreaManagerTest, ListReturnsSortedByStartAddress) {
    mgr_.mark(0x3000, 0x300F, DataType::TEXT);
    mgr_.mark(0x1000, 0x100F, DataType::BYTES);
    mgr_.mark(0x2000, 0x200F, DataType::WORDS);
    auto areas = mgr_.list();
    ASSERT_EQ(areas.size(), 3u);
    EXPECT_EQ(areas[0].start, 0x1000);
    EXPECT_EQ(areas[1].start, 0x2000);
    EXPECT_EQ(areas[2].start, 0x3000);
}

// --- format_at() BYTES ---

TEST_F(DataAreaManagerTest, FormatAtBytesProducesDbOutput) {
    mgr_.mark(0x0000, 0x0003, DataType::BYTES);
    uint8_t mem[] = {0x41, 0x42, 0x00, 0xFF};
    std::string result = mgr_.format_at(0x0000, mem, sizeof(mem));
    EXPECT_EQ(result, "db $41,$42,$00,$FF");
}

TEST_F(DataAreaManagerTest, FormatAtBytesMaxEightPerLine) {
    mgr_.mark(0x0000, 0x000F, DataType::BYTES);
    uint8_t mem[16];
    for (int i = 0; i < 16; i++) mem[i] = static_cast<uint8_t>(i);
    // At addr 0, should emit 8 bytes
    std::string result = mgr_.format_at(0x0000, mem, sizeof(mem));
    EXPECT_EQ(result, "db $00,$01,$02,$03,$04,$05,$06,$07");
    // At addr 8, should emit next 8
    result = mgr_.format_at(0x0008, mem, sizeof(mem));
    EXPECT_EQ(result, "db $08,$09,$0A,$0B,$0C,$0D,$0E,$0F");
}

// --- format_at() WORDS ---

TEST_F(DataAreaManagerTest, FormatAtWordsProducesDwOutput) {
    mgr_.mark(0x0000, 0x0003, DataType::WORDS);
    uint8_t mem[] = {0x34, 0x12, 0x78, 0x56};  // little-endian: $1234, $5678
    std::string result = mgr_.format_at(0x0000, mem, sizeof(mem));
    EXPECT_EQ(result, "dw $1234,$5678");
}

TEST_F(DataAreaManagerTest, FormatAtWordsMaxFourPerLine) {
    mgr_.mark(0x0000, 0x000F, DataType::WORDS);
    uint8_t mem[16];
    for (int i = 0; i < 16; i++) mem[i] = static_cast<uint8_t>(i);
    // At addr 0: 4 words (8 bytes)
    std::string result = mgr_.format_at(0x0000, mem, sizeof(mem));
    // Bytes: 00 01 02 03 04 05 06 07 -> words $0100,$0302,$0504,$0706
    EXPECT_EQ(result, "dw $0100,$0302,$0504,$0706");
}

// --- format_at() TEXT ---

TEST_F(DataAreaManagerTest, FormatAtTextHandlesPrintableChars) {
    mgr_.mark(0x0000, 0x0004, DataType::TEXT);
    uint8_t mem[] = {'H', 'e', 'l', 'l', 'o'};
    std::string result = mgr_.format_at(0x0000, mem, sizeof(mem));
    EXPECT_EQ(result, "db \"Hello\"");
}

TEST_F(DataAreaManagerTest, FormatAtTextHandlesNonPrintable) {
    mgr_.mark(0x0000, 0x0004, DataType::TEXT);
    uint8_t mem[] = {'H', 'i', 0x00, 0x0D, '!'};
    std::string result = mgr_.format_at(0x0000, mem, sizeof(mem));
    EXPECT_EQ(result, "db \"Hi\",$00,$0D,\"!\"");
}

// --- Address not in any area ---

TEST_F(DataAreaManagerTest, FormatAtReturnsEmptyForNonDataAddress) {
    uint8_t mem[] = {0x00};
    std::string result = mgr_.format_at(0x0000, mem, sizeof(mem));
    EXPECT_TRUE(result.empty());
}

TEST_F(DataAreaManagerTest, FindReturnsNullptrWhenEmpty) {
    EXPECT_EQ(mgr_.find(0x1234), nullptr);
}

}  // namespace
