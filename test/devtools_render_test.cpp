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
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "asm_source.h"
#include "data_areas.h"
#include "devtools_ui.h"
#include "disk_file_editor.h"
#include "disk_format.h"
#include "headless_imgui.h"
#include "hw/probe.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "koncepcja.h"
#include "slotshandler.h"
#include "subcycle/machine.h"
#include "subcycle_bridge.h"
#include "symfile.h"
#include "z80_view.h"

extern t_z80regs z80;

extern byte *membank_read[4], *membank_write[4];
extern t_CPC CPC;
extern t_drive driveA;

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

    // Bank 0's READ pointer aims at a separate buffer standing in for the lower
    // ROM overlay, while its WRITE pointer stays on RAM. That asymmetry is the
    // whole point: with both pointing at one buffer (as they first did here)
    // the CPU view and the RAM view are byte-identical, so a memory view that
    // read through the ROM overlay -- the bug this suite exists to cover --
    // would pass every assertion. kRomShadowAddr/kRomByte/kRamByte below give
    // the tests a concrete divergence to assert on.
    rom_.assign(0x4000, 0);
    rom_[kRomShadowAddr] = kRomByte;
    ram_[kRomShadowAddr] = kRamByte;

    for (int i = 0; i < 4; i++) {
      saved_read_[i] = membank_read[i];
      saved_write_[i] = membank_write[i];
      membank_read[i] = ram_.data() + (i * 0x4000);
      membank_write[i] = ram_.data() + (i * 0x4000);
    }
    membank_read[0] = rom_.data();  // lower-ROM overlay over bank 0
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

 public:
  // The address the ROM overlay and RAM disagree on, and the two bytes there.
  // Mirrors the reported case: &1AF1 holds &3E in the 6128 OS ROM while the
  // game stores its lives counter underneath.
  static constexpr word kRomShadowAddr = 0x1AF1;
  static constexpr byte kRomByte = 0x3E;
  static constexpr byte kRamByte = 0x03;

 private:
  std::vector<byte> ram_;
  std::vector<byte> rom_;
  byte* saved_read_[4] = {};
  byte* saved_write_[4] = {};
  std::string saved_resources_;
};

using koncpc_test::HeadlessImGui;

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

// The two memory views must actually disagree where a ROM overlays RAM.
//
// This is the assertion that makes the Memory Hex "CPU view" toggle testable at
// all. Before it, TestRam pointed the read and write banks at one buffer, so
// both views returned the same byte everywhere and the shipped bug -- a toggle
// whose branches called the same function -- passed the whole suite.
TEST_F(DevToolsRenderTest, CpuViewAndRamViewDivergeUnderARomOverlay) {
  EXPECT_EQ(z80_read_mem(TestRam::kRomShadowAddr), TestRam::kRomByte)
      << "CPU view must read through the ROM overlay";
  EXPECT_EQ(z80_read_mem_via_write_bank(TestRam::kRomShadowAddr),
            TestRam::kRamByte)
      << "RAM view must ignore the ROM overlay and show the stored byte";
  EXPECT_NE(z80_read_mem(TestRam::kRomShadowAddr),
            z80_read_mem_via_write_bank(TestRam::kRomShadowAddr))
      << "the two views are indistinguishable, so nothing here can cover the "
         "Memory Hex view toggle";
}

// -----------------------------------------------
// Disassembly: the step group sits in its own menu bar (beads-4i4)
// -----------------------------------------------

namespace {

// Sweep the mouse along the Disassembly menu bar, one frame per position, and
// report whether an item with this label (inside the menu bar's ID scope) ever
// became the hovered item. Hovering proves the button was really submitted in
// that menu bar, where a vertex count could not tell it from the neighbours.
bool disasm_menubar_has_item(HeadlessImGui& gui, DevToolsUI& dt,
                             const char* label) {
  gui.settled_frames([&dt] { dt.render(); });
  ImGuiWindow* win = ImGui::FindWindowByName("Disassembly");
  if (win == nullptr) return false;
  ImRect const bar = win->MenuBarRect();
  ImGuiID const bar_id = ImHashStr("##MenuBar", 0, win->ID);
  ImGuiID const target = ImHashStr(label, 0, bar_id);
  float const y = (bar.Min.y + bar.Max.y) * 0.5f;
  for (float x = bar.Min.x + 1.0f; x < bar.Max.x; x += 4.0f) {
    ImGui::GetIO().AddMousePosEvent(x, y);
    gui.frame([&dt] { dt.render(); });
    if (ImGui::GetCurrentContext()->HoveredId == target) return true;
  }
  return false;
}

// Pins g_emu_paused for the test and puts it back.
class PausedFlag {
 public:
  explicit PausedFlag(bool paused) : saved_(g_emu_paused.load()) {
    g_emu_paused.store(paused);
  }
  PausedFlag(const PausedFlag&) = delete;
  PausedFlag& operator=(const PausedFlag&) = delete;
  PausedFlag(PausedFlag&&) = delete;
  PausedFlag& operator=(PausedFlag&&) = delete;
  ~PausedFlag() { g_emu_paused.store(saved_); }

 private:
  bool saved_;
};

}  // namespace

TEST_F(DevToolsRenderTest, DisassemblyMenuBarCarriesTheStepGroup) {
  PausedFlag const paused(true);
  dt_.toggle_window("disassembly");
  EXPECT_TRUE(disasm_menubar_has_item(gui_, dt_, "In"));
  EXPECT_TRUE(disasm_menubar_has_item(gui_, dt_, "Over"));
  EXPECT_TRUE(disasm_menubar_has_item(gui_, dt_, "Out"));
  EXPECT_TRUE(disasm_menubar_has_item(gui_, dt_, "Run"))
      << "paused: the Run-Pause button offers Run";
}

TEST_F(DevToolsRenderTest, DisassemblyRunPauseFollowsTheMachine) {
  PausedFlag const running(false);
  dt_.toggle_window("disassembly");
  EXPECT_TRUE(disasm_menubar_has_item(gui_, dt_, "Pause"));
  EXPECT_FALSE(disasm_menubar_has_item(gui_, dt_, "Run"));
}

// -----------------------------------------------
// Disc Tools: the Files listing follows a disk swap (beads-p5t)
// -----------------------------------------------

namespace {

// Two discs on the host, one file and two files, to swap between.
class SwapDiscs {
 public:
  SwapDiscs() {
    dir_ = std::filesystem::temp_directory_path() / "koncepcja-disc-tools-swap";
    std::filesystem::create_directories(dir_);
    one_ = write_disc("one.dsk", {"ONE.BIN"});
    two_ = write_disc("two.dsk", {"TWO.BIN", "THREE.BIN"});
  }
  SwapDiscs(const SwapDiscs&) = delete;
  SwapDiscs& operator=(const SwapDiscs&) = delete;
  SwapDiscs(SwapDiscs&&) = delete;
  SwapDiscs& operator=(SwapDiscs&&) = delete;
  ~SwapDiscs() {
    dsk_eject_host(&driveA);
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  const std::string& one() const { return one_; }
  const std::string& two() const { return two_; }

 private:
  std::string write_disc(const char* name,
                         const std::vector<std::string>& files) {
    dsk_eject_host(&driveA);
    EXPECT_EQ("", disk_format_drive('A', "data"));
    for (const std::string& f : files) {
      EXPECT_EQ("", disk_write_file(&driveA, f, {'x', 'y'}, true));
    }
    std::string const path = (dir_ / name).string();
    EXPECT_EQ(0, dsk_save(path, &driveA));
    dsk_eject_host(&driveA);
    return path;
  }

  std::filesystem::path dir_;
  std::string one_;
  std::string two_;
};

}  // namespace

TEST_F(DevToolsRenderTest, DiscToolsListingFollowsADiskSwap) {
  SwapDiscs const discs;
  dt_.toggle_window("disc_tools");

  ASSERT_EQ(0, dsk_load(discs.one(), &driveA));
  gui_.settled_frames([this] { dt_.render(); });
  ASSERT_EQ(1u, dt_.disc_tools_listed_file_count());

  // The user rebuilds and re-mounts: no Refresh click.
  ASSERT_EQ(0, dsk_load(discs.two(), &driveA));
  gui_.settled_frames([this] { dt_.render(); });
  EXPECT_EQ(2u, dt_.disc_tools_listed_file_count())
      << "Disc Tools still lists the disc that was swapped out";

  dsk_eject(&driveA);
  gui_.settled_frames([this] { dt_.render(); });
  EXPECT_EQ(0u, dt_.disc_tools_listed_file_count())
      << "an ejected drive still shows the old listing";
}

namespace {

// Brings the sub-cycle board up on a synthetic 32K system ROM, so Disc Tools
// takes the board path: its pull applies the swap that a load queued while the
// machine is paused, which moves the medium generation a second time.
class LiveBoard {
 public:
  // `rom` is the synthetic 32K system ROM (default: all NOPs).
  explicit LiveBoard(std::vector<char> const& rom = std::vector<char>(0x8000,
                                                                      0)) {
    dir_ =
        std::filesystem::temp_directory_path() / "koncepcja-disc-tools-board";
    std::filesystem::create_directories(dir_);
    std::filesystem::path const rom_file = dir_ / "cpc6128.rom";
    FILE* f = fopen(rom_file.string().c_str(), "wb");
    EXPECT_NE(nullptr, f);
    if (f != nullptr) {
      EXPECT_EQ(rom.size(), fwrite(rom.data(), 1, rom.size(), f));
      EXPECT_EQ(0, fclose(f));
    }
    saved_rom_path_ = CPC.rom_path;
    saved_model_ = CPC.model;
    saved_ram_ = CPC.ram_size;
    CPC.rom_path = dir_.string();
    CPC.model = 2;  // chROMFile[2] == "cpc6128.rom"
    CPC.ram_size = 128;
    started_ = subcycle_bridge_start();
    EXPECT_TRUE(started_);
  }
  LiveBoard(const LiveBoard&) = delete;
  LiveBoard& operator=(const LiveBoard&) = delete;
  LiveBoard(LiveBoard&&) = delete;
  LiveBoard& operator=(LiveBoard&&) = delete;
  ~LiveBoard() {
    if (started_) subcycle_bridge_stop();
    CPC.rom_path = saved_rom_path_;
    CPC.model = saved_model_;
    CPC.ram_size = saved_ram_;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

 private:
  std::filesystem::path dir_;
  std::string saved_rom_path_;
  unsigned int saved_model_ = 0;
  unsigned int saved_ram_ = 0;
  bool started_ = false;
};

void load_drive_a(const std::string& path) {
  t_slot slot{};
  slot.drive = DRIVE::DSK_A;
  slot.file = path;
  ASSERT_EQ(0, file_load(slot));
}

}  // namespace

TEST_F(DevToolsRenderTest, DiscToolsRelistsOncePerSwapWithTheBoardRunning) {
  SwapDiscs const discs;
  LiveBoard const board;
  dt_.toggle_window("disc_tools");

  load_drive_a(discs.one());
  gui_.settled_frames([this] { dt_.render(); });
  ASSERT_EQ(1u, dt_.disc_tools_listed_file_count());
  uint64_t const after_first = dt_.disc_tools_listing_rebuilds();

  load_drive_a(discs.two());
  gui_.settled_frames([this] { dt_.render(); });
  EXPECT_EQ(2u, dt_.disc_tools_listed_file_count());
  EXPECT_EQ(after_first + 1, dt_.disc_tools_listing_rebuilds())
      << "the listing was keyed on the pre-pull generation, so the swap the "
         "pull applied made it stale again: two directory walks and two "
         "pause/resume cycles for one disc";
}

// -----------------------------------------------
// Registers window: a paused render never writes to the machine (beads-vwwq)
// -----------------------------------------------

TEST_F(DevToolsRenderTest, PausedRegistersWindowLeavesTheMachineAlone) {
  // Every ROM byte is PUSH BC: 11 T-states, SP drops by 2 per instruction. SP
  // comes out of reset at 0xFFFF, so it stays odd as long as every PUSH runs
  // whole. A frame ends wherever the frame boundary falls, which is almost
  // always mid-instruction, and the window then renders against that parked
  // CPU; the check runs once that instruction has finished. The window used
  // to push the host register mirror on every paused frame, and z80_poke()
  // restarts the CPU at a fresh instruction boundary, so a PUSH cut between
  // its two SP decrements left SP even. That is the half-done PUSH BC in the
  // firmware kernel that corrupted the keyboard read (beads-vwwq).
  std::vector<char> rom(0x8000, static_cast<char>(0xC5));  // PUSH BC ...
  rom[0x3FFD] = static_cast<char>(0xC3);  // ... JP &0000: loop for ever,
  rom[0x3FFE] = 0;                        // never touching SP
  rom[0x3FFF] = 0;
  LiveBoard const board(rom);
  subcycle::Machine* m = subcycle_bridge_machine();
  ASSERT_NE(nullptr, m);
  m->set_run_tier(subcycle::Machine::RunTier::Faithful);
  // The window unlocks its editors on CPC.paused; pin both pause flags.
  PausedFlag const paused(true);
  bool const saved_cpc_paused = CPC.paused;
  CPC.paused = true;
  dt_.toggle_window("registers");

  for (int frame = 0; frame < 20; ++frame) {
    m->run_frame();
    ASSERT_EQ(0x0000u, m->regs().pc & 0xC000u)
        << "the CPU left the PUSH BC ROM; the test no longer measures anything";
    gui_.settled_frames([this] { dt_.render(); });
    // The frame may have parked between the two SP decrements, where an even
    // SP is honest. Let the in-flight instruction finish before judging.
    m->step_instruction();
    ASSERT_EQ(1, m->regs().sp & 1) << "frame " << frame
                                   << ": SP went even -- rendering the paused "
                                      "Registers window cut a PUSH BC in half";
  }
  CPC.paused = saved_cpc_paused;
}

// -----------------------------------------------
// Register edits never drop or replay the instruction in flight (beads-3yl2)
// -----------------------------------------------

namespace {

// A ROM of PUSH BC (SP drops by 2 per instruction, so from reset's 0xFFFF it
// stays odd while every PUSH runs whole), ending in JP &0000.
std::vector<char> push_bc_rom() {
  std::vector<char> rom(0x8000, static_cast<char>(0xC5));
  rom[0x3FFD] = static_cast<char>(0xC3);
  rom[0x3FFE] = 0;
  rom[0x3FFF] = 0;
  return rom;
}

// Runs frames until one parks between a PUSH's two SP decrements (SP even),
// then publishes the registers the way the per-frame debug sync does.
bool park_mid_push(subcycle::Machine& m) {
  for (int frame = 0; frame < 400; ++frame) {
    m.run_frame();
    if ((m.regs().sp & 1) == 0) {
      subcycle_bridge_sync_regs_view();
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(RegisterEditTest, EditMidInstructionLetsThePushFinish) {
  LiveBoard const board(push_bc_rom());
  subcycle::Machine* m = subcycle_bridge_machine();
  ASSERT_NE(nullptr, m);
  m->set_run_tier(subcycle::Machine::RunTier::Faithful);
  ASSERT_TRUE(park_mid_push(*m)) << "no frame ended between two SP decrements";

  // The DevTools field / IPC `reg set DE` path: edit the host view, push it.
  z80.DE.w.l = 0x1234;
  subcycle_bridge_regs_to_machine();
  EXPECT_EQ(0x1234, m->regs().de) << "the edit never reached the machine";
  EXPECT_EQ(0x1234, z80.DE.w.l) << "the view lost the edit";

  // Let whatever is in flight end, then judge: an edit that restarted the CPU
  // at a fresh boundary dropped the PUSH's second half and left SP even.
  m->step_instruction();
  EXPECT_EQ(1, m->regs().sp & 1)
      << "SP went even -- editing DE cut the PUSH BC in flight in half";
  EXPECT_EQ(0x1234, m->regs().de);
}

TEST(RegisterEditTest, PcEditMidInstructionRedirectsAfterThePushEnds) {
  LiveBoard const board(push_bc_rom());
  subcycle::Machine* m = subcycle_bridge_machine();
  ASSERT_NE(nullptr, m);
  m->set_run_tier(subcycle::Machine::RunTier::Faithful);
  ASSERT_TRUE(park_mid_push(*m)) << "no frame ended between two SP decrements";

  // "Set PC here" / `reg set PC`: the next instruction comes from the new PC,
  // and the half-done PUSH still completes first.
  z80.PC.w.l = 0x0100;
  subcycle_bridge_regs_to_machine();
  EXPECT_EQ(0x0100, m->regs().pc);
  EXPECT_EQ(0x0100, z80.PC.w.l);
  EXPECT_EQ(1, m->regs().sp & 1)
      << "SP is even -- the redirect dropped the second half of the PUSH";
  m->step_instruction();
  EXPECT_EQ(0x0101, m->regs().pc) << "the PUSH at the new PC did not run";
  EXPECT_EQ(1, m->regs().sp & 1);
}

namespace {

// Runs until the probe stops the machine on the exec breakpoint at `addr`,
// then lets the debug sync judge the hit and publish the halted PC.
bool stop_on_breakpoint(subcycle::Machine& m, uint16_t addr) {
  probe_add_exec(m.probe(), addr);
  for (int frame = 0; frame < 20; ++frame) {
    m.run_frame();
    ProbeHit hit{};
    if (m.probe_hit(&hit)) return subcycle_bridge_debug_sync() == 1;
  }
  return false;
}

}  // namespace

TEST(RegisterEditTest, EditAtABreakpointRunsTheShownInstructionOnce) {
  LiveBoard const board(push_bc_rom());
  subcycle::Machine* m = subcycle_bridge_machine();
  ASSERT_NE(nullptr, m);
  m->set_run_tier(subcycle::Machine::RunTier::Faithful);
  PausedFlag const paused(true);  // the stop pauses the emulator
  bool const saved_cpc_paused = CPC.paused;
  ASSERT_TRUE(stop_on_breakpoint(*m, 0x0010));
  ASSERT_EQ(0x0010, z80.PC.w.l) << "the debugger shows the breakpoint address";
  Z80Regs const at_stop = m->regs();
  ASSERT_EQ(0x0011, at_stop.pc) << "the machine parks after the opcode fetch";

  z80.DE.w.l = 0xBEEF;
  subcycle_bridge_regs_to_machine();
  EXPECT_EQ(0xBEEF, m->regs().de);
  EXPECT_EQ(0x0010, z80.PC.w.l) << "the edit lost the halted instruction's PC";
  EXPECT_EQ(at_stop.pc, m->regs().pc)
      << "the edit restarted the instruction at the breakpoint";

  // Resuming carries on with the PUSH that was fetched, with no second fetch,
  // and must not stop on the same breakpoint again: the ROM loops back to
  // &0010 only every ~3 frames, so a whole frame runs without a hit.
  uint64_t const fetched = m->regs().instr_count;
  m->step_instruction();
  EXPECT_EQ(0x0011, m->regs().pc);
  EXPECT_EQ(static_cast<uint16_t>(at_stop.sp - 2), m->regs().sp)
      << "the PUSH at the breakpoint did not run exactly once";
  EXPECT_EQ(fetched + 1, m->regs().instr_count);
  m->run_frame();
  ProbeHit hit{};
  EXPECT_FALSE(m->probe_hit(&hit))
      << "the same breakpoint tripped again right after the edit (hit at &"
      << std::hex << hit.addr << ")";
  probe_clear_exec(m->probe());
  CPC.paused = saved_cpc_paused;
}

TEST(RegisterEditTest, PcEditAtABreakpointDropsTheUnrunInstruction) {
  LiveBoard const board(push_bc_rom());
  subcycle::Machine* m = subcycle_bridge_machine();
  ASSERT_NE(nullptr, m);
  m->set_run_tier(subcycle::Machine::RunTier::Faithful);
  PausedFlag const paused(true);
  bool const saved_cpc_paused = CPC.paused;
  ASSERT_TRUE(stop_on_breakpoint(*m, 0x0010));
  probe_clear_exec(m->probe());
  Z80Regs const at_stop = m->regs();

  // The PUSH at the breakpoint has only been fetched; jumping away from it
  // must not run it.
  z80.PC.w.l = 0x0200;
  subcycle_bridge_regs_to_machine();
  EXPECT_EQ(0x0200, m->regs().pc);
  EXPECT_EQ(0x0200, z80.PC.w.l);
  EXPECT_EQ(at_stop.sp, m->regs().sp) << "the PUSH at the breakpoint ran";
  m->step_instruction();
  EXPECT_EQ(0x0201, m->regs().pc);
  EXPECT_EQ(static_cast<uint16_t>(at_stop.sp - 2), m->regs().sp)
      << "the PUSH at the new PC did not run whole";
  CPC.paused = saved_cpc_paused;
}

// The Assembler window mirrors the host-owned source (beads-cv2.4).  Opening
// it loads the text without writing it back, and a later IPC write is picked
// up the same way: an echo would bump the generation and could clobber a
// concurrent IPC 'asm text'.
TEST_F(DevToolsRenderTest, AssemblerMirrorsTheHostSourceWithoutEchoingIt) {
  auto const before = g_asm_source.set("org &4000\nld a,1\nret\n");
  dt_.toggle_window("assembler");

  int const vtx = gui_.settled_frames([this] { dt_.render(); });
  EXPECT_GT(vtx, 0);
  EXPECT_EQ(before, g_asm_source.generation())
      << "loading the text into the editor wrote it back";
  EXPECT_EQ("org &4000\nld a,1\nret\n", g_asm_source.text());

  auto const ipc = g_asm_source.set("nop\n");
  gui_.settled_frames([this] { dt_.render(); });
  EXPECT_EQ(ipc, g_asm_source.generation());
  EXPECT_EQ("nop\n", g_asm_source.text());
  g_asm_source.set("");
}
