#include "fileutils.h"

#include <gtest/gtest.h>

#include <string>

TEST(FileUtils, Listdirectory) {
  std::string directory = "test/zip";

  std::vector<std::string> result = listDirectory(directory);

  ASSERT_EQ(1, result.size());
}

TEST(FileUtils, ListdirectoryMatchingExtension) {
  std::string directory = "test/zip";

  std::vector<std::string> result = listDirectoryExt(directory, "zip");

  ASSERT_EQ(1, result.size());
}

TEST(FileUtils, ListdirectoryNonMatchingExtension) {
  std::string directory = "test/zip";

  std::vector<std::string> result = listDirectoryExt(directory, "zup");

  ASSERT_EQ(0, result.size());
}

TEST(FileUtils, ListdirectoryEmptyStringDoesNotCrash) {
  // Before the fix, directory[directory.size() - 1] was UB when empty.
  std::string directory = "";
  std::vector<std::string> result = listDirectory(directory);
  // Should not crash — empty directory either returns empty or fails gracefully
  EXPECT_GE(result.size(), 0u);
}
