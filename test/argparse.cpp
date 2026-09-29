#include "argparse.h"

#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "keyboard.h"
#include "koncepcja.h"

TEST(ArgParseTest, parseArgsNoArg) {
  const char* argv[] = {"./koncepcja"};
  CapriceArgs args;
  std::vector<std::string> slot_list;

  parseArguments(1, const_cast<char**>(argv), slot_list, args);

  ASSERT_EQ(0, slot_list.size());
}

TEST(ArgParseTest, parseArgsOneArg) {
  const char* argv[] = {"./koncepcja", "./foo.dsk"};
  CapriceArgs args;
  std::vector<std::string> slot_list;

  parseArguments(2, const_cast<char**>(argv), slot_list, args);

  ASSERT_EQ(1, slot_list.size());
  ASSERT_EQ("./foo.dsk", slot_list.at(0));
}

TEST(ArgParseTest, parseArgsSeveralArgs) {
  const char* argv[] = {"./koncepcja", "./foo.dsk", "bar.zip", "0", "__"};
  CapriceArgs args;
  std::vector<std::string> slot_list;

  parseArguments(5, const_cast<char**>(argv), slot_list, args);

  ASSERT_EQ(4, slot_list.size());
  for (int i = 1; i < 5; i++) ASSERT_EQ(argv[i], slot_list.at(i - 1));
}

TEST(argParseTest, cfgFileArgsSwitch) {
  const char* argv[] = {"./koncepcja",
                        "--cfg_file=/home/koncepcja/koncepcja.cfg"};
  CapriceArgs args;
  std::vector<std::string> slot_list;

  parseArguments(2, const_cast<char**>(argv), slot_list, args);
  ASSERT_EQ("/home/koncepcja/koncepcja.cfg", args.cfgFilePath);
}

TEST(argParseTest, cfgOverrideValid) {
  const char* argv[] = {"./koncepcja", "--override=system.model=3",
                        "--override=control.kbd_layout=keymap_us.map",
                        "--override=no.value="};
  CapriceArgs args;
  std::vector<std::string> slot_list;

  parseArguments(4, const_cast<char**>(argv), slot_list, args);
  ASSERT_EQ("3", args.cfgOverrides["system"]["model"]);
  ASSERT_EQ("keymap_us.map", args.cfgOverrides["control"]["kbd_layout"]);
  ASSERT_EQ("", args.cfgOverrides["no"]["value"]);
}

TEST(argParseTest, cfgOverrideInvalid) {
  const char* argv[] = {"./koncepcja",
                        "--override=no.value",
                        "--override=nosection=3",
                        "--override=emptyitem.=3",
                        "--override=.emptysection=3",
                        "--override==nokey"};
  CapriceArgs args;
  std::vector<std::string> slot_list;

  parseArguments(6, const_cast<char**>(argv), slot_list, args);
  ASSERT_TRUE(args.cfgOverrides.empty());
}

TEST(argParseTest, replaceKoncpcKeysNoKeyword) {
  std::string command = "print \"Hello, world !\"";

  ASSERT_EQ(command, replaceKoncpcKeys(command));
}

TEST(argParseTest, replaceKoncpcKeysKeywords) {
  // expected
  //   Which is: "print \"Hello, world !\"\f\b\f\0"
  // replaceKoncpcKeys(command)
  //   Which is: "print \"Hello, world !\"\f\t\f\0"
  std::string command = "print \"Hello, world !\"KONCPC_SCRNSHOTKONCPC_EXIT";
  std::string expected = "print \"Hello, world !\"\f\x9\f";
  expected += '\0';

  ASSERT_EQ(expected, replaceKoncpcKeys(command));
}

TEST(argParseTest, replaceKoncpcKeysRepeatedKeywords) {
  std::string command =
      "print \"Hello\"KONCPC_SCRNSHOT ; print \",\" ; KONCPC_SCRNSHOT ; print "
      "\"world !\" ; KONCPC_SCRNSHOT";
  std::string expected =
      "print \"Hello\"\f\x9 ; print \",\" ; \f\x9 ; print \"world !\" ; \f\x9";

  ASSERT_EQ(expected, replaceKoncpcKeys(command));
}

// ---------------------------------------------------------------------------
// AGENTS.md drift guard (beads-5bcr). AGENTS.md (CLAUDE.md is a link to it) is
// the contract agents read first, and its copy of the option list once drifted
// to 9 of 15 flags -- without --headless, while telling agents to use an SDL
// environment-variable workaround instead. These read the file itself.
// ---------------------------------------------------------------------------

namespace {

std::string agents_source_dir() {
#ifdef KONCPC_SOURCE_DIR
  return KONCPC_SOURCE_DIR;
#else
  return ".";
#endif
}

std::string read_doc(const std::string& name) {
  std::ifstream f(agents_source_dir() + "/" + name, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  // A checkout without symlink support stores a link as a file holding its
  // target's name: follow it.
  if (!text.empty() && text.size() < 64 && text.find('\n') == std::string::npos)
    return read_doc(text);
  return text;
}

// The lines of the fenced block's "Options:" list, up to its blank line.
std::vector<std::string> options_block(const std::string& doc) {
  std::vector<std::string> lines;
  std::istringstream in(doc);
  std::string line;
  bool inside = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!inside) {
      inside = line == "Options:";
      continue;
    }
    if (line.empty()) break;
    lines.push_back(line);
  }
  return lines;
}

// Every "-X" and "--long" token at the start of each option line.
void option_tokens(const std::vector<std::string>& lines,
                   std::set<char>& shorts, std::set<std::string>& longs) {
  for (const std::string& line : lines) {
    size_t i = line.find_first_not_of(' ');
    if (i == std::string::npos || line[i] != '-') continue;
    std::string const spec = line.substr(i, line.find(' ', i) - i);
    size_t at = 0;
    while (at < spec.size()) {
      if (spec.compare(at, 2, "--") == 0) {
        size_t end = spec.find_first_of("=/", at + 2);
        if (end == std::string::npos) end = spec.size();
        longs.insert(spec.substr(at + 2, end - at - 2));
        at = end;
      } else if (spec[at] == '-' && at + 1 < spec.size()) {
        shorts.insert(spec[at + 1]);
        at += 2;
      } else {
        ++at;
      }
    }
  }
}

}  // namespace

TEST(AgentsDocDrift, OptionListMatchesTheParserExactly) {
  std::set<char> table_shorts;
  std::set<std::string> table_longs;
  for (const CliOption& o : cli_options()) {
    table_shorts.insert(o.short_name);
    table_longs.insert(std::string(o.long_name));
  }
  for (const char* name : {"AGENTS.md", "CLAUDE.md"}) {
    std::string const doc = read_doc(name);
    ASSERT_FALSE(doc.empty()) << "could not read " << name;
    auto const block = options_block(doc);
    ASSERT_FALSE(block.empty()) << name << " has no Options: block";
    std::set<char> shorts;
    std::set<std::string> longs;
    option_tokens(block, shorts, longs);
    EXPECT_EQ(table_shorts, shorts)
        << name << ": short flags differ from src/argparse.cpp";
    EXPECT_EQ(table_longs, longs)
        << name << ": long options differ from src/argparse.cpp";
  }
}

// The [sound] keys in the config example must be ones loadConfiguration()
// reads. The doc once listed the C++ member names (snd_enabled, ...), which a
// user could copy into a config file and have silently do nothing.
TEST(AgentsDocDrift, DocumentedSoundKeysAreKeysTheConfigLoaderReads) {
  std::string const doc = read_doc("AGENTS.md");
  size_t const start = doc.find("\n[sound]\n");
  ASSERT_NE(std::string::npos, start) << "AGENTS.md has no [sound] example";
  std::set<std::string> documented;
  std::istringstream in(doc.substr(start + 9));
  std::string line;
  while (std::getline(in, line) && !line.empty() && line[0] != '[' &&
         line[0] != '`') {
    if (line[0] == ' ' || line[0] == '#') continue;
    size_t const eq = line.find('=');
    if (eq != std::string::npos) documented.insert(line.substr(0, eq));
  }
  ASSERT_FALSE(documented.empty());

  std::ifstream src(agents_source_dir() + "/src/kon_cpc_ja.cpp");
  std::ostringstream ss;
  ss << src.rdbuf();
  std::string const code = ss.str();
  ASSERT_FALSE(code.empty());
  for (const std::string& key : documented) {
    EXPECT_NE(std::string::npos, code.find("(\"sound\", \"" + key + "\""))
        << "[sound] " << key
        << " is documented in AGENTS.md but loadConfiguration never reads it";
  }
}
