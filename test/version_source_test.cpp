// konCePCja — the version has exactly ONE source of truth.
//
// `.release-please-manifest.json` is it: that is the file release-please bumps
// on every release. Both build systems parse it (makefile via sed,
// CMakeLists.txt via string(REGEX MATCH)), and this test asserts the value that
// actually got compiled into the binary matches it.
//
// WHY THIS TEST EXISTS: the tree used to carry a top-level `VERSION` file that
// was *documented* as the single source of truth. release-please never touched
// it, so it sat at 5.10.0 while shipped releases were at 6.1.0 — every v6
// binary reported `v5.10.0` for its own version, and nothing noticed. A second
// copy of a version always drifts; the only defence is a check that fails.
//
// Because test_runner is built by both the makefile and CMake, this one test
// covers both parsers.

#include <gtest/gtest.h>

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// Extracts the value of the "." key from a release-please manifest.
// Deliberately a tiny hand-rolled scan rather than a JSON dependency: it has to
// agree with a sed one-liner and a CMake regex, so keeping all three simple is
// the point. Returns "" if the key is absent.
std::string manifest_root_version(const std::string& json) {
  size_t const key = json.find("\".\"");
  if (key == std::string::npos) return "";

  size_t const colon = json.find(':', key + 3);
  if (colon == std::string::npos) return "";

  size_t const open = json.find('"', colon + 1);
  if (open == std::string::npos) return "";

  size_t const close = json.find('"', open + 1);
  if (close == std::string::npos) return "";

  return json.substr(open + 1, close - open - 1);
}

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string source_dir() {
#ifdef KONCPC_SOURCE_DIR
  return KONCPC_SOURCE_DIR;
#else
  return ".";
#endif
}

std::string manifest_path() {
  return source_dir() + "/.release-please-manifest.json";
}

}  // namespace

// If this fails, the build system could not find or parse the manifest — which
// means the version baked into the binary is untrustworthy.
TEST(VersionSource, ManifestIsReadableAndHasARootVersion) {
  std::string const json = read_file(manifest_path());
  ASSERT_FALSE(json.empty())
      << "could not read " << manifest_path()
      << " — the version is single-sourced from this file.";

  std::string const version = manifest_root_version(json);
  ASSERT_FALSE(version.empty())
      << "no \".\" key in " << manifest_path() << ":\n"
      << json;
  // Guard against the manifest holding something that is not a version at all.
  EXPECT_NE(version.find('.'), std::string::npos)
      << "manifest version '" << version << "' does not look like x.y.z";
  EXPECT_TRUE(isdigit(static_cast<unsigned char>(version[0])))
      << "manifest version '" << version << "' should start with a digit";
}

// The check that would have caught the 5.10.0-vs-6.1.0 drift on the first
// build.
TEST(VersionSource, CompiledVersionMatchesTheManifest) {
#ifndef KONCPC_VERSION_STRING
  GTEST_FAIL() << "KONCPC_VERSION_STRING was not defined at compile time, so "
                  "this binary cannot report its own version.";
#else
  std::string const json = read_file(manifest_path());
  ASSERT_FALSE(json.empty()) << "could not read " << manifest_path();

  std::string const expected = "v" + manifest_root_version(json);
  EXPECT_EQ(std::string(KONCPC_VERSION_STRING), expected)
      << "The compiled-in version disagrees with "
         ".release-please-manifest.json. That manifest is the ONLY source of "
         "truth — do not 'fix' this by editing another file to match. Check "
         "the "
         "parsers in makefile (KONCPC_VERSION) and CMakeLists.txt.";
#endif
}

// vcpkg.json carries its own version-string, so it is a second copy by
// construction. release-please is configured to bump it (extra-files in
// release-please-config.json); this test is what notices if that ever stops
// working.
TEST(VersionSource, VcpkgManifestMatchesTheManifest) {
  std::string const vcpkg = read_file(source_dir() + "/vcpkg.json");
  if (vcpkg.empty()) GTEST_SKIP() << "no vcpkg.json in this tree";

  size_t const key = vcpkg.find("\"version-string\"");
  if (key == std::string::npos)
    GTEST_SKIP() << "vcpkg.json declares no version-string — nothing to drift";

  size_t const colon = vcpkg.find(':', key);
  size_t const open = vcpkg.find('"', colon + 1);
  size_t const close = vcpkg.find('"', open + 1);
  ASSERT_NE(close, std::string::npos) << "malformed vcpkg.json version-string";
  std::string const vcpkg_version = vcpkg.substr(open + 1, close - open - 1);

  std::string const json = read_file(manifest_path());
  ASSERT_FALSE(json.empty());

  EXPECT_EQ(vcpkg_version, manifest_root_version(json))
      << "vcpkg.json version-string drifted from "
         ".release-please-manifest.json. The manifest is authoritative; update "
         "vcpkg.json (and check the extra-files entry in "
         "release-please-config.json still bumps it).";
}
