#include "configuration.h"

#include <gtest/gtest.h>
#include <stdlib.h>

#include "koncepcja.h"
#include "silicon_disc.h"
#include "slotshandler.h"
#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <errno.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

// TODO(cpitrat): Cleaner way to handle this
extern char chAppPath[_MAX_PATH + 1];
// TODO(cpitrat): Make this a list or vector in t_CPC
extern t_disk_format disk_format[8];

class ConfigurationTest : public testing::Test {
 public:
  // loadConfiguration() writes process-global device state, not just the
  // t_CPC it is handed: system/silicon_disc lands in g_silicon_disc.enabled.
  // Leaving that on leaked into RamExpansionTest, where an enabled Silicon
  // Disc suppresses the out-of-range bank clamp. Restore it.
  void SetUp() { saved_silicon_disc_ = g_silicon_disc.enabled; }

  void TearDown() {
    g_silicon_disc.enabled = saved_silicon_disc_;
    for (auto f : tmpFilenames_) {
      ASSERT_EQ(0, unlink(f.c_str()));
    }
  }

  const std::string getTmpFilename(unsigned int idx) {
    while (tmpFilenames_.size() <= idx) {
      std::string fn;
      createTmpFile(fn);
      tmpFilenames_.push_back(fn);
    }
    return tmpFilenames_[idx];
  }

 protected:
  std::vector<std::string> tmpFilenames_;
  bool saved_silicon_disc_ = false;
  config::Config configuration_;

 private:
  void createTmpFile(std::string& filename) {
#ifdef _MSC_VER
    char tmpFilename[L_tmpnam_s];
    tmpnam_s(tmpFilename, sizeof(tmpFilename));
    FILE* f = fopen(tmpFilename, "w");
    ASSERT_NE(f, nullptr);
    fclose(f);
#else
    char tmpFilename[] = "test/.koncepcja_tmp_XXXXXX";
    int fd = mkstemp(tmpFilename);
    ASSERT_GE(fd, 0);
    close(fd);
#endif
    filename = tmpFilename;
  }
};

// "Integrated" tests (on a real file)
TEST_F(ConfigurationTest, parseFileAndSaveBack) {
  std::ofstream configFile(getTmpFilename(0));
  configFile << "# A comment in top\n"
             << "[system] # A comment at the end of the line\n"
             << "model=42\n"
             << "# This is an unused param:\n"
             << "unused=1\n"
             << "# Here is an empty line:\n"
             << "\n"
             << "resources_path=./resources\n"
             << "\n"
             << "[video]\n"
             << "resources_path=./toto\n"
             << "model = 8\n"
             << "[input]\n"
             << "resources_path=./directory with spaces\n"
             << "[sound]\n"
             << "enabled=1";
  configFile.close();

  configuration_.parseFile(getTmpFilename(0));

  ASSERT_EQ(42, configuration_.getIntValue("system", "model", 0));
  ASSERT_EQ(8, configuration_.getIntValue("video", "model", 0));
  ASSERT_EQ(10, configuration_.getIntValue("sound", "model", 10));
  ASSERT_EQ("./resources",
            configuration_.getStringValue("system", "resources_path", "none"));
  ASSERT_EQ("./toto",
            configuration_.getStringValue("video", "resources_path", "none"));
  ASSERT_EQ("./directory with spaces",
            configuration_.getStringValue("input", "resources_path", "none"));
  ASSERT_EQ("./default", configuration_.getStringValue(
                             "sound", "resources_path", "./default"));

  configuration_.saveToFile(getTmpFilename(1));
  config::Config otherConfig;
  otherConfig.parseFile(getTmpFilename(1));

  ASSERT_EQ(42, otherConfig.getIntValue("system", "model", 0));
  ASSERT_EQ(8, otherConfig.getIntValue("video", "model", 0));
  ASSERT_EQ(10, otherConfig.getIntValue("sound", "model", 10));
  ASSERT_EQ("./resources",
            otherConfig.getStringValue("system", "resources_path", "none"));
  ASSERT_EQ("./toto",
            otherConfig.getStringValue("video", "resources_path", "none"));
  ASSERT_EQ("./directory with spaces",
            otherConfig.getStringValue("input", "resources_path", "none"));
  ASSERT_EQ("./default",
            otherConfig.getStringValue("sound", "resources_path", "./default"));
}

TEST_F(ConfigurationTest, parseFileDoesntExist) {
  configuration_.parseFile("/a/non/existing/file");
}

namespace {
std::string readFile(const std::string& path) {
  std::ifstream ifs(path);
  std::stringstream buffer;
  buffer << ifs.rdbuf();
  return buffer.str();
}
}  // namespace

// Saving is an edit of the parsed text, not a regeneration of it. Before this,
// toStream dumped the key/value map and every comment in koncepcja.cfg — a
// self-documenting template — was deleted the first time anything saved.
TEST_F(ConfigurationTest, saveToFileKeepsCommentsAndLayout) {
  std::string const initialConfig =
      "# A comment in top\n"
      "[system] # A comment at the end of the line\n"
      "model=42\n";
  configuration_.parseString(initialConfig);

  configuration_.saveToFile(getTmpFilename(0));

  ASSERT_EQ(initialConfig, readFile(getTmpFilename(0)));
}

// Honest note on coverage: this test does NOT discriminate the key-loss bug.
// The old writer dumped config_, which already held every parsed key, so it
// passed before the fix too — the keys were lost one level up, in
// saveConfiguration, which never read the file it overwrote (see
// saveConfigurationEditsTheFileInsteadOfReplacingIt, which does discriminate).
// Kept as a forward guard: rewriting lines in place must not drop a key.
TEST_F(ConfigurationTest, saveToFileKeepsParsedKeysItNeverSets) {
  configuration_.parseString(
      "[system]\n"
      "model=42\n"
      "a_key_from_a_newer_build=hello\n");

  configuration_.setIntValue("system", "model", 2);
  configuration_.saveToFile(getTmpFilename(0));

  config::Config reloaded;
  reloaded.parseFile(getTmpFilename(0));
  ASSERT_EQ(2, reloaded.getIntValue("system", "model", 0));
  ASSERT_EQ("hello", reloaded.getStringValue(
                         "system", "a_key_from_a_newer_build", "GONE"));
}

TEST_F(ConfigurationTest,
       saveToFileRewritesValueInPlaceKeepingSpacingAndComment) {
  configuration_.parseString(
      "[system]\n"
      "model = 42  # the machine\n"
      "speed=4\n");

  configuration_.setIntValue("system", "model", 3);
  configuration_.saveToFile(getTmpFilename(0));

  ASSERT_EQ(
      "[system]\n"
      "model = 3  # the machine\n"
      "speed=4\n",
      readFile(getTmpFilename(0)));
}

// New keys join their section (after its last non-blank line, so trailing
// blank lines stay trailing); a section the file never had goes at the end.
TEST_F(ConfigurationTest, saveToFileAppendsNewKeysToTheirSection) {
  configuration_.parseString(
      "[system]\n"
      "model=2\n"
      "\n"
      "[video]\n"
      "scr_scale=1\n");

  configuration_.setIntValue("system", "speed", 4);
  configuration_.setIntValue("sound", "volume", 80);
  configuration_.saveToFile(getTmpFilename(0));

  ASSERT_EQ(
      "[system]\n"
      "model=2\n"
      "speed=4\n"
      "\n"
      "[video]\n"
      "scr_scale=1\n"
      "[sound]\n"
      "volume=80\n",
      readFile(getTmpFilename(0)));
}

// A file that repeats a key must not re-parse to the stale duplicate: every
// occurrence carries the new value.
TEST_F(ConfigurationTest, saveToFileRewritesEveryOccurrenceOfARepeatedKey) {
  configuration_.parseString(
      "[system]\n"
      "model=1\n"
      "model=2\n");

  configuration_.setIntValue("system", "model", 3);
  configuration_.saveToFile(getTmpFilename(0));

  ASSERT_EQ(
      "[system]\n"
      "model=3\n"
      "model=3\n",
      readFile(getTmpFilename(0)));

  config::Config reloaded;
  reloaded.parseFile(getTmpFilename(0));
  ASSERT_EQ(3, reloaded.getIntValue("system", "model", 0));
}

// Re-saving must converge: the emulator writes this file on every file open.
TEST_F(ConfigurationTest, saveToFileIsIdempotent) {
  configuration_.parseString(
      "# top\n"
      "[system]\n"
      "model = 2 # a comment\n"
      "\n"
      "[video]\n"
      "scr_scale=1\n");
  configuration_.setIntValue("sound", "volume", 80);
  configuration_.saveToFile(getTmpFilename(0));
  std::string const once = readFile(getTmpFilename(0));

  config::Config again;
  again.parseFile(getTmpFilename(0));
  again.saveToFile(getTmpFilename(1));

  ASSERT_EQ(once, readFile(getTmpFilename(1)));
}

// A Config that never parsed anything (a config file that does not exist yet)
// still generates a complete file from scratch.
TEST_F(ConfigurationTest, saveToFileGeneratesFromScratchWhenNothingWasParsed) {
  configuration_.setIntValue("system", "model", 2);
  configuration_.setIntValue("sound", "volume", 80);

  configuration_.saveToFile(getTmpFilename(0));

  ASSERT_EQ(
      "[sound]\n"
      "volume=80\n"
      "[system]\n"
      "model=2\n",
      readFile(getTmpFilename(0)));
}

#ifndef _MSC_VER
// POSIX-only: chmod with S_IRUSR/S_IRGRP/S_IROTH not available on MSVC
TEST_F(ConfigurationTest, saveToNonWritableFile) {
  // Make a non-writable config file
  auto configFileName = getTmpFilename(0);
  ASSERT_EQ(0, chmod(configFileName.c_str(), S_IRUSR | S_IRGRP | S_IROTH));
  std::string initalConfig = "[system]\nmodel=42\n";
  configuration_.parseString(initalConfig);

  EXPECT_FALSE(configuration_.saveToFile(getTmpFilename(0)));

  // This works when the file is writable
  ASSERT_EQ(
      0, chmod(configFileName.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));
  EXPECT_TRUE(configuration_.saveToFile(getTmpFilename(0)));
}
#endif

// TODO(cpitrat): test about every value in conf ?
TEST_F(ConfigurationTest, loadConfigurationWithValidContent) {
  std::ofstream configFile(getTmpFilename(0));
  configFile << "[system]\n"
             << "model=1\n"
             << "jumpers=30\n"
             << "ram_size=128\n"
             << "speed=32\n"
             << "printer=1\n"
             << "resources_path=./resources\n"
             << "[file]\n"
             << "fmt02=Test disk "
                "format,40,1,9,2,82,229,193,198,194,199,195,200,196,201,197\n";
  configFile.close();

  t_CPC CPC[2];
  loadConfiguration(CPC[0], getTmpFilename(0));
  saveConfiguration(CPC[0], getTmpFilename(1));
  loadConfiguration(CPC[1], getTmpFilename(1));

  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(1, CPC[i].model) << "with i = " << i;
    ASSERT_EQ(30, CPC[i].jumpers) << "with i = " << i;
    ASSERT_EQ(128, CPC[i].ram_size) << "with i = " << i;
    ASSERT_EQ(32, CPC[i].speed) << "with i = " << i;
    ASSERT_EQ(1, CPC[i].limit_speed) << "with i = " << i;
    ASSERT_EQ(1, CPC[i].printer) << "with i = " << i;
    ASSERT_EQ("./resources", std::string(CPC[i].resources_path))
        << "with i = " << i;
    // TODO(cpitrat): move disk_format array in t_CPC
    ASSERT_EQ("Test disk format", disk_format[2].label);
    ASSERT_EQ(40, disk_format[0].tracks);
    ASSERT_EQ(1, disk_format[0].sides);
    ASSERT_EQ(9, disk_format[0].sectors);
    ASSERT_EQ(2, disk_format[0].sector_size);
    ASSERT_EQ(82, disk_format[0].gap3_length);
    ASSERT_EQ(229, disk_format[0].filler_byte);
    ASSERT_EQ(193, disk_format[0].sector_ids[0][0]);
    ASSERT_EQ(198, disk_format[0].sector_ids[0][1]);
    ASSERT_EQ(194, disk_format[0].sector_ids[0][2]);
    ASSERT_EQ(199, disk_format[0].sector_ids[0][3]);
    ASSERT_EQ(195, disk_format[0].sector_ids[0][4]);
    ASSERT_EQ(200, disk_format[0].sector_ids[0][5]);
    ASSERT_EQ(196, disk_format[0].sector_ids[0][6]);
    ASSERT_EQ(201, disk_format[0].sector_ids[0][7]);
    ASSERT_EQ(197, disk_format[0].sector_ids[0][8]);
  }
}

// TODO(cpitrat): test about every value in conf ?
TEST_F(ConfigurationTest, loadConfigurationWithInvalidValues) {
  std::ofstream configFile(getTmpFilename(0));
  configFile
      << "[system]\n"
      << "model=4\n"       // model should be <= 3 - default to 2
      << "jumpers=255\n"   // jumpers is & with 0x1e == 30
      << "ram_size=704\n"  // 704 is not a valid RAM size, defaults to 128
      << "speed=64\n"      // max speed is 32 - will default to 4
      << "printer=2\n"     // printer should be 0 or 1 - it's & with 1
      << "resources_path=\n";
  configFile.close();
  ASSERT_NE(nullptr, getcwd(chAppPath, sizeof(chAppPath) - 1))
      << "getcwd error: " << strerror(errno);

  t_CPC CPC;
  loadConfiguration(CPC, getTmpFilename(0));

  ASSERT_EQ(2, CPC.model);
  ASSERT_EQ(30, CPC.jumpers);
  ASSERT_EQ(128, CPC.ram_size);
  ASSERT_EQ(4, CPC.speed);
  ASSERT_EQ(1, CPC.limit_speed);
  ASSERT_EQ(0, CPC.printer);
  ASSERT_EQ(std::string(chAppPath) + "/resources",
            std::string(CPC.resources_path));
}

// Every key loadConfiguration reads, saveConfiguration must write back. Five
// did not — vsync, run_tier, lightgun, silicon_disc and max_stack_size were
// read at boot and then deleted by the next save, so setting any of them
// silently reverted to the default. The MRU list auto-saves on every file
// open, so "the next save" was one disk load away.
TEST_F(ConfigurationTest, saveConfigurationPreservesEverySettingItReads) {
  std::ofstream configFile(getTmpFilename(0));
  configFile << "# a hand-written note\n"
             << "[system]\n"
             << "model=2\n"
             << "run_tier=2\n"
             << "silicon_disc=1\n"
             << "a_key_from_a_newer_build=keep me\n"
             << "[video]\n"
             << "vsync=0\n"
             << "[input]\n"
             << "lightgun=2\n"
             << "[devtools]\n"
             << "max_stack_size=99\n";
  configFile.close();

  t_CPC CPC[2];
  loadConfiguration(CPC[0], getTmpFilename(0));
  saveConfiguration(CPC[0], getTmpFilename(1));

  config::Config saved;
  saved.parseFile(getTmpFilename(1));
  EXPECT_EQ(0, saved.getIntValue("video", "vsync", 1)) << "vsync was dropped";
  EXPECT_EQ(2, saved.getIntValue("system", "run_tier", 0))
      << "run_tier was dropped";
  EXPECT_EQ(2, saved.getIntValue("input", "lightgun", 0))
      << "lightgun was dropped";
  EXPECT_EQ(1, saved.getIntValue("system", "silicon_disc", 0))
      << "silicon_disc was dropped";
  EXPECT_EQ(99, saved.getIntValue("devtools", "max_stack_size", 50))
      << "max_stack_size was dropped";

  // And the settings survive the full boot-edit-boot loop.
  loadConfiguration(CPC[1], getTmpFilename(1));
  EXPECT_EQ(0, CPC[1].scr_vsync);
  EXPECT_EQ(99, CPC[1].devtools_max_stack_size);
  EXPECT_EQ(PhazerType::TrojanLightPhazer,
            static_cast<PhazerType::Value>(CPC[1].phazer_emulation));
}

// Saving over an existing file edits it: a comment and an unrecognised key
// written by hand (or by a newer build) are still there afterwards.
TEST_F(ConfigurationTest, saveConfigurationEditsTheFileInsteadOfReplacingIt) {
  std::ofstream configFile(getTmpFilename(0));
  configFile << "# a hand-written note\n"
             << "[system]\n"
             << "model=2\n"
             << "a_key_from_a_newer_build=keep me\n";
  configFile.close();

  t_CPC CPC;
  loadConfiguration(CPC, getTmpFilename(0));
  saveConfiguration(CPC, getTmpFilename(0));

  std::string const text = readFile(getTmpFilename(0));
  EXPECT_NE(std::string::npos, text.find("# a hand-written note"))
      << "comments were stripped:\n"
      << text;
  EXPECT_NE(std::string::npos, text.find("a_key_from_a_newer_build=keep me"))
      << "an unknown key was deleted:\n"
      << text;
}

// Real unit tests

TEST_F(ConfigurationTest, hasValueReturnsFalseOnNonExistingSystem) {
  ASSERT_FALSE(config::hasValue(configuration_.getConfigMapForTests(), "system",
                                "model"));
}

TEST_F(ConfigurationTest, hasValueReturnsFalseOnNonExistingKey) {
  std::string config = "[system]\nmodel=42";
  configuration_.parseString(config);

  ASSERT_FALSE(config::hasValue(configuration_.getConfigMapForTests(), "system",
                                "something"));
}

TEST_F(ConfigurationTest, hasValueReturnsTrueOnExistingKey) {
  std::string config = "[system]\nmodel=42";
  configuration_.parseString(config);

  ASSERT_TRUE(config::hasValue(configuration_.getConfigMapForTests(), "system",
                               "model"));
}

TEST_F(ConfigurationTest, getIntOnEmptyConfigReturnsDefault) {
  ASSERT_EQ(0, configuration_.getIntValue("system", "model", 0));
  ASSERT_EQ(18, configuration_.getIntValue("system", "model", 18));
}

TEST_F(ConfigurationTest, getIntOnParsedStringConfigReturnsParsedValue) {
  std::string config = "[system]\nmodel=42";
  configuration_.parseString(config);

  ASSERT_EQ(42, configuration_.getIntValue("system", "model", 0));
}

TEST_F(ConfigurationTest, getIntWithValueInMultipleSectionsReturnsTheRightOne) {
  std::string config = "[system]\nmodel=42\n[video]\nmodel=17";
  configuration_.parseString(config);

  ASSERT_EQ(42, configuration_.getIntValue("system", "model", 0));
  ASSERT_EQ(17, configuration_.getIntValue("video", "model", 0));
}

TEST_F(ConfigurationTest, getStringOnEmptyConfigReturnsDefault) {
  ASSERT_EQ("/path/by/default", configuration_.getStringValue(
                                    "system", "path", "/path/by/default"));
  ASSERT_EQ("/other/path/by/default",
            configuration_.getStringValue("system", "path",
                                          "/other/path/by/default"));
}

TEST_F(ConfigurationTest, getStringOnParsedStringConfigReturnsParsedValue) {
  std::string config = "[system]\npath=/path/defined";
  configuration_.parseString(config);

  ASSERT_EQ("/path/defined", configuration_.getStringValue("system", "path",
                                                           "/path/by/default"));
}

TEST_F(ConfigurationTest,
       getStringWithValueInMultipleSectionsReturnsTheRightOne) {
  std::string config = "[system]\npath=/path/system\n[video]\npath=/path/video";
  configuration_.parseString(config);

  ASSERT_EQ("/path/system", configuration_.getStringValue("system", "path",
                                                          "/path/by/default"));
  ASSERT_EQ("/path/video",
            configuration_.getStringValue("video", "path", "/path/by/default"));
}

TEST_F(ConfigurationTest, setStringValueModifiesValue) {
  std::string config = "[system]\npath=/path/original";
  configuration_.parseString(config);
  ASSERT_EQ("/path/original",
            configuration_.getStringValue("system", "path", "/path/default"));

  configuration_.setStringValue("system", "path", "/path/modified");
  ASSERT_EQ("/path/modified",
            configuration_.getStringValue("system", "path", "/path/default"));
}

TEST_F(ConfigurationTest, setIntValueModifiesValue) {
  std::string config = "[system]\nmodel=17";
  configuration_.parseString(config);
  ASSERT_EQ(17, configuration_.getIntValue("system", "model", 0));

  configuration_.setIntValue("system", "model", 42);
  ASSERT_EQ(42, configuration_.getIntValue("system", "model", 0));
}

// beads-iorb: a -O override is one-run intent. At save time the live state
// echoes the override's value back through the setters; persisting that echo
// turns a temporary flag into permanent config (a scratch-dir m4_sd_path was
// written into a real user's file exactly this way). The echo must not reach
// the written file — while a genuine in-session change to the same key must.
TEST_F(ConfigurationTest, anOverrideEchoedBackAtSaveDoesNotPersist) {
  configuration_.parseString("[peripheral]\nm4_sd_path=\nm4board=0\n");
  config::ConfigMap overrides;
  overrides["peripheral"]["m4_sd_path"] = "/tmp/scratch/m4sd";
  configuration_.setOverrides(overrides);

  // Reads serve the override — that part of the contract is unchanged.
  ASSERT_EQ("/tmp/scratch/m4sd",
            configuration_.getStringValue("peripheral", "m4_sd_path", ""));

  // Save time: the live state hands every current value back, including the
  // override's.
  configuration_.setStringValue("peripheral", "m4_sd_path",
                                "/tmp/scratch/m4sd");
  configuration_.setIntValue("peripheral", "m4board", 0);

  std::ostringstream oss;
  configuration_.toStream(oss);
  EXPECT_EQ(oss.str().find("/tmp/scratch"), std::string::npos)
      << "the one-run -O value was persisted into the file:\n"
      << oss.str();
  EXPECT_NE(oss.str().find("m4_sd_path="), std::string::npos)
      << "the key itself must survive the save";
  // The live view still serves the override after the save.
  EXPECT_EQ("/tmp/scratch/m4sd",
            configuration_.getStringValue("peripheral", "m4_sd_path", ""));
}

TEST_F(ConfigurationTest, aRealChangeToAnOverriddenKeyPersists) {
  configuration_.parseString("[peripheral]\nm4_sd_path=/home/sd\n");
  config::ConfigMap overrides;
  overrides["peripheral"]["m4_sd_path"] = "/tmp/scratch/m4sd";
  configuration_.setOverrides(overrides);

  // The user changes the setting in-session: that is not an echo, and from
  // here on the key belongs to them — including a later save-time echo of
  // the NEW value.
  configuration_.setStringValue("peripheral", "m4_sd_path", "/home/newsd");
  ASSERT_EQ("/home/newsd",
            configuration_.getStringValue("peripheral", "m4_sd_path", ""));

  std::ostringstream oss;
  configuration_.toStream(oss);
  EXPECT_NE(oss.str().find("m4_sd_path=/home/newsd"), std::string::npos)
      << oss.str();

  // And a subsequent save-echo of the user's value keeps persisting it.
  configuration_.setStringValue("peripheral", "m4_sd_path", "/home/newsd");
  std::ostringstream oss2;
  configuration_.toStream(oss2);
  EXPECT_NE(oss2.str().find("m4_sd_path=/home/newsd"), std::string::npos);
}

// A key whose live value is unchanged since load must not be written back:
// the file keeps whatever it holds NOW. Every clean exit used to write the
// whole live state over the file — and a SIGTERM is a clean exit, SDL turns
// it into SDL_EVENT_QUIT — so a stale kbd_layout was re-persisted over a hand
// fix the moment the running instance was killed.
TEST_F(ConfigurationTest, anUnchangedValueDoesNotOverwriteAHandEdit) {
  // The file as it is at save time: edited by hand while the emulator ran.
  configuration_.parseString("[control]\nkbd_layout=keymap_us.map\n");
  // What this session loaded at boot.
  config::ConfigMap loaded;
  loaded["control"]["kbd_layout"] = "keymap_es_linux.map";
  configuration_.setBaseline(loaded);

  // Save time: live state echoes the loaded value back, unchanged.
  configuration_.setStringValue("control", "kbd_layout", "keymap_es_linux.map");

  std::ostringstream oss;
  configuration_.toStream(oss);
  EXPECT_NE(oss.str().find("kbd_layout=keymap_us.map"), std::string::npos)
      << "the hand edit was overwritten by the stale live value:\n"
      << oss.str();
  EXPECT_EQ(oss.str().find("keymap_es_linux"), std::string::npos);
}

TEST_F(ConfigurationTest, aValueChangedSinceLoadPersists) {
  configuration_.parseString("[control]\nkbd_layout=keymap_es_linux.map\n");
  config::ConfigMap loaded;
  loaded["control"]["kbd_layout"] = "keymap_es_linux.map";
  configuration_.setBaseline(loaded);

  configuration_.setStringValue("control", "kbd_layout", "keymap_fr_win.map");

  std::ostringstream oss;
  configuration_.toStream(oss);
  EXPECT_NE(oss.str().find("kbd_layout=keymap_fr_win.map"), std::string::npos)
      << oss.str();
}

// An unchanged value is still written when the target file lacks the key:
// older files must keep gaining the keys a newer build reads (see
// saveConfigurationPreservesEverySettingItReads), and a save to a different
// file must be complete.
TEST_F(ConfigurationTest, anUnchangedValueIsWrittenWhenTheFileLacksTheKey) {
  configuration_.parseString("[control]\n");
  config::ConfigMap loaded;
  loaded["control"]["kbd_layout"] = "keymap_us.map";
  configuration_.setBaseline(loaded);

  configuration_.setStringValue("control", "kbd_layout", "keymap_us.map");

  std::ostringstream oss;
  configuration_.toStream(oss);
  EXPECT_NE(oss.str().find("kbd_layout=keymap_us.map"), std::string::npos)
      << oss.str();
}

// End to end through loadConfiguration/saveConfiguration: boot with one
// value, fix the file by hand while "running", save on exit — the fix
// survives.
TEST_F(ConfigurationTest, saveOnExitKeepsAHandEditMadeWhileRunning) {
  {
    std::ofstream f(getTmpFilename(0));
    f << "[control]\nkbd_layout=keymap_es_linux.map\n";
  }
  t_CPC CPC;
  loadConfiguration(CPC, getTmpFilename(0));
  ASSERT_EQ("keymap_es_linux.map", CPC.kbd_layout);

  {
    std::ofstream f(getTmpFilename(0));
    f << "[control]\nkbd_layout=keymap_us.map\n";
  }
  saveConfiguration(CPC, getTmpFilename(0));

  config::Config saved;
  saved.parseFile(getTmpFilename(0));
  EXPECT_EQ("keymap_us.map", saved.getStringValue("control", "kbd_layout", ""));
}

// A value persisted by one save is the reference for the next. Without this,
// change -> save -> revert -> save loses the revert: the reverted value equals
// the boot baseline, so the second save skips it and the file keeps the
// intermediate value. The MRU auto-save on every file open makes such an
// intermediate save routine.
TEST_F(ConfigurationTest, aPersistedValueBecomesTheBaselineForTheNextSave) {
  configuration_.parseString("[system]\nlimit_speed=1\nmodel=3\n");
  config::ConfigMap loaded;
  loaded["system"]["limit_speed"] = "1";
  loaded["system"]["model"] = "2";  // the file was hand-edited to 3 meanwhile
  configuration_.setBaseline(loaded);

  configuration_.setIntValue("system", "limit_speed", 0);  // a real change
  configuration_.setIntValue("system", "model", 2);        // unchanged: skipped

  const config::ConfigMap& next = configuration_.baseline();
  EXPECT_EQ("0", next.at("system").at("limit_speed"))
      << "a written value must become the next baseline";
  EXPECT_EQ("2", next.at("system").at("model"))
      << "a skipped key keeps its loaded baseline, not the file's hand edit";
}

// ─── Which file wins ─────────────────────────────────────────────────────
// The user's profile config outranks a koncepcja.cfg in the working
// directory: a debug-style build run from a source checkout used the
// checkout's untracked file — stale for months, invisible from inside the
// app — ahead of the config the user maintains (beads-825s).

namespace {
void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}
void unset_env(const char* name) {
#ifdef _WIN32
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}
std::string env_or_empty(const char* name) {
  const char* v = getenv(name);
  return v ? v : "";
}
void write_file(const std::filesystem::path& p, const char* text) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream f(p);
  f << text;
}
}  // namespace

class ConfigLookupTest : public testing::Test {
 protected:
  void SetUp() override {
    // A per-test sandbox (two test binaries may run at once: see the
    // concurrent-test-runner note in the project memory).
    root_ = std::filesystem::temp_directory_path() /
            ("koncepcja-cfg-lookup-" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(root_ / "cwd");
    std::filesystem::create_directories(root_ / "home");
    std::filesystem::create_directories(root_ / "xdg");
    saved_home_ = env_or_empty("HOME");
    saved_xdg_ = env_or_empty("XDG_CONFIG_HOME");
    had_xdg_ = getenv("XDG_CONFIG_HOME") != nullptr;
    snprintf(saved_app_path_, sizeof(saved_app_path_), "%s", chAppPath);
    // Point every candidate at the sandbox so nothing on the machine leaks in.
    set_env("HOME", (root_ / "home").string());
    set_env("XDG_CONFIG_HOME", (root_ / "xdg").string());
    snprintf(chAppPath, sizeof(chAppPath), "%s",
             (root_ / "cwd").string().c_str());
  }
  void TearDown() override {
    snprintf(chAppPath, sizeof(chAppPath), "%s", saved_app_path_);
    set_env("HOME", saved_home_);
    if (had_xdg_) {
      set_env("XDG_CONFIG_HOME", saved_xdg_);
    } else {
      unset_env("XDG_CONFIG_HOME");
    }
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
  std::string saved_home_;
  std::string saved_xdg_;
  bool had_xdg_ = false;
  char saved_app_path_[_MAX_PATH + 1] = {};
};

TEST_F(ConfigLookupTest, TheProfileConfigOutranksTheWorkingDirectory) {
  write_file(root_ / "cwd" / "koncepcja.cfg", "[system]\nmodel=0\n");
  write_file(root_ / "xdg" / "koncepcja" / "koncepcja.cfg",
             "[system]\nmodel=2\n");

  EXPECT_EQ((root_ / "xdg" / "koncepcja" / "koncepcja.cfg").string(),
            getConfigurationFilename());
}

TEST_F(ConfigLookupTest, TheHomeProfileConfigAlsoOutranksTheWorkingDirectory) {
  write_file(root_ / "cwd" / "koncepcja.cfg", "[system]\nmodel=0\n");
  write_file(root_ / "home" / ".config" / "koncepcja" / "koncepcja.cfg",
             "[system]\nmodel=2\n");

  EXPECT_EQ(
      (root_ / "home" / ".config" / "koncepcja" / "koncepcja.cfg").string(),
      getConfigurationFilename());
}

TEST_F(ConfigLookupTest, TheWorkingDirectoryIsTheFallbackWithoutAProfile) {
  write_file(root_ / "cwd" / "koncepcja.cfg", "[system]\nmodel=0\n");

  EXPECT_EQ((root_ / "cwd" / "koncepcja.cfg").string(),
            getConfigurationFilename());
}

TEST_F(ConfigLookupTest, LoadRecordsTheFileForTheAppToShow) {
  write_file(root_ / "cwd" / "koncepcja.cfg", "[system]\nmodel=0\n");
  t_CPC CPC;
  loadConfiguration(CPC, (root_ / "cwd" / "koncepcja.cfg").string());
  EXPECT_EQ((root_ / "cwd" / "koncepcja.cfg").string(), koncpc_config_file());
}

// The baseline advances to what was actually persisted — only after a save
// that succeeded.  Advancing it on a failed save would make the change look
// 'unchanged' to the next save, which would then leave the file's old value.
TEST_F(ConfigurationTest, aFailedSaveDoesNotAdvanceTheBaseline) {
  {
    std::ofstream f(getTmpFilename(0));
    f << "[system]\nmodel=2\n";
  }
  t_CPC CPC;
  loadConfiguration(CPC, getTmpFilename(0));
  ASSERT_EQ(2, CPC.model);

  CPC.model = 3;
  std::filesystem::path const nowhere = std::filesystem::temp_directory_path() /
                                        "koncepcja-no-such-dir" /
                                        "koncepcja.cfg";
  ASSERT_FALSE(saveConfiguration(CPC, nowhere.string()));
  ASSERT_TRUE(saveConfiguration(CPC, getTmpFilename(0)));

  config::Config saved;
  saved.parseFile(getTmpFilename(0));
  EXPECT_EQ(3, saved.getIntValue("system", "model", -1))
      << "the failed save must not have made the change look 'unchanged'";
}

TEST_F(ConfigurationTest, changeSaveRevertSavePersistsTheRevert) {
  {
    std::ofstream f(getTmpFilename(0));
    f << "[system]\nmodel=2\n";
  }
  t_CPC CPC;
  loadConfiguration(CPC, getTmpFilename(0));
  ASSERT_EQ(2, CPC.model);

  CPC.model = 3;
  ASSERT_TRUE(saveConfiguration(CPC, getTmpFilename(0)));
  CPC.model = 2;
  ASSERT_TRUE(saveConfiguration(CPC, getTmpFilename(0)));

  config::Config saved;
  saved.parseFile(getTmpFilename(0));
  EXPECT_EQ(2, saved.getIntValue("system", "model", -1))
      << "the revert to the loaded value was mistaken for 'unchanged' and lost";
}

// The baseline guard skips only a key whose live value still EQUALS the loaded
// one. A live fullscreen toggle flips scr_window, so it is kept out of the
// file solely by koncpc_save_configuration_preserving_intent() swapping the
// load-time intent back in before the save. Pin that contract: with the swap
// the file keeps 1; without it the toggle persists.
TEST_F(ConfigurationTest,
       liveFullscreenToggleIsKeptOutOfTheFileOnlyByTheIntentSwap) {
  {
    std::ofstream f(getTmpFilename(0));
    f << "[video]\nscr_window=1\n";
  }
  t_CPC CPC;
  loadConfiguration(CPC, getTmpFilename(0));
  ASSERT_EQ(1u, CPC.scr_window);
  unsigned int const intent = CPC.scr_window;

  CPC.scr_window = 0;  // a live fullscreen toggle
  // What the preserving-intent wrapper does around the save:
  CPC.scr_window = intent;
  ASSERT_TRUE(saveConfiguration(CPC, getTmpFilename(0)));
  CPC.scr_window = 0;
  {
    config::Config saved;
    saved.parseFile(getTmpFilename(0));
    EXPECT_EQ(1, saved.getIntValue("video", "scr_window", -1));
  }

  // Without the swap the baseline guard alone does NOT protect it.
  ASSERT_TRUE(saveConfiguration(CPC, getTmpFilename(0)));
  config::Config saved;
  saved.parseFile(getTmpFilename(0));
  EXPECT_EQ(0, saved.getIntValue("video", "scr_window", -1));
}

TEST_F(ConfigurationTest, keysWithoutOverridesSaveExactlyAsBefore) {
  configuration_.parseString("[system]\nmodel=2\n");
  config::ConfigMap overrides;
  overrides["peripheral"]["m4board"] = "1";  // unrelated key
  configuration_.setOverrides(overrides);

  configuration_.setIntValue("system", "model", 3);
  std::ostringstream oss;
  configuration_.toStream(oss);
  EXPECT_NE(oss.str().find("model=3"), std::string::npos) << oss.str();
}

TEST_F(ConfigurationTest, toStreamDumpsConfig) {
  std::string config = "[system]\nmodel=1\n";
  configuration_.parseString(config);

  std::ostringstream oss;
  configuration_.toStream(oss);

  ASSERT_EQ(config, oss.str());
}

TEST_F(ConfigurationTest, parseDiskFormatValid2sides4sectors) {
  std::string diskFormat("DSK format label,42,2,4,1,82,229,1,2,3,4,5,6,7,8");

  auto df = parseDiskFormat(diskFormat);
  auto dfString = serializeDiskFormat(df);

  ASSERT_EQ("DSK format label", df.label);
  ASSERT_EQ(42, df.tracks);
  ASSERT_EQ(2, df.sides);
  ASSERT_EQ(4, df.sectors);
  ASSERT_EQ(1, df.sector_size);
  ASSERT_EQ(82, df.gap3_length);
  ASSERT_EQ(229, df.filler_byte);
  ASSERT_EQ(1, df.sector_ids[0][0]);
  ASSERT_EQ(2, df.sector_ids[0][1]);
  ASSERT_EQ(3, df.sector_ids[0][2]);
  ASSERT_EQ(4, df.sector_ids[0][3]);
  ASSERT_EQ(5, df.sector_ids[1][0]);
  ASSERT_EQ(6, df.sector_ids[1][1]);
  ASSERT_EQ(7, df.sector_ids[1][2]);
  ASSERT_EQ(8, df.sector_ids[1][3]);
  ASSERT_EQ(diskFormat, dfString);
}

TEST_F(ConfigurationTest, parseDiskFormatValid1side5sectors) {
  std::string diskFormat("DSK format label,17,1,5,2,31,119,1,2,3,4,5");

  auto df = parseDiskFormat(diskFormat);
  auto dfString = serializeDiskFormat(df);

  ASSERT_EQ("DSK format label", df.label);
  ASSERT_EQ(17, df.tracks);
  ASSERT_EQ(1, df.sides);
  ASSERT_EQ(5, df.sectors);
  ASSERT_EQ(2, df.sector_size);
  ASSERT_EQ(31, df.gap3_length);
  ASSERT_EQ(119, df.filler_byte);
  ASSERT_EQ(1, df.sector_ids[0][0]);
  ASSERT_EQ(2, df.sector_ids[0][1]);
  ASSERT_EQ(3, df.sector_ids[0][2]);
  ASSERT_EQ(4, df.sector_ids[0][3]);
  ASSERT_EQ(5, df.sector_ids[0][4]);
  ASSERT_EQ(diskFormat, dfString);
}

// TODO(cpitrat): Clarify why it behaves like this.
// This test validates existing behaviour. The reason why missing sector is
// accepted and why its default value is the sector's index + 1 is a mystery to
// me.
TEST_F(ConfigurationTest, parseDiskFormatValidMissingSectors) {
  std::string diskFormat("DSK format label,17,2,4,2,31,119,9,10");

  auto df = parseDiskFormat(diskFormat);
  auto dfString = serializeDiskFormat(df);

  ASSERT_EQ("DSK format label", df.label);
  ASSERT_EQ(17, df.tracks);
  ASSERT_EQ(2, df.sides);
  ASSERT_EQ(4, df.sectors);
  ASSERT_EQ(2, df.sector_size);
  ASSERT_EQ(31, df.gap3_length);
  ASSERT_EQ(119, df.filler_byte);
  ASSERT_EQ(9, df.sector_ids[0][0]);
  ASSERT_EQ(10, df.sector_ids[0][1]);
  ASSERT_EQ(3, df.sector_ids[0][2]);
  ASSERT_EQ(4, df.sector_ids[0][3]);
  ASSERT_EQ(1, df.sector_ids[1][0]);
  ASSERT_EQ(2, df.sector_ids[1][1]);
  ASSERT_EQ(3, df.sector_ids[1][2]);
  ASSERT_EQ(4, df.sector_ids[1][3]);
  ASSERT_EQ(diskFormat + ",3,4,1,2,3,4",
            dfString);  // Description is completed with missing sectors
}
