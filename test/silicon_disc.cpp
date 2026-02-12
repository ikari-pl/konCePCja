#include <gtest/gtest.h>
#include "silicon_disc.h"
#include <cstring>
#include <filesystem>

namespace {

class SiliconDiscTest : public ::testing::Test {
protected:
    void SetUp() override {
        sd_ = SiliconDisc();
    }
    void TearDown() override {
        silicon_disc_free(sd_);
    }
    SiliconDisc sd_;
};

TEST_F(SiliconDiscTest, InitAllocatesMemory) {
    EXPECT_EQ(sd_.data, nullptr);
    silicon_disc_init(sd_);
    EXPECT_NE(sd_.data, nullptr);
}

TEST_F(SiliconDiscTest, InitIdempotent) {
    silicon_disc_init(sd_);
    uint8_t* first = sd_.data;
    silicon_disc_init(sd_);
    EXPECT_EQ(sd_.data, first);  // same pointer, not reallocated
}

TEST_F(SiliconDiscTest, FreeReleasesMemory) {
    silicon_disc_init(sd_);
    silicon_disc_free(sd_);
    EXPECT_EQ(sd_.data, nullptr);
    EXPECT_FALSE(sd_.enabled);
}

TEST_F(SiliconDiscTest, ClearZerosContents) {
    silicon_disc_init(sd_);
    memset(sd_.data, 0xAA, SILICON_DISC_SIZE);
    silicon_disc_clear(sd_);
    for (size_t i = 0; i < SILICON_DISC_SIZE; i++) {
        EXPECT_EQ(sd_.data[i], 0) << "byte " << i << " not cleared";
        if (sd_.data[i] != 0) break;  // fail fast
    }
}

TEST_F(SiliconDiscTest, BankPtrReturnsCorrectOffsets) {
    silicon_disc_init(sd_);
    EXPECT_EQ(sd_.bank_ptr(0), sd_.data);
    EXPECT_EQ(sd_.bank_ptr(1), sd_.data + SILICON_DISC_BANK_SIZE);
    EXPECT_EQ(sd_.bank_ptr(2), sd_.data + 2 * SILICON_DISC_BANK_SIZE);
    EXPECT_EQ(sd_.bank_ptr(3), sd_.data + 3 * SILICON_DISC_BANK_SIZE);
}

TEST_F(SiliconDiscTest, BankPtrOutOfRange) {
    silicon_disc_init(sd_);
    EXPECT_EQ(sd_.bank_ptr(-1), nullptr);
    EXPECT_EQ(sd_.bank_ptr(4), nullptr);
}

TEST_F(SiliconDiscTest, BankPtrNullWhenNotAllocated) {
    EXPECT_EQ(sd_.bank_ptr(0), nullptr);
}

TEST_F(SiliconDiscTest, OwnsBankWhenEnabled) {
    sd_.enabled = true;
    EXPECT_FALSE(sd_.owns_bank(0));
    EXPECT_FALSE(sd_.owns_bank(3));
    EXPECT_TRUE(sd_.owns_bank(4));
    EXPECT_TRUE(sd_.owns_bank(5));
    EXPECT_TRUE(sd_.owns_bank(6));
    EXPECT_TRUE(sd_.owns_bank(7));
    EXPECT_FALSE(sd_.owns_bank(8));
}

TEST_F(SiliconDiscTest, OwnsBankFalseWhenDisabled) {
    sd_.enabled = false;
    EXPECT_FALSE(sd_.owns_bank(4));
    EXPECT_FALSE(sd_.owns_bank(7));
}

TEST_F(SiliconDiscTest, SaveAndLoadRoundTrip) {
    silicon_disc_init(sd_);
    // Write a known pattern
    for (size_t i = 0; i < SILICON_DISC_SIZE; i++) {
        sd_.data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    std::string path = (std::filesystem::temp_directory_path() / "test_sdisc.ksdx").string();
    ASSERT_TRUE(silicon_disc_save(sd_, path));

    // Load into a fresh disc
    SiliconDisc sd2;
    silicon_disc_init(sd2);
    memset(sd2.data, 0, SILICON_DISC_SIZE);
    ASSERT_TRUE(silicon_disc_load(sd2, path));

    EXPECT_EQ(memcmp(sd_.data, sd2.data, SILICON_DISC_SIZE), 0);
    silicon_disc_free(sd2);
    std::filesystem::remove(path);
}

TEST_F(SiliconDiscTest, LoadRejectsBadHeader) {
    std::string path = (std::filesystem::temp_directory_path() / "test_sdisc_bad.ksdx").string();
    // Write a file with invalid header
    FILE* f = fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite("BADHDR00", 1, 8, f);
    fclose(f);

    silicon_disc_init(sd_);
    EXPECT_FALSE(silicon_disc_load(sd_, path));
    std::filesystem::remove(path);
}

TEST_F(SiliconDiscTest, SaveFailsWhenNotAllocated) {
    std::string path = (std::filesystem::temp_directory_path() / "test_sdisc_null.ksdx").string();
    EXPECT_FALSE(silicon_disc_save(sd_, path));
}

TEST_F(SiliconDiscTest, SizeConstants) {
    EXPECT_EQ(SILICON_DISC_BANKS, 4);
    EXPECT_EQ(SILICON_DISC_FIRST_BANK, 4);
    EXPECT_EQ(SILICON_DISC_BANK_SIZE, 65536u);
    EXPECT_EQ(SILICON_DISC_SIZE, 256u * 1024);
}

} // namespace
