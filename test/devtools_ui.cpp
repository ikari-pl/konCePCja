#include <gtest/gtest.h>
#include "devtools_ui.h"

// -----------------------------------------------
// DevToolsUI toggle/is_open tests
// -----------------------------------------------

TEST(DevToolsUI, InitiallyAllClosed) {
  DevToolsUI dt;
  EXPECT_FALSE(dt.is_window_open("registers"));
  EXPECT_FALSE(dt.is_window_open("disassembly"));
  EXPECT_FALSE(dt.is_window_open("memory_hex"));
  EXPECT_FALSE(dt.is_window_open("stack"));
  EXPECT_FALSE(dt.is_window_open("breakpoints"));
  EXPECT_FALSE(dt.is_window_open("symbols"));
  EXPECT_FALSE(dt.any_window_open());
}

TEST(DevToolsUI, ToggleOpensWindow) {
  DevToolsUI dt;
  dt.toggle_window("registers");
  EXPECT_TRUE(dt.is_window_open("registers"));
  EXPECT_TRUE(dt.any_window_open());
}

TEST(DevToolsUI, ToggleTwiceClosesWindow) {
  DevToolsUI dt;
  dt.toggle_window("disassembly");
  EXPECT_TRUE(dt.is_window_open("disassembly"));
  dt.toggle_window("disassembly");
  EXPECT_FALSE(dt.is_window_open("disassembly"));
  EXPECT_FALSE(dt.any_window_open());
}

TEST(DevToolsUI, MultipleWindowsIndependent) {
  DevToolsUI dt;
  dt.toggle_window("registers");
  dt.toggle_window("stack");
  EXPECT_TRUE(dt.is_window_open("registers"));
  EXPECT_TRUE(dt.is_window_open("stack"));
  EXPECT_FALSE(dt.is_window_open("disassembly"));
  EXPECT_TRUE(dt.any_window_open());

  dt.toggle_window("registers");
  EXPECT_FALSE(dt.is_window_open("registers"));
  EXPECT_TRUE(dt.is_window_open("stack"));
  EXPECT_TRUE(dt.any_window_open());
}

TEST(DevToolsUI, UnknownWindowReturnsFalse) {
  DevToolsUI dt;
  EXPECT_FALSE(dt.is_window_open("nonexistent"));
}

TEST(DevToolsUI, ToggleUnknownWindowIsNoop) {
  DevToolsUI dt;
  dt.toggle_window("nonexistent");
  EXPECT_FALSE(dt.any_window_open());
}

TEST(DevToolsUI, WindowPtrReturnsValidPointer) {
  DevToolsUI dt;
  bool* p = dt.window_ptr("registers");
  ASSERT_NE(p, nullptr);
  EXPECT_FALSE(*p);
  *p = true;
  EXPECT_TRUE(dt.is_window_open("registers"));
}

TEST(DevToolsUI, WindowPtrNullForUnknown) {
  DevToolsUI dt;
  EXPECT_EQ(dt.window_ptr("nonexistent"), nullptr);
}

TEST(DevToolsUI, AllWindowNames) {
  DevToolsUI dt;
  const char* names[] = {
    "registers", "disassembly", "memory_hex",
    "stack", "breakpoints", "symbols",
    "session_recording", "gfx_finder", "silicon_disc",
    "asic", "disc_tools", "data_areas", "disasm_export",
    "video_state", "audio_state"
  };
  for (const char* name : names) {
    EXPECT_FALSE(dt.is_window_open(name)) << "Window " << name << " should start closed";
    EXPECT_NE(dt.window_ptr(name), nullptr) << "Window " << name << " should have valid ptr";
    dt.toggle_window(name);
    EXPECT_TRUE(dt.is_window_open(name)) << "Window " << name << " should be open after toggle";
  }
  EXPECT_TRUE(dt.any_window_open());
}

// -----------------------------------------------
// navigate_to / navigate_memory tests
// -----------------------------------------------

TEST(DevToolsUI, NavigateToDisasmOpensDisassembly) {
  DevToolsUI dt;
  EXPECT_FALSE(dt.is_window_open("disassembly"));
  dt.navigate_to(0x4000, NavTarget::DISASM);
  EXPECT_TRUE(dt.is_window_open("disassembly"));
}

TEST(DevToolsUI, NavigateToMemoryOpensMemoryHex) {
  DevToolsUI dt;
  EXPECT_FALSE(dt.is_window_open("memory_hex"));
  dt.navigate_to(0xBE80, NavTarget::MEMORY);
  EXPECT_TRUE(dt.is_window_open("memory_hex"));
}

TEST(DevToolsUI, NavigateToGfxOpensGfxFinder) {
  DevToolsUI dt;
  EXPECT_FALSE(dt.is_window_open("gfx_finder"));
  dt.navigate_to(0xC000, NavTarget::GFX);
  EXPECT_TRUE(dt.is_window_open("gfx_finder"));
}

TEST(DevToolsUI, NavigateMemoryOpensMemoryHex) {
  DevToolsUI dt;
  EXPECT_FALSE(dt.is_window_open("memory_hex"));
  dt.navigate_memory(0x1234);
  EXPECT_TRUE(dt.is_window_open("memory_hex"));
}

TEST(DevToolsUI, NavigateDisassemblyOpensDisassembly) {
  DevToolsUI dt;
  dt.navigate_disassembly(0x8000);
  EXPECT_TRUE(dt.is_window_open("disassembly"));
}

TEST(DevToolsUI, AnyWindowOpenReflectsState) {
  DevToolsUI dt;
  EXPECT_FALSE(dt.any_window_open());

  dt.toggle_window("symbols");
  EXPECT_TRUE(dt.any_window_open());

  dt.toggle_window("symbols");
  EXPECT_FALSE(dt.any_window_open());

  dt.toggle_window("memory_hex");
  dt.toggle_window("breakpoints");
  EXPECT_TRUE(dt.any_window_open());

  dt.toggle_window("memory_hex");
  EXPECT_TRUE(dt.any_window_open());  // breakpoints still open

  dt.toggle_window("breakpoints");
  EXPECT_FALSE(dt.any_window_open());
}
