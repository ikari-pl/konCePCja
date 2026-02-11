#include <gtest/gtest.h>
#include "command_palette.h"
#include "search_engine.h"
#include <string>
#include <vector>

namespace {

class CommandPaletteTest : public testing::Test {
protected:
  void SetUp() override {
    palette_.clear_commands();
    palette_.register_command("Pause", "Pause emulation", "F5", []() {});
    palette_.register_command("Reset", "Reset the CPC", "F5", []() {});
    palette_.register_command("DevTools", "Open developer tools", "Shift+F2", []() {});
    palette_.register_command("Fullscreen", "Toggle fullscreen mode", "F2", []() {});
    palette_.register_command("Screenshot", "Take a screenshot", "F3", []() {});
  }

  CommandPalette palette_;
};

TEST_F(CommandPaletteTest, FuzzyMatchExactPrefixScoresHighest) {
  auto results = palette_.filter_commands("pause");
  ASSERT_GE(results.size(), 1u);
  EXPECT_EQ(results[0]->name, "Pause");
}

TEST_F(CommandPaletteTest, FuzzyMatchSubstringScoresLower) {
  auto results = palette_.filter_commands("dev");
  ASSERT_GE(results.size(), 1u);
  EXPECT_EQ(results[0]->name, "DevTools");
}

TEST_F(CommandPaletteTest, FuzzyMatchNoMatchReturnsEmpty) {
  auto results = palette_.filter_commands("xyznonexistent");
  EXPECT_TRUE(results.empty());
}

TEST_F(CommandPaletteTest, CommandRegistrationAndLookup) {
  EXPECT_EQ(palette_.commands().size(), 5u);
  EXPECT_EQ(palette_.commands()[0].name, "Pause");
  EXPECT_EQ(palette_.commands()[2].name, "DevTools");
}

TEST_F(CommandPaletteTest, FilterCommandsByQuery) {
  auto results = palette_.filter_commands("screen");
  ASSERT_GE(results.size(), 2u);
  // Both "Fullscreen" and "Screenshot" should match
  bool has_fullscreen = false, has_screenshot = false;
  for (const auto* cmd : results) {
    if (cmd->name == "Fullscreen") has_fullscreen = true;
    if (cmd->name == "Screenshot") has_screenshot = true;
  }
  EXPECT_TRUE(has_fullscreen);
  EXPECT_TRUE(has_screenshot);
}

TEST_F(CommandPaletteTest, IpcModeSendsCommandAndReturnsResponse) {
  palette_.set_ipc_handler([](const std::string& cmd) -> std::string {
    if (cmd == "ping") return "OK pong\n";
    return "ERR unknown\n";
  });
  EXPECT_EQ(palette_.execute_ipc("ping"), "OK pong\n");
  EXPECT_EQ(palette_.execute_ipc("bad"), "ERR unknown\n");
}

} // namespace
