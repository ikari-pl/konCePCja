#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include "pokes.h"

namespace {

// Mock memory for testing
static std::map<uint16_t, uint8_t> mock_mem;

void mock_write(uint16_t addr, uint8_t val) {
    mock_mem[addr] = val;
}

uint8_t mock_read(uint16_t addr) {
    auto it = mock_mem.find(addr);
    return (it != mock_mem.end()) ? it->second : 0;
}

class PokesTest : public ::testing::Test {
protected:
    void SetUp() override {
        mgr.clear();
        mock_mem.clear();
    }
    PokeManager mgr;
};

// --- Parsing tests ---

TEST_F(PokesTest, ParseValidSingleGame) {
    std::string pok =
        "NJet Set Willy\n"
        "MInfinite Lives\n"
        "Z 35899 0 0\n"
        "Y 35900 0 0\n"
        "MNo Nasties\n"
        "Y 34795 195 0\n";

    auto err = mgr.load_from_string(pok);
    EXPECT_EQ(err, "");
    ASSERT_EQ(mgr.games().size(), 1u);
    EXPECT_EQ(mgr.games()[0].title, "Jet Set Willy");
    ASSERT_EQ(mgr.games()[0].pokes.size(), 2u);

    // First poke: Infinite Lives, 2 values
    EXPECT_EQ(mgr.games()[0].pokes[0].description, "Infinite Lives");
    ASSERT_EQ(mgr.games()[0].pokes[0].values.size(), 2u);
    EXPECT_EQ(mgr.games()[0].pokes[0].values[0].address, 35899);
    EXPECT_EQ(mgr.games()[0].pokes[0].values[0].value, 0);
    EXPECT_EQ(mgr.games()[0].pokes[0].values[1].address, 35900);
    EXPECT_EQ(mgr.games()[0].pokes[0].values[1].value, 0);

    // Second poke: No Nasties, 1 value
    EXPECT_EQ(mgr.games()[0].pokes[1].description, "No Nasties");
    ASSERT_EQ(mgr.games()[0].pokes[1].values.size(), 1u);
    EXPECT_EQ(mgr.games()[0].pokes[1].values[0].address, 34795);
    EXPECT_EQ(mgr.games()[0].pokes[1].values[0].value, 195);
}

TEST_F(PokesTest, ParseMultipleGames) {
    std::string pok =
        "NJet Set Willy\n"
        "MInfinite Lives\n"
        "Y 35899 0 0\n"
        "NManic Miner\n"
        "MInfinite Lives\n"
        "Y 35136 0 0\n";

    auto err = mgr.load_from_string(pok);
    EXPECT_EQ(err, "");
    ASSERT_EQ(mgr.games().size(), 2u);
    EXPECT_EQ(mgr.games()[0].title, "Jet Set Willy");
    EXPECT_EQ(mgr.games()[1].title, "Manic Miner");
    ASSERT_EQ(mgr.games()[0].pokes.size(), 1u);
    ASSERT_EQ(mgr.games()[1].pokes.size(), 1u);
}

TEST_F(PokesTest, ParseAskUserValue256) {
    std::string pok =
        "NTest Game\n"
        "MLives\n"
        "Y 1000 256 0\n";

    auto err = mgr.load_from_string(pok);
    EXPECT_EQ(err, "");
    ASSERT_EQ(mgr.games().size(), 1u);
    ASSERT_EQ(mgr.games()[0].pokes[0].values.size(), 1u);
    EXPECT_TRUE(mgr.games()[0].pokes[0].values[0].needs_input);
    EXPECT_EQ(mgr.games()[0].pokes[0].values[0].value, 0); // default
}

TEST_F(PokesTest, ParseEmptyFileReturnsError) {
    auto err = mgr.load_from_string("");
    EXPECT_NE(err, "");
    EXPECT_EQ(mgr.games().size(), 0u);
}

TEST_F(PokesTest, ParseInvalidMBeforeN) {
    std::string pok =
        "MInfinite Lives\n"
        "Y 1000 0 0\n";
    auto err = mgr.load_from_string(pok);
    EXPECT_NE(err, "");
}

TEST_F(PokesTest, ParseInvalidValueLine) {
    std::string pok =
        "NGame\n"
        "MPoke\n"
        "Y not_a_number\n";
    auto err = mgr.load_from_string(pok);
    EXPECT_NE(err, "");
}

TEST_F(PokesTest, ParseWithWindowsCRLF) {
    std::string pok =
        "NGame\r\n"
        "MPoke\r\n"
        "Y 1000 42 0\r\n";
    auto err = mgr.load_from_string(pok);
    EXPECT_EQ(err, "");
    ASSERT_EQ(mgr.games().size(), 1u);
    EXPECT_EQ(mgr.games()[0].title, "Game");
    EXPECT_EQ(mgr.games()[0].pokes[0].values[0].value, 42);
}

TEST_F(PokesTest, ParseSkipsUnknownPrefixes) {
    // Lines starting with unknown chars should be skipped
    std::string pok =
        "; This is a comment\n"
        "NGame\n"
        "MPoke\n"
        "Y 1000 42 0\n";
    auto err = mgr.load_from_string(pok);
    EXPECT_EQ(err, "");
    ASSERT_EQ(mgr.games().size(), 1u);
}

TEST_F(PokesTest, ParseOriginalValuePreserved) {
    std::string pok =
        "NGame\n"
        "MPoke\n"
        "Y 1000 42 99\n";
    auto err = mgr.load_from_string(pok);
    EXPECT_EQ(err, "");
    EXPECT_EQ(mgr.games()[0].pokes[0].values[0].original_value, 99);
}

// --- Apply/Unapply tests ---

TEST_F(PokesTest, ApplyWritesValues) {
    std::string pok =
        "NGame\n"
        "MCheat\n"
        "Z 1000 42 0\n"
        "Y 1001 99 0\n";
    ASSERT_EQ(mgr.load_from_string(pok), "");

    mock_mem[1000] = 10;
    mock_mem[1001] = 20;

    int n = mgr.apply(0, 0, mock_write, mock_read);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(mock_mem[1000], 42);
    EXPECT_EQ(mock_mem[1001], 99);
    EXPECT_TRUE(mgr.games()[0].pokes[0].applied);
}

TEST_F(PokesTest, ApplySavesOriginalValues) {
    std::string pok =
        "NGame\n"
        "MCheat\n"
        "Y 1000 42 0\n";
    ASSERT_EQ(mgr.load_from_string(pok), "");

    mock_mem[1000] = 77;  // current memory value

    int n = mgr.apply(0, 0, mock_write, mock_read);
    EXPECT_EQ(n, 1);
    // original_value should be updated to what was read from memory
    EXPECT_EQ(mgr.games()[0].pokes[0].values[0].original_value, 77);
}

TEST_F(PokesTest, UnapplyRestoresValues) {
    std::string pok =
        "NGame\n"
        "MCheat\n"
        "Z 1000 42 0\n"
        "Y 1001 99 0\n";
    ASSERT_EQ(mgr.load_from_string(pok), "");

    mock_mem[1000] = 10;
    mock_mem[1001] = 20;

    mgr.apply(0, 0, mock_write, mock_read);
    EXPECT_EQ(mock_mem[1000], 42);
    EXPECT_EQ(mock_mem[1001], 99);

    int n = mgr.unapply(0, 0, mock_write);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(mock_mem[1000], 10);  // restored
    EXPECT_EQ(mock_mem[1001], 20);  // restored
    EXPECT_FALSE(mgr.games()[0].pokes[0].applied);
}

TEST_F(PokesTest, UnapplyFailsIfNotApplied) {
    std::string pok =
        "NGame\n"
        "MCheat\n"
        "Y 1000 42 0\n";
    ASSERT_EQ(mgr.load_from_string(pok), "");

    int n = mgr.unapply(0, 0, mock_write);
    EXPECT_EQ(n, -1);
}

TEST_F(PokesTest, ApplyAllAppliesAllPokes) {
    std::string pok =
        "NGame\n"
        "MCheat1\n"
        "Y 1000 42 0\n"
        "MCheat2\n"
        "Z 2000 10 0\n"
        "Y 2001 20 0\n";
    ASSERT_EQ(mgr.load_from_string(pok), "");

    int total_vals = 0;
    int n = mgr.apply_all(0, mock_write, mock_read, &total_vals);
    EXPECT_EQ(n, 2);       // 2 pokes applied
    EXPECT_EQ(total_vals, 3); // 1 + 2 values
    EXPECT_EQ(mock_mem[1000], 42);
    EXPECT_EQ(mock_mem[2000], 10);
    EXPECT_EQ(mock_mem[2001], 20);
}

TEST_F(PokesTest, ApplyInvalidIndicesReturnsError) {
    std::string pok =
        "NGame\n"
        "MCheat\n"
        "Y 1000 42 0\n";
    ASSERT_EQ(mgr.load_from_string(pok), "");

    EXPECT_EQ(mgr.apply(99, 0, mock_write, mock_read), -1);
    EXPECT_EQ(mgr.apply(0, 99, mock_write, mock_read), -1);
    EXPECT_EQ(mgr.unapply(99, 0, mock_write), -1);
    EXPECT_EQ(mgr.unapply(0, 99, mock_write), -1);
    EXPECT_EQ(mgr.apply_all(99, mock_write, mock_read), -1);
}

// --- File loading test ---

TEST_F(PokesTest, LoadFromFile) {
    auto tmp = std::filesystem::temp_directory_path() / "test_pokes.pok";
    {
        std::ofstream f(tmp);
        f << "NTest Game\n"
          << "MInfinite Lives\n"
          << "Y 1000 42 0\n";
    }

    auto err = mgr.load(tmp.string());
    EXPECT_EQ(err, "");
    ASSERT_EQ(mgr.games().size(), 1u);
    EXPECT_EQ(mgr.games()[0].title, "Test Game");

    std::filesystem::remove(tmp);
}

TEST_F(PokesTest, LoadNonexistentFileReturnsError) {
    auto err = mgr.load("/nonexistent/path/file.pok");
    EXPECT_NE(err, "");
}

// --- Clear test ---

TEST_F(PokesTest, ClearRemovesAllGames) {
    std::string pok =
        "NGame\n"
        "MCheat\n"
        "Y 1000 42 0\n";
    ASSERT_EQ(mgr.load_from_string(pok), "");
    ASSERT_EQ(mgr.games().size(), 1u);

    mgr.clear();
    EXPECT_EQ(mgr.games().size(), 0u);
}

// --- List formatting test ---

TEST_F(PokesTest, ListFormatting) {
    std::string pok =
        "NJet Set Willy\n"
        "MInfinite Lives\n"
        "Z 35899 0 0\n"
        "Y 35900 0 0\n"
        "MNo Nasties\n"
        "Y 34795 195 0\n"
        "NManic Miner\n"
        "MSkip Level\n"
        "Z 1000 0 0\n"
        "Y 1001 0 0\n";
    ASSERT_EQ(mgr.load_from_string(pok), "");

    // Verify structure for list formatting
    ASSERT_EQ(mgr.games().size(), 2u);
    EXPECT_EQ(mgr.games()[0].title, "Jet Set Willy");
    ASSERT_EQ(mgr.games()[0].pokes.size(), 2u);
    EXPECT_EQ(mgr.games()[0].pokes[0].description, "Infinite Lives");
    EXPECT_EQ(mgr.games()[0].pokes[0].values.size(), 2u);
    EXPECT_EQ(mgr.games()[0].pokes[1].description, "No Nasties");
    EXPECT_EQ(mgr.games()[0].pokes[1].values.size(), 1u);
    EXPECT_EQ(mgr.games()[1].title, "Manic Miner");
    ASSERT_EQ(mgr.games()[1].pokes.size(), 1u);
    EXPECT_EQ(mgr.games()[1].pokes[0].description, "Skip Level");
    EXPECT_EQ(mgr.games()[1].pokes[0].values.size(), 2u);
}

// --- Z/Y continuation test ---

TEST_F(PokesTest, ZContinuationYTermination) {
    // Z means more values follow, Y means last value
    std::string pok =
        "NGame\n"
        "MBig Cheat\n"
        "Z 1000 1 0\n"
        "Z 1001 2 0\n"
        "Z 1002 3 0\n"
        "Y 1003 4 0\n";
    ASSERT_EQ(mgr.load_from_string(pok), "");
    ASSERT_EQ(mgr.games()[0].pokes[0].values.size(), 4u);
    EXPECT_EQ(mgr.games()[0].pokes[0].values[0].value, 1);
    EXPECT_EQ(mgr.games()[0].pokes[0].values[1].value, 2);
    EXPECT_EQ(mgr.games()[0].pokes[0].values[2].value, 3);
    EXPECT_EQ(mgr.games()[0].pokes[0].values[3].value, 4);
}


TEST_F(PokesTest, DoubleApplyPreservesOriginalValue) {
    std::string pok =
        "NGame\n"
        "MCheat\n"
        "Y 1000 42 0\n";
    ASSERT_EQ(mgr.load_from_string(pok), "");

    mock_mem[1000] = 77;  // original memory value

    // First apply: should succeed and save original_value=77
    int n = mgr.apply(0, 0, mock_write, mock_read);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(mock_mem[1000], 42);
    EXPECT_EQ(mgr.games()[0].pokes[0].values[0].original_value, 77);

    // Second apply: should return 0 (already applied), not overwrite original_value
    n = mgr.apply(0, 0, mock_write, mock_read);
    EXPECT_EQ(n, 0);
    // original_value must still be 77, not 42
    EXPECT_EQ(mgr.games()[0].pokes[0].values[0].original_value, 77);

    // Unapply: should restore original value
    n = mgr.unapply(0, 0, mock_write);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(mock_mem[1000], 77);  // correctly restored
}

}  // namespace
