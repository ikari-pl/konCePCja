// konCePCja — headless render tests for the DevTools windows.
//
// WHY THIS FILE EXISTS
//
// test/devtools_ui.cpp covers DevToolsUI's *bookkeeping* only — which show_*
// flag a name maps to, what toggle_window() does. It never renders a frame, so
// every render_*() function had zero coverage.
//
// That gap hid a guaranteed segfault: render_symbols() passed a literal
// nullptr as the ImGui::Selectable label. ImGui hashes and measures the label
// before drawing anything (vendor/imgui/imgui_widgets.cpp:7315-7316), so the
// first symbol row dereferenced NULL — EXC_BAD_ACCESS at 0x0 on the main
// thread. It stayed invisible because the row loop does not execute until a
// symbol exists, so the emulator looked healthy until a user added one.
//
// The lesson generalises: UI defects live in the *populated* render path. A
// window rendered with an empty collection exercises almost none of its code.
// So these tests install real RAM, populate the debugger's tables, and only
// then render.

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "data_areas.h"
#include "devtools_ui.h"
#include "imgui.h"
#include "koncepcja.h"
#include "symfile.h"
#include "z80_view.h"

extern byte *membank_read[4], *membank_write[4];
extern t_CPC CPC;

namespace {

// Installs 64KB of real RAM behind the four 16KB bank pointers.
//
// g_memory_bus is statically wired to the membank_read/membank_write arrays
// (kon_cpc_ja.cpp:518), but in a test binary the individual bank pointers are
// null, so any window that reads emulator memory — disassembly, memory hex,
// stack, GFX finder — dereferences null inside MemoryBus::read_raw. That is a
// harness gap, not an emulator bug: the real app always banks memory in before
// a frame can be drawn.
//
// Saves and restores the previous pointers, because other suites
// (test/z80_disassembly.cpp) install their own small buffers and the runner is
// invoked with --gtest_shuffle.
class TestRam {
 public:
  TestRam() : ram_(0x10000, 0) {
    // Some real Z80 so the disassembler decodes instructions rather than a
    // uniform field of nops: ld a,$10 / dec a / jp nz,$0005 / ret.
    const byte prog[] = {0x3E, 0x10, 0x3D, 0xC2, 0x05, 0x00, 0xC9};
    std::copy(std::begin(prog), std::end(prog), ram_.begin());

    for (int i = 0; i < 4; i++) {
      saved_read_[i] = membank_read[i];
      saved_write_[i] = membank_write[i];
      membank_read[i] = ram_.data() + (i * 0x4000);
      membank_write[i] = ram_.data() + (i * 0x4000);
    }
    saved_resources_ = CPC.resources_path;
    CPC.resources_path = "resources";
  }

  TestRam(const TestRam&) = delete;
  TestRam& operator=(const TestRam&) = delete;
  TestRam(TestRam&&) = delete;
  TestRam& operator=(TestRam&&) = delete;

  ~TestRam() {
    for (int i = 0; i < 4; i++) {
      membank_read[i] = saved_read_[i];
      membank_write[i] = saved_write_[i];
    }
    CPC.resources_path = saved_resources_;
  }

 private:
  std::vector<byte> ram_;
  byte* saved_read_[4] = {};
  byte* saved_write_[4] = {};
  std::string saved_resources_;
};

// A headless ImGui context — no platform or renderer backend at all.
//
// ImGui needs a backend for input and for uploading the font texture, neither
// of which a test cares about. Because we deliberately do NOT set
// ImGuiBackendFlags_RendererHasTextures, ImGui takes its legacy path, so all
// the logic that actually breaks — layout, clipping, ID hashing, label
// handling, draw-list emission — runs exactly as it does in the shipping app.
class HeadlessImGui {
 public:
  HeadlessImGui() {
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    // A test must never read or clobber the developer's real imgui.ini.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();

    // Stand in for the renderer backend: on the legacy path ImGui asserts that
    // the atlas was rasterised by the backend (imgui_draw.cpp:2772).
    unsigned char* pixels = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &tex_w, &tex_h);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));
  }

  HeadlessImGui(const HeadlessImGui&) = delete;
  HeadlessImGui& operator=(const HeadlessImGui&) = delete;
  HeadlessImGui(HeadlessImGui&&) = delete;
  HeadlessImGui& operator=(HeadlessImGui&&) = delete;

  ~HeadlessImGui() { ImGui::DestroyContext(ctx_); }

  // Runs one frame and returns how many vertices it emitted.
  //
  // The vertex count is what keeps these tests honest. An ImGui window that is
  // collapsed, clipped or off-screen silently draws nothing — Selectable() and
  // friends early-return on window->SkipItems before touching their arguments
  // (imgui_widgets.cpp:7308). So "it did not crash" would also pass when no
  // widget ran at all. Asserting vertices were produced proves the code under
  // test actually executed.
  int frame(const std::function<void()>& body) {
    ImGui::NewFrame();
    body();
    ImGui::Render();
    return ImGui::GetDrawData()->TotalVtxCount;
  }

  // ImGui needs a couple of frames to settle a newly created window
  // (appearing, auto-fit, table column widths). Returns the last frame's count.
  int settled_frames(const std::function<void()>& body, int n = 3) {
    int vtx = 0;
    for (int i = 0; i < n; i++) vtx = frame(body);
    return vtx;
  }

 private:
  ImGuiContext* ctx_ = nullptr;
};

// Every window name accepted by DevToolsUI::window_ptr().
const std::vector<std::string>& all_window_names() {
  static const std::vector<std::string> names = {
      "registers",         "disassembly",
      "memory_hex",        "stack",
      "breakpoints",       "symbols",
      "session_recording", "gfx_finder",
      "silicon_disc",      "asic",
      "disc_tools",        "data_areas",
      "disasm_export",     "video_state",
      "audio_state",       "recording_controls",
      "assembler",         "drive_sound_lab"};
  return names;
}

// Base fixture: real RAM, a headless ImGui context, and a private DevToolsUI.
//
// Every render test gets all three, so no test can accidentally render against
// null memory or leak window state into its neighbours.
class DevToolsRenderTest : public ::testing::Test {
 protected:
  void TearDown() override { clear_debug_state(); }

  // Give the debugger windows something to draw. Rendering an empty list
  // exercises almost nothing, which is exactly how the Selectable bug survived.
  static void populate_debug_state() {
    g_symfile.clear();
    g_symfile.addSymbol(0x1AF1, "life");  // François' Fruity Frank symbol
    g_symfile.addSymbol(0x4000, "screen_base");
    g_symfile.addSymbol(0xBB5A, "txt_output");

    z80_clear_breakpoints();
    z80_add_breakpoint(0x0038);
    z80_add_breakpoint(0x4000);

    z80_clear_watchpoints();
    z80_add_watchpoint(0x1AF1, 1, READ);
    z80_add_watchpoint(0xBE80, 2, WRITE);

    g_data_areas.clear_all();
    g_data_areas.mark(0x4000, 0x40FF, DataType::BYTES, "sprite_table");
    g_data_areas.mark(0x5000, 0x50FF, DataType::WORDS, "jump_table");
    g_data_areas.mark(0x6000, 0x603F, DataType::TEXT, "messages");
  }

  static void clear_debug_state() {
    g_symfile.clear();
    z80_clear_breakpoints();
    z80_clear_watchpoints();
    g_data_areas.clear_all();
  }

  TestRam ram_;
  HeadlessImGui gui_;
  DevToolsUI dt_;
};

}  // namespace

// -----------------------------------------------
// Regression test for the crash François reported
// -----------------------------------------------

// A symbol table containing rows must render. Before the fix this segfaulted on
// the first row, every time, on every platform.
TEST_F(DevToolsRenderTest, SymbolsWindowRendersRowsWithoutCrashing) {
  g_symfile.addSymbol(0x1AF1, "life");
  dt_.symtable_mark_dirty();
  dt_.toggle_window("symbols");

  int const vtx = gui_.settled_frames([this] { dt_.render(); });

  EXPECT_GT(vtx, 0)
      << "Symbols window emitted no geometry — the row loop never "
         "ran, so this test would pass even with broken "
         "rendering code.";
}

// Proves the symbol *rows* are what render, not just the window chrome. This is
// the assertion that makes the test above meaningful: if rows were skipped, the
// vertex count would not grow with the number of symbols.
TEST_F(DevToolsRenderTest, SymbolRowsContributeGeometry) {
  dt_.toggle_window("symbols");

  g_symfile.clear();
  dt_.symtable_mark_dirty();
  int const vtx_empty = gui_.settled_frames([this] { dt_.render(); });

  for (int i = 0; i < 16; i++)
    g_symfile.addSymbol(static_cast<word>(0x1000 + (i * 0x10)),
                        "sym_" + std::to_string(i));
  dt_.symtable_mark_dirty();
  int const vtx_rows = gui_.settled_frames([this] { dt_.render(); });

  EXPECT_GT(vtx_rows, vtx_empty)
      << "Adding 16 symbols did not increase emitted geometry (" << vtx_empty
      << " -> " << vtx_rows << "), so the table body is not being drawn and "
      << "this suite is not covering the row path at all.";
}

// Mutating the table between frames is what the row's X button does.
TEST_F(DevToolsRenderTest, SymbolsWindowSurvivesMutationBetweenFrames) {
  dt_.toggle_window("symbols");

  for (int i = 0; i < 8; i++)
    g_symfile.addSymbol(static_cast<word>(0x2000 + (i * 0x10)),
                        "s" + std::to_string(i));
  dt_.symtable_mark_dirty();
  EXPECT_GT(gui_.settled_frames([this] { dt_.render(); }), 0);

  g_symfile.delSymbol("s3");
  dt_.symtable_mark_dirty();
  EXPECT_GT(gui_.settled_frames([this] { dt_.render(); }), 0);

  g_symfile.clear();
  dt_.symtable_mark_dirty();
  gui_.settled_frames([this] { dt_.render(); });
}

// Symbols appear in the breakpoint list too (render_breakpoints looks each
// address up), so a symbol whose name is long or empty must not upset it.
TEST_F(DevToolsRenderTest, BreakpointRowsWithAndWithoutSymbols) {
  z80_clear_breakpoints();
  z80_add_breakpoint(0x0038);  // will have a symbol
  z80_add_breakpoint(0x7FFF);  // will not
  g_symfile.clear();
  g_symfile.addSymbol(0x0038, std::string(120, 'x'));  // overlong label
  dt_.symtable_mark_dirty();

  dt_.toggle_window("breakpoints");
  EXPECT_GT(gui_.settled_frames([this] { dt_.render(); }), 0);
}

// -----------------------------------------------
// Every window, rendered with populated state
// -----------------------------------------------

class DevToolsWindowRender : public DevToolsRenderTest,
                             public ::testing::WithParamInterface<std::string> {
};

// One window per test case, so a crash names the window that caused it instead
// of taking the whole suite down anonymously.
TEST_P(DevToolsWindowRender, RendersWithPopulatedStateWithoutCrashing) {
  populate_debug_state();
  dt_.symtable_mark_dirty();

  dt_.toggle_window(GetParam());
  ASSERT_TRUE(dt_.is_window_open(GetParam()));

  int const vtx = gui_.settled_frames([this] { dt_.render(); });
  EXPECT_GT(vtx, 0) << GetParam() << " emitted no geometry — it is open but "
                    << "drawing nothing, so this case covers nothing.";
}

INSTANTIATE_TEST_SUITE_P(AllWindows, DevToolsWindowRender,
                         ::testing::ValuesIn(all_window_names()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                           return info.param;
                         });

// All windows at once — catches ID collisions and cross-window state clashes
// that per-window tests cannot see.
TEST_F(DevToolsRenderTest, AllWindowsOpenSimultaneously) {
  populate_debug_state();
  dt_.symtable_mark_dirty();
  for (const auto& name : all_window_names()) dt_.toggle_window(name);

  EXPECT_GT(gui_.settled_frames([this] { dt_.render(); }, 4), 0);
}

// A closed window must draw nothing at all — guards the `shown` gate in
// DevToolsUI::render()'s time_window dispatcher.
TEST_F(DevToolsRenderTest, ClosedWindowsEmitNoGeometry) {
  populate_debug_state();
  dt_.close_all_windows();

  int const vtx = gui_.settled_frames([this] { dt_.render(); });
  EXPECT_EQ(vtx, 0) << "DevTools drew " << vtx
                    << " vertices with every window closed.";
}
