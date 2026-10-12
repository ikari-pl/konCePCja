#include "symfile.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class SymfileTest : public testing::Test {
 public:
  void TearDown() {
    for (auto f : tmpFilenames_) {
      ASSERT_EQ(0, unlink(f.c_str()));
    }
  }

 protected:
  std::string createTmpFile() {
#ifdef _MSC_VER
    char tmpFilename[L_tmpnam_s];
    tmpnam_s(tmpFilename, sizeof(tmpFilename));
    FILE* f = fopen(tmpFilename, "w");
    if (!f) return "";
    fclose(f);
#else
    char tmpFilename[] = "test/.koncepcja_tmp_XXXXXX";
    int fd = mkstemp(tmpFilename);
    if (fd < 0) return "";
    close(fd);
#endif
    tmpFilenames_.push_back(tmpFilename);
    return tmpFilename;
  }

  std::string readWholeFile(const std::string& filename) {
    std::ifstream is(filename);
    if (is.bad()) return "**bad stream after open**";
    if (is.fail()) return "**fail stream after open**";
    is.seekg(0, is.end);
    int length = is.tellg();
    is.seekg(0, is.beg);
    char* buffer = new char[length + 1];
    is.read(buffer, length);
    if (is.bad()) return "**bad stream after read**";
    // For some reason, the stream from the tmp file is failed after read on
    // Windows It seems that length has an invalid value.
    if (is.fail()) length = is.gcount();
    buffer[length] = 0;
    is.close();
    return std::string(buffer, length);
  }

  std::vector<std::string> tmpFilenames_;
};

TEST_F(SymfileTest, parseSymfile) {
  Symfile symfile("test/symfile/example.sym");

  std::vector<word> expected_breakpoints = {0x4042};
  std::vector<word> expected_entrypoints = {0x1729};
  std::map<word, std::string> expected_symbols = {
      {0x4005, "hello_world"},
      {0x4012, "foo_bar"},
  };

  ASSERT_EQ(expected_breakpoints, symfile.Breakpoints());
  ASSERT_EQ(expected_entrypoints, symfile.Entrypoints());
  ASSERT_EQ(expected_symbols, symfile.Symbols());
}

TEST_F(SymfileTest, saveSymfile) {
  Symfile symfile;
  symfile.addBreakpoint(0x1234);
  symfile.addBreakpoint(0x1235);
  symfile.addEntrypoint(0x2345);
  symfile.addEntrypoint(0x2346);
  symfile.addSymbol(0x3456, "foobar");
  symfile.addSymbol(0x3457, "foobaz");

  auto filename = createTmpFile();
  ASSERT_FALSE(filename.empty());
  symfile.SaveTo(filename);

  auto got = readWholeFile(filename);
  auto want = readWholeFile("test/symfile/expected.sym");
  ASSERT_EQ(got, want);
}

// -- the table as several threads share it (beads-3o1.7) ---------------------

namespace {
// What the hook was last called with, and how often.
uint64_t g_hook_generation = 0;
int g_hook_calls = 0;
void count_change(uint64_t generation) {
  g_hook_generation = generation;
  g_hook_calls += 1;
}
}  // namespace

TEST(SymfileTable, ANameStandsForOneAddress) {
  Symfile table;
  table.addSymbol(0x4000, "start");
  table.addSymbol(0x4200, "start");  // moved, not doubled
  word addr = 0;
  ASSERT_EQ(0, table.lookupName("start", addr));
  EXPECT_EQ(0x4200, addr);
  EXPECT_EQ("", table.lookupAddr(0x4000))
      << "the old address kept the name, where delSymbol could not reach it";
  ASSERT_EQ(1U, table.listSymbols().size());

  EXPECT_TRUE(table.delSymbol("start"));
  EXPECT_TRUE(table.listSymbols().empty());
  EXPECT_FALSE(table.delSymbol("start")) << "nothing of that name is left";
}

TEST(SymfileTable, AnAddressHasOneName) {
  Symfile table;
  table.addSymbol(0x4000, "first");
  table.addSymbol(0x4000, "second");
  word addr = 0;
  EXPECT_NE(0, table.lookupName("first", addr));
  EXPECT_EQ("second", table.lookupAddr(0x4000));
  EXPECT_EQ(1U, table.listSymbols().size());
}

TEST(SymfileTable, EveryChangeMovesTheGenerationAndCallsTheHook) {
  Symfile table;
  EXPECT_EQ(0U, table.generation());
  g_hook_calls = 0;
  table.set_changed_hook(&count_change);

  table.addSymbol(0x4000, "start");
  EXPECT_EQ(1U, table.generation());
  EXPECT_EQ(1, g_hook_calls);
  EXPECT_EQ(1U, g_hook_generation);

  EXPECT_FALSE(table.delSymbol("absent"));
  EXPECT_EQ(1U, table.generation()) << "nothing changed";
  EXPECT_EQ(1, g_hook_calls);

  EXPECT_TRUE(table.delSymbol("start"));
  EXPECT_EQ(2U, table.generation());
  EXPECT_EQ(2, g_hook_calls);

  // Breakpoints and entry points are not symbols.
  table.addBreakpoint(0x4000);
  table.addEntrypoint(0x4000);
  EXPECT_EQ(2U, table.generation());

  table.clear();
  EXPECT_EQ(2U, table.generation()) << "there was no symbol to clear";
  table.addSymbol(0x1, "a");
  table.clear();
  EXPECT_EQ(4U, table.generation());
  EXPECT_EQ(4, g_hook_calls);

  table.set_changed_hook(nullptr);
  table.addSymbol(0x2, "b");
  EXPECT_EQ(5U, table.generation());
  EXPECT_EQ(4, g_hook_calls) << "the hook was removed";
}

TEST(SymfileTable, ABatchIsOneChange) {
  Symfile loaded;
  loaded.addSymbol(0xC000, "rom_entry");
  loaded.addSymbol(0xC003, "rom_name");
  loaded.addSymbol(0x4200, "main");

  Symfile table;
  table.addSymbol(0x4200, "start");
  g_hook_calls = 0;
  table.set_changed_hook(&count_change);
  EXPECT_EQ(3U, table.addSymbols(loaded));
  EXPECT_EQ(1, g_hook_calls) << "one change, however many symbols";
  EXPECT_EQ(2U, table.generation());
  EXPECT_EQ("main", table.lookupAddr(0x4200));
  EXPECT_EQ(3U, table.listSymbols().size());

  EXPECT_EQ(0U, table.addSymbols(Symfile()));
  EXPECT_EQ(0U, table.addSymbols(table)) << "itself: nothing to add";
  EXPECT_EQ(1, g_hook_calls) << "an empty batch is no change";

  std::vector<std::pair<word, std::string>> const batch = {{0x10, "x"},
                                                           {0x20, "y"}};
  EXPECT_EQ(2U, table.addSymbols(batch));
  EXPECT_EQ(2, g_hook_calls);

  uint64_t generation = 0;
  auto const listed = table.listSymbols(generation);
  EXPECT_EQ(3U, generation);
  ASSERT_EQ(5U, listed.size());
  EXPECT_EQ(0x10, listed.front().first) << "in address order";
  EXPECT_EQ("rom_name", listed.back().second);
  table.set_changed_hook(nullptr);
}

TEST(SymfileTable, AFileThatCannotBeOpenedSaysSo) {
  EXPECT_TRUE(Symfile().readable());
  Symfile const missing("test/no-such-directory/no-such-file.sym");
  EXPECT_FALSE(missing.readable());
  EXPECT_TRUE(missing.Symbols().empty());
}

// The DevTools look symbols up while they draw, and the text IPC and the
// KCPC server edit the table from their own threads. Under ThreadSanitizer
// this is the test that reports a table without a lock.
TEST(SymfileTable, ReadersAndWritersShareTheTable) {
  Symfile table;
  constexpr int kWriters = 3;
  constexpr int kPerWriter = 400;
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&table, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        auto const addr = static_cast<word>(w * 0x1000 + i);
        table.addSymbol(addr,
                        "s" + std::to_string(w) + "_" + std::to_string(i));
        if (i % 4 == 3) {
          table.delSymbol("s" + std::to_string(w) + "_" + std::to_string(i));
        }
      }
    });
  }
  threads.emplace_back([&table] {
    word addr = 0;
    for (int i = 0; i < 2000; ++i) {
      (void)table.lookupAddr(static_cast<word>(i));
      (void)table.lookupName("s1_7", addr);
      if (i % 100 == 0) (void)table.listSymbols();
    }
  });
  for (std::thread& t : threads) t.join();
  EXPECT_EQ(static_cast<size_t>(kWriters * (kPerWriter - kPerWriter / 4)),
            table.listSymbols().size());
  EXPECT_EQ(static_cast<uint64_t>(kWriters * (kPerWriter + kPerWriter / 4)),
            table.generation());
}
