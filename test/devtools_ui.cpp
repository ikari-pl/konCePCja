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
    "stack", "breakpoints", "symbols"
  };
  for (const char* name : names) {
    EXPECT_FALSE(dt.is_window_open(name)) << "Window " << name << " should start closed";
    EXPECT_NE(dt.window_ptr(name), nullptr) << "Window " << name << " should have valid ptr";
    dt.toggle_window(name);
    EXPECT_TRUE(dt.is_window_open(name)) << "Window " << name << " should be open after toggle";
  }
  EXPECT_TRUE(dt.any_window_open());
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
