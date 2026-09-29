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

  // The macOS .app bundle is packaged by its own target, not an archive cp:
  // it carried the ROMs and resources and no licence at all.
  EXPECT_TRUE(contains(
      makefile,
      "cp LICENSE.md NOTICE.md README.md $(BUNDLE_DIR)/Contents/Resources/"))
      << "the app bundle must carry the project's licence and notice";
  EXPECT_TRUE(contains(makefile, "cp -r resources rom licenses $(BUNDLE_DIR)"))
      << "and the third-party licences";
}

TEST(LicenceFiles, TheWindowsZipShipsTheProjectLicenceAndNotice) {
  // The MSVC workflow packages its own zip (not via the makefile); it copied
  // the third-party licences and none of the program's own.
  std::string const msvc =
      read_file(source_dir() + "/.github/workflows/msvc.yml");
  ASSERT_FALSE(msvc.empty()) << "could not read the MSVC workflow";
  EXPECT_TRUE(
      contains(msvc, "Copy-Item LICENSE.md, NOTICE.md, README.md $stage"))
      << "the Windows release zip must stage LICENSE.md and NOTICE.md";
  EXPECT_FALSE(contains(msvc, "COPYING.txt"));
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

// The Debian recipe was Caprice32's from 2017 — package name, homepage, SDL2
// build-dependency, a debian/menu entry — shipped untouched in every Linux
// source archive. It must describe this project and its current build.
TEST(DebianPackaging, TheRecipeDescribesKoncepcja) {
  std::string const control = read_file(source_dir() + "/debian/control");
  ASSERT_FALSE(control.empty());
  EXPECT_TRUE(contains(control, "Source: koncepcja"));
  EXPECT_TRUE(contains(control, "Package: koncepcja"));
  EXPECT_TRUE(
      contains(control, "Homepage: https://github.com/ikari-pl/konCePCja"));
  EXPECT_FALSE(contains(control, "caprice"));
  EXPECT_FALSE(contains(control, "libsdl2")) << "the build vendors SDL3";
  EXPECT_TRUE(contains(control, "debhelper-compat"))
      << "compat level lives in Build-Depends, not a debian/compat file";

  std::string const changelog = read_file(source_dir() + "/debian/changelog");
  ASSERT_FALSE(changelog.empty());
  EXPECT_EQ(0u, changelog.find("koncepcja ("))
      << "the changelog names the package; the version comes from "
         "`make debian-changelog` (release-please manifest)";
  EXPECT_FALSE(contains(changelog, "caprice32 ("));

  std::string const rules = read_file(source_dir() + "/debian/rules");
  ASSERT_FALSE(rules.empty());
  EXPECT_FALSE(contains(rules, "caprice"));
  EXPECT_TRUE(contains(rules, "vendor/SDL") || contains(rules, "SDL_SRC"))
      << "the package must build the vendored SDL3 it links";

  // Obsolete since Debian 9 / debhelper 13 respectively.
  EXPECT_FALSE(std::filesystem::exists(source_dir() + "/debian/menu"));
  EXPECT_FALSE(std::filesystem::exists(source_dir() + "/debian/compat"));
}

TEST(DebianPackaging, TheDesktopEntryIsKoncepcjas) {
  std::string const desktop =
      read_file(source_dir() + "/resources/freedesktop/koncepcja.desktop");
  ASSERT_FALSE(desktop.empty());
  EXPECT_TRUE(contains(desktop, "Name=konCePCja"));
  EXPECT_TRUE(contains(desktop, "Icon=koncepcja"));
  EXPECT_TRUE(contains(desktop, "Exec=koncepcja"));
  EXPECT_FALSE(contains(desktop, "Caprice"));
  EXPECT_FALSE(contains(desktop, "caprice32"));
  // The hicolor theme wants square renditions per size; these are rendered
  // from the 1024x1024 .icns master (the marketing PNG is 850x759).
  for (const char* size : {"16x16", "24x24", "32x32", "48x48", "64x64",
                           "128x128", "256x256", "512x512"}) {
    EXPECT_TRUE(std::filesystem::exists(
        source_dir() + "/resources/freedesktop/icons/hicolor/" + size +
        "/apps/koncepcja.png"))
        << size;
  }
}

TEST(DebianPackaging,
     InstallStagesTheDesktopEntryAndDoesNotBakeDestdirIntoEtc) {
  std::string const makefile = read_file(source_dir() + "/makefile");
  ASSERT_FALSE(makefile.empty());
  EXPECT_TRUE(contains(makefile, "share/applications/koncepcja.desktop"));
  EXPECT_TRUE(contains(makefile, "share/pixmaps/koncepcja.png"));
  EXPECT_TRUE(
      contains(makefile, "share/icons/hicolor/$$size/apps/koncepcja.png"))
      << "the square hicolor set must be installed per size";
  EXPECT_TRUE(contains(makefile, "s,__SHARE_PATH__,$(SHARE_PATH),"))
      << "a package build passes SHARE_PATH so /etc/koncepcja.cfg does not "
         "point into the staging directory";
  EXPECT_TRUE(contains(makefile, "debian-changelog:"));
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
