#include "keyboard.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// The Settings ▸ Input "Host Keyboard Layout" combo offers exactly the *.map
// files under the resources directory, sorted, so a user can see and fix the
// active host layout without editing the config file by hand.
TEST(KeyboardTest, hostLayoutFilesListsTheShippedMapsSorted) {
  auto const files = InputMapper::host_layout_files("./resources");
  ASSERT_FALSE(files.empty());
  EXPECT_NE(std::find(files.begin(), files.end(), "keymap_us.map"),
            files.end());
  EXPECT_TRUE(std::is_sorted(files.begin(), files.end()));
  for (const auto& f : files) {
    EXPECT_EQ(".map", f.substr(f.size() - 4)) << f;
    EXPECT_EQ(std::string::npos, f.find('/'))
        << "bare filename expected: " << f;
  }
}

TEST(KeyboardTest, hostLayoutFilesIgnoresNonMapFilesAndSubdirs) {
  auto const dir =
      std::filesystem::temp_directory_path() /
      ("koncpc_keymaps_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir / "keymap_nested.map");  // a dir
  for (const char* name :
       {"keymap_b.map", "keymap_a.map", "notes.txt", "other.map"}) {
    std::ofstream(dir / name) << "# empty\n";
  }

  auto const files = InputMapper::host_layout_files(dir.string());
  std::filesystem::remove_all(dir);

  EXPECT_EQ(
      (std::vector<std::string>{"keymap_a.map", "keymap_b.map", "other.map"}),
      files);
}

TEST(KeyboardTest, hostLayoutFilesOnAMissingDirectoryIsEmpty) {
  EXPECT_TRUE(
      InputMapper::host_layout_files("/nonexistent/koncpc/resources").empty());
}

TEST(KeyboardTest, parseAllLayoutFiles) {
  InputMapper mapper(/*CPC=*/nullptr);
  for (const auto& entry : std::filesystem::directory_iterator("./resources")) {
    auto path = entry.path().string();
    if (path.substr(path.length() - 4, 4) == ".map") {
      EXPECT_TRUE(mapper.load_layout(path)) << "Error parsing " << path;
    }
  }
}
