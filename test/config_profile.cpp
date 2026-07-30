#include "config_profile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "koncepcja.h"  // JoystickEmulation / KeyboardSupportMode

namespace fs = std::filesystem;

class ConfigProfileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "koncepcja_profile_test";
    fs::create_directories(test_dir_);
    mgr_.set_profile_dir(test_dir_.string());
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  fs::path test_dir_;
  ConfigProfileManager mgr_;
};

TEST_F(ConfigProfileTest, WriteAndReadRoundTrip) {
  ConfigProfile p;
  p.model = 3;
  p.ram_size = 256;
  p.speed = 8;
  p.scr_scale = 3;
  p.scr_scanlines = 1;
  p.snd_enabled = 0;
  p.snd_playback_rate = 4;
  p.snd_bits = 0;
  p.snd_stereo = 0;
  p.snd_volume = 42;
  p.joystick_emulation = 1;

  std::string path = (test_dir_ / "test.kpf").string();
  EXPECT_EQ(ConfigProfileManager::write_profile(path, p), "");

  ConfigProfile q;
  EXPECT_EQ(ConfigProfileManager::read_profile(path, q), "");

  EXPECT_EQ(q.model, 3u);
  EXPECT_EQ(q.ram_size, 256u);
  EXPECT_EQ(q.speed, 8u);
  EXPECT_EQ(q.scr_scale, 3u);
  EXPECT_EQ(q.scr_scanlines, 1u);
  EXPECT_EQ(q.snd_enabled, 0u);
  EXPECT_EQ(q.snd_playback_rate, 4u);
  EXPECT_EQ(q.snd_bits, 0u);
  EXPECT_EQ(q.snd_stereo, 0u);
  EXPECT_EQ(q.snd_volume, 42u);
  EXPECT_EQ(q.joystick_emulation, 1u);
}

TEST_F(ConfigProfileTest, ListIncludesBuiltins) {
  auto names = mgr_.list();
  EXPECT_NE(std::find(names.begin(), names.end(), "cpc464"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "cpc664"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "cpc6128"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "6128plus"), names.end());
}

TEST_F(ConfigProfileTest, ListIncludesSavedProfiles) {
  // Write a custom profile file
  ConfigProfile p;
  p.model = 0;
  std::string path = (test_dir_ / "custom.kpf").string();
  EXPECT_EQ(ConfigProfileManager::write_profile(path, p), "");

  auto names = mgr_.list();
  EXPECT_NE(std::find(names.begin(), names.end(), "custom"), names.end());
}

TEST_F(ConfigProfileTest, ListIsSorted) {
  ConfigProfile p;
  ConfigProfileManager::write_profile((test_dir_ / "zzz.kpf").string(), p);
  ConfigProfileManager::write_profile((test_dir_ / "aaa.kpf").string(), p);

  auto names = mgr_.list();
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
}

TEST_F(ConfigProfileTest, DeleteProfile) {
  ConfigProfile p;
  std::string path = (test_dir_ / "todel.kpf").string();
  ConfigProfileManager::write_profile(path, p);

  EXPECT_TRUE(fs::exists(path));
  EXPECT_EQ(mgr_.remove("todel"), "");
  EXPECT_FALSE(fs::exists(path));
}

TEST_F(ConfigProfileTest, DeleteNonexistent) {
  auto err = mgr_.remove("doesnotexist");
  EXPECT_NE(err, "");
}

TEST_F(ConfigProfileTest, DeleteBuiltinFails) {
  auto err = mgr_.remove("cpc464");
  EXPECT_NE(err, "");
}

TEST_F(ConfigProfileTest, InvalidNameRejected) {
  EXPECT_NE(mgr_.remove(""), "");
  EXPECT_NE(mgr_.remove("foo bar"), "");
  EXPECT_NE(mgr_.remove("../etc"), "");
  EXPECT_NE(mgr_.remove("a/b"), "");
}

TEST_F(ConfigProfileTest, CurrentDefaultsEmpty) {
  EXPECT_EQ(mgr_.current(), "");
}

TEST_F(ConfigProfileTest, ReadProfileWithComments) {
  // Write a profile with inline comments manually
  std::string path = (test_dir_ / "commented.kpf").string();
  {
    std::ofstream f(path);
    f << "; konCePCja profile\n";
    f << "[general]\n";
    f << "model = 1  ; CPC664\n";
    f << "ram_size = 64\n";
    f << "# this is also a comment\n";
    f << "[sound]\n";
    f << "volume = 50\n";
  }

  ConfigProfile p;
  EXPECT_EQ(ConfigProfileManager::read_profile(path, p), "");
  EXPECT_EQ(p.model, 1u);
  EXPECT_EQ(p.ram_size, 64u);
  EXPECT_EQ(p.snd_volume, 50u);
}

TEST_F(ConfigProfileTest, FrameskipRoundTrip) {
  ConfigProfile p;
  p.frameskip = 1;

  std::string path = (test_dir_ / "frameskip.kpf").string();
  EXPECT_EQ(ConfigProfileManager::write_profile(path, p), "");

  ConfigProfile q;
  EXPECT_EQ(ConfigProfileManager::read_profile(path, q), "");

  EXPECT_EQ(q.frameskip, 1u);
}

TEST_F(ConfigProfileTest, FrameskipDefaultValue) {
  // Write a profile that does not contain a frameskip line
  std::string path = (test_dir_ / "no_frameskip.kpf").string();
  {
    std::ofstream f(path);
    f << "[general]\n";
    f << "model = 2\n";
    f << "ram_size = 128\n";
  }

  ConfigProfile p;
  EXPECT_EQ(ConfigProfileManager::read_profile(path, p), "");
  EXPECT_EQ(p.frameskip, 0u);
}

TEST_F(ConfigProfileTest, ReadNonexistentFile) {
  ConfigProfile p;
  auto err = ConfigProfileManager::read_profile("/nonexistent/path.kpf", p);
  EXPECT_NE(err, "");
}

// ─────────────────────────────────────────────────
// Built-in machine definitions
// ─────────────────────────────────────────────────

// The model numbers are a contract: 0=464, 1=664, 2=6128, 3=6128+, enforced
// elsewhere by read_clamped("system", "model", 2, 0, 3). "6128plus" shipped
// model 4, which is not a machine at all — it read past chROMFile[4] and made
// set_asic(CPC.model == 3) false, so the profile produced a Plus with no ASIC.
TEST_F(ConfigProfileTest, BuiltinProfileModelsAreValid) {
  struct Expected {
    const char* name;
    unsigned int model;
    unsigned int ram_size;
  };
  const Expected kExpected[] = {{"cpc464", 0, 64},
                                {"cpc664", 1, 64},
                                {"cpc6128", 2, 128},
                                {"6128plus", 3, 128}};

  for (const auto& e : kExpected) {
    ConfigProfile const p = ConfigProfileManager::builtin_profile(e.name);
    EXPECT_EQ(p.model, e.model) << e.name << " has the wrong model";
    EXPECT_EQ(p.ram_size, e.ram_size) << e.name << " has the wrong RAM size";
    EXPECT_LE(p.model, 3u) << e.name << " is outside the 0..3 model range";
  }
}

// ─────────────────────────────────────────────────
// Profile sanitisation
// ─────────────────────────────────────────────────

// A .kpf is a user-editable text file and load() writes straight into the
// global CPC struct, where these fields index arrays and size allocations.
TEST_F(ConfigProfileTest, SanitizeClampsOutOfRangeModel) {
  ConfigProfile p;
  p.model = 99;
  ConfigProfileManager::sanitize(p);
  EXPECT_LE(p.model, 3u) << "model must never index past chROMFile[4]";
}

TEST_F(ConfigProfileTest, SanitizeClampsVolumeAndRate) {
  ConfigProfile p;
  p.snd_volume = 9999;
  p.snd_playback_rate = 250;
  ConfigProfileManager::sanitize(p);
  EXPECT_LE(p.snd_volume, 100u);
  EXPECT_LE(p.snd_playback_rate, 4u) << "index into SAMPLE_RATES, not Hz";
}

// Never round a request UP: an edited file must not be able to inflate the
// pbRAMbuffer allocation.
TEST_F(ConfigProfileTest, SanitizeSnapsRamSizeDownToASupportedValue) {
  ConfigProfile p;
  p.ram_size = 200;  // between 192 and 256
  ConfigProfileManager::sanitize(p);
  EXPECT_EQ(p.ram_size, 192u);

  p.ram_size = 999999;
  ConfigProfileManager::sanitize(p);
  EXPECT_EQ(p.ram_size, 4160u) << "clamped to the largest supported size";

  p.ram_size = 1;  // below every supported size
  ConfigProfileManager::sanitize(p);
  EXPECT_EQ(p.ram_size, 64u);
}

TEST_F(ConfigProfileTest, SanitizeLeavesValidValuesAlone) {
  ConfigProfile p;  // defaults are all valid
  ConfigProfile const before = p;
  ConfigProfileManager::sanitize(p);
  EXPECT_EQ(p.model, before.model);
  EXPECT_EQ(p.ram_size, before.ram_size);
  EXPECT_EQ(p.speed, before.speed);
  EXPECT_EQ(p.snd_volume, before.snd_volume);
}

// These are cast straight to enum class values, so an out-of-range number
// would reach switches that do not handle it.
TEST_F(ConfigProfileTest, SanitizeRejectsOutOfRangeEnums) {
  ConfigProfile p;
  p.joystick_emulation = 77;
  p.keyboard_support_mode = 77;
  ConfigProfileManager::sanitize(p);
  EXPECT_LT(p.joystick_emulation,
            static_cast<unsigned int>(JoystickEmulation::Last));
  EXPECT_LT(p.keyboard_support_mode,
            static_cast<unsigned int>(KeyboardSupportMode::Last));
}

// The sentinel Last is not a selectable mode either.
TEST_F(ConfigProfileTest, SanitizeRejectsTheEnumSentinel) {
  ConfigProfile p;
  p.joystick_emulation = static_cast<unsigned int>(JoystickEmulation::Last);
  ConfigProfileManager::sanitize(p);
  EXPECT_LT(p.joystick_emulation,
            static_cast<unsigned int>(JoystickEmulation::Last));
}

// ─────────────────────────────────────────────────
// INI parsing edge cases
// ─────────────────────────────────────────────────

// Documents the accepted comment syntax: ';' and '#' both start a comment,
// whole-line or inline.
//
// Honest note on coverage: this test does NOT discriminate the inline-'#' fix.
// Every field here is parsed with std::stoul, which reads the leading digits
// and stops, so "64 # hash comment" already yielded 64 before the parser
// stripped '#'. The fix is defensive consistency with the whole-line skip
// above it, not a behaviour change — there is no input for which the current
// parser observably differs. Kept because the accepted syntax is worth pinning.
TEST_F(ConfigProfileTest, ReadProfileAcceptsBothCommentStyles) {
  std::string const path = (test_dir_ / "comments.kpf").string();
  std::ofstream f(path);
  f << "; a full-line comment\n"
    << "# another full-line comment\n"
    << "model = 1 ; semicolon comment\n"
    << "ram_size = 64 # hash comment\n"
    << "volume = 55\n";
  f.close();

  ConfigProfile p;
  ASSERT_EQ(ConfigProfileManager::read_profile(path, p), "");
  EXPECT_EQ(p.model, 1u);
  EXPECT_EQ(p.ram_size, 64u) << "'#' inline comment leaked into the value";
}
