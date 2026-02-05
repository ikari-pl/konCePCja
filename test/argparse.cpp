#include <gtest/gtest.h>

#include "koncepcja.h"
#include "argparse.h"
#include "keyboard.h"
#include <string>

TEST(ArgParseTest, parseArgsNoArg)
{
   const char *argv[] = {"./koncepcja"};
   CapriceArgs args;
   std::vector<std::string> slot_list;

   parseArguments(1, const_cast<char **>(argv), slot_list, args);

   ASSERT_EQ(0, slot_list.size());
}

TEST(ArgParseTest, parseArgsOneArg)
{
   const char *argv[] = {"./koncepcja", "./foo.dsk"};
   CapriceArgs args;
   std::vector<std::string> slot_list;

   parseArguments(2, const_cast<char **>(argv), slot_list, args);

   ASSERT_EQ(1, slot_list.size());
   ASSERT_EQ("./foo.dsk", slot_list.at(0));
}

TEST(ArgParseTest, parseArgsSeveralArgs)
{
   const char *argv[] = {"./koncepcja", "./foo.dsk", "bar.zip", "0", "__"};
   CapriceArgs args;
   std::vector<std::string> slot_list;

   parseArguments(5, const_cast<char **>(argv), slot_list, args);

   ASSERT_EQ(4, slot_list.size());
   for (int i=1; i < 5; i++)
	   ASSERT_EQ(argv[i], slot_list.at(i-1));
}

TEST(argParseTest, cfgFileArgsSwitch)
{
   const char *argv[] = {"./koncepcja", "--cfg_file=/home/koncepcja/koncepcja.cfg"};
   CapriceArgs args;
   std::vector<std::string> slot_list;

   parseArguments(2, const_cast<char **>(argv), slot_list, args);
   ASSERT_EQ("/home/koncepcja/koncepcja.cfg", args.cfgFilePath);
}

TEST(argParseTest, cfgOverrideValid)
{
   const char *argv[] = {"./koncepcja", "--override=system.model=3", "--override=control.kbd_layout=keymap_us.map", "--override=no.value="};
   CapriceArgs args;
   std::vector<std::string> slot_list;

   parseArguments(4, const_cast<char **>(argv), slot_list, args);
   ASSERT_EQ("3", args.cfgOverrides["system"]["model"]);
   ASSERT_EQ("keymap_us.map", args.cfgOverrides["control"]["kbd_layout"]);
   ASSERT_EQ("", args.cfgOverrides["no"]["value"]);
}

TEST(argParseTest, cfgOverrideInvalid)
{
   const char *argv[] = {"./koncepcja", "--override=no.value", "--override=nosection=3", "--override=emptyitem.=3", "--override=.emptysection=3", "--override==nokey"};
   CapriceArgs args;
   std::vector<std::string> slot_list;

   parseArguments(6, const_cast<char **>(argv), slot_list, args);
   ASSERT_TRUE(args.cfgOverrides.empty());
}

TEST(argParseTest, replaceKoncpcKeysNoKeyword)
{
  std::string command = "print \"Hello, world !\"";

  ASSERT_EQ(command, replaceKoncpcKeys(command));
}

TEST(argParseTest, replaceKoncpcKeysKeywords)
{
  // expected
  //   Which is: "print \"Hello, world !\"\f\b\f\0"
  // replaceKoncpcKeys(command)
  //   Which is: "print \"Hello, world !\"\f\t\f\0"
  std::string command = "print \"Hello, world !\"KONCPC_SCRNSHOTKONCPC_EXIT";
  std::string expected = "print \"Hello, world !\"\f\x9\f";
  expected += '\0';

  ASSERT_EQ(expected, replaceKoncpcKeys(command));
}

TEST(argParseTest, replaceKoncpcKeysRepeatedKeywords)
{
  std::string command = "print \"Hello\"KONCPC_SCRNSHOT ; print \",\" ; KONCPC_SCRNSHOT ; print \"world !\" ; KONCPC_SCRNSHOT";
  std::string expected = "print \"Hello\"\f\x9 ; print \",\" ; \f\x9 ; print \"world !\" ; \f\x9";

  ASSERT_EQ(expected, replaceKoncpcKeys(command));
}
