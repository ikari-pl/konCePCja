#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include "config_profile.h"

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

TEST_F(ConfigProfileTest, ReadNonexistentFile) {
    ConfigProfile p;
    auto err = ConfigProfileManager::read_profile("/nonexistent/path.kpf", p);
    EXPECT_NE(err, "");
}

