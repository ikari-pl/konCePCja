// The licence text a release carries must be the project's own.
//
// WHY THIS TEST EXISTS: the tree kept the stock GPLv2 text as COPYING.txt from
// its Caprice32 origin long after the clean-room cutover moved the project to
// the konCePCja Source License (LICENSE.md), and the archive targets in the
// makefile copied that COPYING.txt into every release and source package —
// GPLv2 text shipped next to binaries licensed under something else
// (beads-7hcq). debian/copyright likewise still described the caprice32
// package as GPL-2.0+. Nothing compiled or ran differently, so nothing
// noticed. This pins the artefacts a release is built from.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string source_dir() {
#ifdef KONCPC_SOURCE_DIR
  return KONCPC_SOURCE_DIR;
#else
  return ".";
#endif
}

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(LicenceFiles, TheGplTextIsGoneFromTheTree) {
  EXPECT_FALSE(std::filesystem::exists(source_dir() + "/COPYING.txt"))
      << "COPYING.txt was the stock GPLv2 text inherited from Caprice32; the "
         "project's licence is LICENSE.md (see NOTICE.md for the provenance)";
  EXPECT_TRUE(std::filesystem::exists(source_dir() + "/LICENSE.md"));
  EXPECT_TRUE(std::filesystem::exists(source_dir() + "/NOTICE.md"));
}

TEST(LicenceFiles, EveryArchiveTargetShipsTheProjectLicenceAndNotice) {
  std::string const makefile = read_file(source_dir() + "/makefile");
  ASSERT_FALSE(makefile.empty()) << "could not read the makefile";
  EXPECT_FALSE(contains(makefile, "COPYING.txt"))
      << "an archive target still copies the GPLv2 text into a release";

  // Every `cp` that stages README.md into an archive stages the licence and
  // the notice beside it: the MinGW zip, the macOS zip, the source package.
  std::istringstream lines(makefile);
  std::string line;
  int archive_copies = 0;
  while (std::getline(lines, line)) {
    if (!contains(line, "cp ") || !contains(line, "README.md")) continue;
    if (!contains(line, "ARCHIVE_DIR") && !contains(line, "SRC_PACKAGE_DIR"))
      continue;
    ++archive_copies;
    EXPECT_TRUE(contains(line, "LICENSE.md")) << line;
    EXPECT_TRUE(contains(line, "NOTICE.md")) << line;
  }
  EXPECT_EQ(3, archive_copies)
      << "expected the MinGW, macOS and source-package targets";
}

TEST(LicenceFiles, DebianCopyrightDescribesThisProjectNotCaprice32) {
  std::string const copyright = read_file(source_dir() + "/debian/copyright");
  ASSERT_FALSE(copyright.empty());
  EXPECT_TRUE(contains(copyright, "Upstream-Name: konCePCja"));
  EXPECT_TRUE(contains(copyright, "konCePCja-Source-1.0.0"));
  EXPECT_FALSE(contains(copyright, "License: GPL-2.0+"))
      << "the project's files are not GPL";
  EXPECT_FALSE(contains(copyright, "caprice32"));
}

TEST(LicenceFiles, RomLicenceDoesNotNameAGplProject) {
  std::string const rom = read_file(source_dir() + "/rom/ROM-LICENSE.txt");
  ASSERT_FALSE(rom.empty());
  EXPECT_FALSE(contains(rom, "GPLv2"))
      << "the ROMs are excluded from the konCePCja Source License, not from "
         "a GPLv2 licence the project no longer uses";
}

TEST(LicenceFiles, BundledLicencesCoverOnlyWhatIsStillShipped) {
  // wGui (LGPL) left with the old GUI; its licence text kept shipping.
  EXPECT_FALSE(
      std::filesystem::exists(source_dir() + "/licenses/wGui License.txt"));
  // The Vera font is still in resources/, so its licence stays.
  EXPECT_TRUE(std::filesystem::exists(source_dir() +
                                      "/licenses/Bitstream Vera License.txt"));
  EXPECT_TRUE(
      std::filesystem::exists(source_dir() + "/resources/vera_mono.ttf"));
}
