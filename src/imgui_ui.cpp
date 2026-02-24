#include "imgui_ui.h"
#include "devtools_ui.h"
#include "imgui_ui_testable.h"
#include "imgui.h"
#include "command_palette.h"
#include "menu_actions.h"
#include "workspace_layout.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <iostream>

#include "koncepcja.h"
#include "crtc.h"
#include "rom_identify.h"
#include "keyboard.h"
#include "z80.h"
#include "z80_disassembly.h"
#include "slotshandler.h"
#include "disk.h"
#include "tape.h"
#include "video.h"
#include "symfile.h"
#include "amdrum.h"
#include "smartwatch.h"
#include "amx_mouse.h"
#include "drive_sounds.h"
#include "symbiface.h"
#include "m4board.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

extern SDL_Window* mainSDLWindow;
extern t_CPC CPC;
extern t_z80regs z80;
extern t_CRTC CRTC;
extern t_GateArray GateArray;
extern t_PSG PSG;
extern t_drive driveA;
extern t_drive driveB;
extern byte *pbRAM;
extern byte *memmap_ROM[256];
extern byte *membank_read[4];
extern byte *pbExpansionROM;
extern byte *pbROMhi;
extern byte bTapeLevel;
extern std::vector<byte> pbTapeImage;
extern byte *pbTapeImageEnd;
extern byte *pbTapeBlock;
extern int iTapeCycleCount;
extern dword dwTapeZeroPulseCycles;

ImGuiUIState imgui_state;

// Forward declarations
static void tape_scan_blocks();
static void imgui_render_topbar();
static void imgui_render_menu();
static void imgui_render_options();
static void imgui_render_devtools();
static void imgui_render_memory_tool();
static void imgui_render_vkeyboard();

static void close_menu();

// Height tracking for stacked topbar + devtools bar
static int s_main_topbar_h = 25;
static int s_devtools_bar_h = 0;

// ─────────────────────────────────────────────────
// SDL3 file dialog callback
// ─────────────────────────────────────────────────

static void SDLCALL file_dialog_callback(void *userdata, const char * const *filelist, int /*filter*/)
{
  auto action = static_cast<FileDialogAction>(reinterpret_cast<intptr_t>(userdata));
  if (!filelist || !filelist[0]) return; // cancelled or error
  imgui_state.pending_dialog = action;
  imgui_state.pending_dialog_result = filelist[0];
}

static void process_pending_dialog()
{
  if (imgui_state.pending_dialog == FileDialogAction::None) return;

  FileDialogAction action = imgui_state.pending_dialog;
  std::string path = imgui_state.pending_dialog_result;
  int rom_slot = imgui_state.pending_rom_slot;

  imgui_state.pending_dialog = FileDialogAction::None;
  imgui_state.pending_dialog_result.clear();
  imgui_state.pending_rom_slot = -1;

  std::string dir = path.substr(0, path.find_last_of("/\\"));

  switch (action) {
    case FileDialogAction::LoadDiskA:
    case FileDialogAction::LoadDiskA_LED:
      CPC.driveA.file = path;
      file_load(CPC.driveA);
      CPC.current_dsk_path = dir;
      if (action == FileDialogAction::LoadDiskA) close_menu();
      break;
    case FileDialogAction::LoadDiskB:
    case FileDialogAction::LoadDiskB_LED:
      CPC.driveB.file = path;
      file_load(CPC.driveB);
      CPC.current_dsk_path = dir;
      if (action == FileDialogAction::LoadDiskB) close_menu();
      break;
    case FileDialogAction::SaveDiskA:
      dsk_save(path, &driveA);
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::SaveDiskB:
      dsk_save(path, &driveB);
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::LoadSnapshot:
      CPC.snapshot.file = path;
      file_load(CPC.snapshot);
      CPC.current_snap_path = dir;
      close_menu();
      break;
    case FileDialogAction::SaveSnapshot:
      snapshot_save(path);
      CPC.current_snap_path = dir;
      break;
    case FileDialogAction::LoadTape:
      CPC.tape.file = path;
      file_load(CPC.tape);
      CPC.current_tape_path = dir;
      tape_scan_blocks();
      close_menu();
      break;
    case FileDialogAction::LoadTape_LED:
      CPC.tape.file = path;
      file_load(CPC.tape);
      CPC.current_tape_path = dir;
      tape_scan_blocks();
      break;
    case FileDialogAction::LoadCartridge:
      CPC.cartridge.file = path;
      file_load(CPC.cartridge);
      CPC.current_cart_path = dir;
      emulator_reset();
      close_menu();
      break;
    case FileDialogAction::LoadROM:
      if (rom_slot >= 0 && rom_slot < MAX_ROM_SLOTS)
        CPC.rom_file[rom_slot] = path;
      break;
    default:
      break;
  }

  // Clear ImGui focus so keyboard events reach the emulator immediately
  ImGui::SetWindowFocus(NULL);
}

// ─────────────────────────────────────────────────
// Theme setup
// ─────────────────────────────────────────────────

void imgui_init_ui()
{
  // Merge transport symbol glyphs from system font into default font
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->AddFontDefault();
#ifdef __APPLE__
  // On macOS, merge Apple Symbols font for transport control glyphs
  ImFontConfig merge_cfg;
  merge_cfg.MergeMode = true;
  merge_cfg.PixelSnapH = true;
  static const ImWchar symbol_ranges[] = {
    0x23CF, 0x23CF, // ⏏
    0x25A0, 0x25A0, // ■
    0x25B6, 0x25B6, // ▶
    0x25C0, 0x25C0, // ◀
    0,
  };
  io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Apple Symbols.ttf", 13.0f, &merge_cfg, symbol_ranges);
#endif

  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.GrabRounding = 3.0f;
  style.ScrollbarRounding = 4.0f;
  style.TabRounding = 4.0f;
  style.WindowBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.WindowPadding = ImVec2(10, 10);
  style.FramePadding = ImVec2(6, 4);
  style.ItemSpacing = ImVec2(8, 6);

  ImVec4* c = style.Colors;
  // Background: 0x1A1A1E
  c[ImGuiCol_WindowBg]        = ImVec4(0.102f, 0.102f, 0.118f, 1.00f);
  c[ImGuiCol_PopupBg]         = ImVec4(0.120f, 0.120f, 0.140f, 0.95f);
  c[ImGuiCol_ChildBg]         = ImVec4(0.090f, 0.090f, 0.105f, 1.00f);
  // Text: 0xF0F0F0
  c[ImGuiCol_Text]            = ImVec4(0.941f, 0.941f, 0.941f, 1.00f);
  c[ImGuiCol_TextDisabled]    = ImVec4(0.500f, 0.500f, 0.500f, 1.00f);
  // Accent amber: 0x8A6A10
  c[ImGuiCol_Header]          = ImVec4(0.541f, 0.416f, 0.063f, 0.40f);
  c[ImGuiCol_HeaderHovered]   = ImVec4(0.541f, 0.416f, 0.063f, 0.60f);
  c[ImGuiCol_HeaderActive]    = ImVec4(0.541f, 0.416f, 0.063f, 0.80f);
  c[ImGuiCol_Button]          = ImVec4(0.541f, 0.416f, 0.063f, 0.45f);
  c[ImGuiCol_ButtonHovered]   = ImVec4(0.600f, 0.480f, 0.100f, 0.70f);
  c[ImGuiCol_ButtonActive]    = ImVec4(0.650f, 0.520f, 0.130f, 0.90f);
  // Selection blue: 0x3D5AFE
  c[ImGuiCol_Tab]                  = ImVec4(0.240f, 0.353f, 0.996f, 0.30f);
  c[ImGuiCol_TabHovered]           = ImVec4(0.240f, 0.353f, 0.996f, 0.60f);
  c[ImGuiCol_TabSelected]          = ImVec4(0.240f, 0.353f, 0.996f, 0.80f);
  c[ImGuiCol_TabSelectedOverline]  = ImVec4(0.240f, 0.353f, 0.996f, 1.00f);
  // Frame/border
  c[ImGuiCol_FrameBg]         = ImVec4(0.160f, 0.160f, 0.180f, 1.00f);
  c[ImGuiCol_FrameBgHovered]  = ImVec4(0.200f, 0.200f, 0.230f, 1.00f);
  c[ImGuiCol_FrameBgActive]   = ImVec4(0.240f, 0.240f, 0.280f, 1.00f);
  c[ImGuiCol_Border]          = ImVec4(0.300f, 0.300f, 0.350f, 0.50f);
  c[ImGuiCol_TitleBg]         = ImVec4(0.080f, 0.080f, 0.100f, 1.00f);
  c[ImGuiCol_TitleBgActive]   = ImVec4(0.120f, 0.120f, 0.150f, 1.00f);
  c[ImGuiCol_ScrollbarBg]     = ImVec4(0.080f, 0.080f, 0.100f, 0.60f);
  c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.300f, 0.300f, 0.350f, 0.80f);
  c[ImGuiCol_CheckMark]       = ImVec4(0.541f, 0.416f, 0.063f, 1.00f);
  c[ImGuiCol_SliderGrab]      = ImVec4(0.541f, 0.416f, 0.063f, 0.80f);
  c[ImGuiCol_SliderGrabActive]= ImVec4(0.650f, 0.520f, 0.130f, 1.00f);
  c[ImGuiCol_Separator]       = ImVec4(0.300f, 0.300f, 0.350f, 0.50f);

  // When viewports are enabled, platform windows should not have rounded corners
  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    style.WindowRounding = 0.0f;
    c[ImGuiCol_WindowBg].w = 1.0f;
  }

  // Register command palette entries from menu actions
  g_command_palette.clear_commands();
  for (const auto& ma : koncpc_menu_actions()) {
    if (ma.title[0] == '\0') continue; // skip empty entries
    std::string title = ma.title;
    std::string shortcut = ma.shortcut ? ma.shortcut : "";
    KONCPC_KEYS action_key = ma.action;
    g_command_palette.register_command(
        title, "", shortcut,
        [action_key]() {
          extern void applyKeypress(CPCScancode scancode, byte keyboard_matrix[], bool pressed);
          extern byte keyboard_matrix[];
          applyKeypress(static_cast<CPCScancode>(action_key), keyboard_matrix, true);
          applyKeypress(static_cast<CPCScancode>(action_key), keyboard_matrix, false);
        });
  }
  // Extra commands
  g_command_palette.register_command("Pause / Resume", "Toggle emulation pause", "Pause",
      []() {
        extern t_CPC CPC;
        CPC.paused = !CPC.paused;
      });
  g_command_palette.register_command("DevTools", "Open developer tools", "Shift+F2",
      []() { imgui_state.show_devtools = !imgui_state.show_devtools; });
  g_command_palette.register_command("Registers", "Show CPU registers", "",
      []() { g_devtools_ui.toggle_window("registers"); });
  g_command_palette.register_command("Disassembly", "Show disassembly view", "",
      []() { g_devtools_ui.toggle_window("disassembly"); });
  g_command_palette.register_command("Memory Hex", "Show memory hex view", "",
      []() { g_devtools_ui.toggle_window("memory_hex"); });
  g_command_palette.register_command("Stack", "Show stack window", "",
      []() { g_devtools_ui.toggle_window("stack"); });
  g_command_palette.register_command("Breakpoints", "Show breakpoint list", "",
      []() { g_devtools_ui.toggle_window("breakpoints"); });
  g_command_palette.register_command("Symbol Table", "Show symbol table", "",
      []() { g_devtools_ui.toggle_window("symbols"); });
  g_command_palette.register_command("Session Recording", "Show session recording controls", "",
      []() { g_devtools_ui.toggle_window("session_recording"); });
  g_command_palette.register_command("Graphics Finder", "Show graphics finder/tile viewer", "",
      []() { g_devtools_ui.toggle_window("gfx_finder"); });
  g_command_palette.register_command("Silicon Disc", "Show Silicon Disc panel", "",
      []() { g_devtools_ui.toggle_window("silicon_disc"); });
  g_command_palette.register_command("ASIC Registers", "Show ASIC register viewer", "",
      []() { g_devtools_ui.toggle_window("asic"); });
  g_command_palette.register_command("Disc Tools", "Show disc file/sector tools", "",
      []() { g_devtools_ui.toggle_window("disc_tools"); });
  g_command_palette.register_command("Data Areas", "Show data area manager", "",
      []() { g_devtools_ui.toggle_window("data_areas"); });
  g_command_palette.register_command("Disasm Export", "Export disassembly to file", "",
      []() { g_devtools_ui.toggle_window("disasm_export"); });
  g_command_palette.register_command("Recording Controls", "WAV/YM/AVI recording start/stop", "",
      []() { g_devtools_ui.toggle_window("recording_controls"); });
}

// ─────────────────────────────────────────────────
// Main dispatcher
// ─────────────────────────────────────────────────

void imgui_render_ui()
{
  process_pending_dialog();
  // Dockspace host must be rendered before other windows so they can dock into it
  workspace_render_dockspace();
  workspace_render_cpc_screen();
  imgui_render_topbar();
  if (imgui_state.show_menu)        imgui_render_menu();
  if (imgui_state.show_options)     imgui_render_options();
  if (imgui_state.show_devtools)    imgui_render_devtools();
  if (imgui_state.show_memory_tool) imgui_render_memory_tool();
  if (imgui_state.show_vkeyboard)   imgui_render_vkeyboard();
  // Phase 2 debug windows (extracted to DevToolsUI)
  g_devtools_ui.render();
  g_command_palette.render();

  // Reset devtools bar height when hidden so dockspace reclaims the space
  if (!imgui_state.show_devtools && s_devtools_bar_h != 0) {
    s_devtools_bar_h = 0;
    video_set_topbar(nullptr, s_main_topbar_h);
  }

  // Keyboard capture policy:
  // In docked mode, the emulator receives keyboard input only when the
  // CPC Screen tab is the focused/active window.  Clicking on any devtools
  // window (including text fields) naturally routes keyboard to ImGui.
  // In classic mode, keyboard goes to the emulator unless a GUI window is open.
  bool any_modal_gui = imgui_state.show_menu || imgui_state.show_options ||
                       imgui_state.show_memory_tool || imgui_state.show_vkeyboard ||
                       g_command_palette.is_open();
  if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Docked) {
    if (!any_modal_gui && imgui_state.cpc_screen_focused) {
      ImGui::GetIO().WantCaptureKeyboard = false;
    }
  } else {
    bool any_gui_open = any_modal_gui || imgui_state.show_devtools ||
                        g_devtools_ui.any_window_open();
    if (!any_gui_open) {
      ImGui::GetIO().WantCaptureKeyboard = false;
    }
  }
}

// ─────────────────────────────────────────────────
// Helper: close menu and resume emulation
// ─────────────────────────────────────────────────

static void close_menu()
{
  imgui_state.show_menu = false;
  // Don't clear show_options/show_about/show_quit_confirm here —
  // they may have just been set by the menu action that triggered close_menu().
  // Each dialog is responsible for clearing its own flag on close.
  // Only unpause if no dialog is keeping the emulator paused.
  if (!imgui_state.show_options && !imgui_state.show_quit_confirm) {
    CPC.paused = false;
  }
}

// ─────────────────────────────────────────────────
// Tape block scanner — builds offset table from TZX image
// ─────────────────────────────────────────────────

// safe_read_word/dword moved to imgui_ui_testable.h

static void tape_scan_blocks()
{
  imgui_state.tape_block_offsets.clear();
  imgui_state.tape_current_block = 0;
  if (pbTapeImage.empty()) return;

  byte* p = &pbTapeImage[0];
  byte* end = pbTapeImageEnd;

  while (p < end) {
    imgui_state.tape_block_offsets.push_back(p);

    // Calculate block size with bounds checking
    // Same size logic as Tape_BlockDone + Tape_GetNextBlock
    size_t block_size = 0;
    word w; dword d;

    switch (*p) {
      case 0x10: // Standard speed data
        if (!safe_read_word(p, end, 0x03, w)) goto done;
        block_size = w + 0x04 + 1;
        break;
      case 0x11: // Turbo speed data
        if (!safe_read_dword(p, end, 0x10, d)) goto done;
        block_size = (d & 0x00ffffff) + 0x12 + 1;
        break;
      case 0x12: // Pure tone
        block_size = 4 + 1;
        break;
      case 0x13: // Pulse sequence
        if (p + 2 > end) goto done;
        block_size = *(p+0x01) * 2 + 1 + 1;
        break;
      case 0x14: // Pure data
        if (!safe_read_dword(p, end, 0x08, d)) goto done;
        block_size = (d & 0x00ffffff) + 0x0a + 1;
        break;
      case 0x15: // Direct recording
        if (!safe_read_dword(p, end, 0x06, d)) goto done;
        block_size = (d & 0x00ffffff) + 0x08 + 1;
        break;
      case 0x20: // Pause
        block_size = 2 + 1;
        break;
      case 0x21: // Group start
        if (p + 2 > end) goto done;
        block_size = *(p+0x01) + 1 + 1;
        break;
      case 0x22: // Group end
        block_size = 1;
        break;
      case 0x30: // Text description
        if (p + 2 > end) goto done;
        block_size = *(p+0x01) + 1 + 1;
        break;
      case 0x31: // Message
        if (p + 3 > end) goto done;
        block_size = *(p+0x02) + 2 + 1;
        break;
      case 0x32: // Archive info
        if (!safe_read_word(p, end, 0x01, w)) goto done;
        block_size = w + 2 + 1;
        break;
      case 0x33: // Hardware type
        if (p + 2 > end) goto done;
        block_size = (*(p+0x01) * 3) + 1 + 1;
        break;
      case 0x34: // Emulation info
        block_size = 8 + 1;
        break;
      case 0x35: // Custom info
        if (!safe_read_dword(p, end, 0x11, d)) goto done;
        block_size = d + 0x14 + 1;
        break;
      case 0x40: // Snapshot
        if (!safe_read_dword(p, end, 0x02, d)) goto done;
        block_size = (d & 0x00ffffff) + 0x04 + 1;
        break;
      case 0x5A: // Glue
        block_size = 9 + 1;
        break;
      default: // Unknown block with 4-byte length
        if (!safe_read_dword(p, end, 0x01, d)) goto done;
        block_size = d + 4 + 1;
        break;
    }

    // Validate we won't advance past end
    if (p + block_size > end) goto done;
    p += block_size;
  }
done:;
}

// ─────────────────────────────────────────────────
// Top Bar
// ─────────────────────────────────────────────────

int imgui_topbar_height()
{
  // Button(21) + 2px padding top + 2px padding bottom = 25px.
  // Dynamic sync (lines below) corrects if ImGui expands beyond this.
  return 25;
}

static void imgui_render_topbar()
{
  float pad_y = 2.0f;
  float bar_height = static_cast<float>(imgui_topbar_height());

  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, bar_height));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, pad_y));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.094f, 0.094f, 0.094f, 1.0f));

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoDocking |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

  if (ImGui::Begin("##topbar", nullptr, flags)) {
    {
      s_main_topbar_h = static_cast<int>(ImGui::GetWindowSize().y);
      int total = s_main_topbar_h + s_devtools_bar_h;
      if (total != video_get_topbar_height()) {
        video_set_topbar(nullptr, total);
      }
    }
    if (ImGui::Button("Menu (F1)")) {
      if (!CPC.scr_gui_is_currently_on) {
        imgui_state.show_menu = true;
        imgui_state.menu_just_opened = true;
        CPC.paused = true;
      }
    }
    // Drive activity LEDs
    {
      float frameH = ImGui::GetFrameHeight();
      for (int drv = 0; drv < 2; drv++) {
        bool active = drv == 0 ? imgui_state.drive_a_led : imgui_state.drive_b_led;
        t_drive& drive = drv == 0 ? driveA : driveB;
        auto& driveFile = drv == 0 ? CPC.driveA.file : CPC.driveB.file;
        const char* driveLabel = drv == 0 ? "A:" : "B:";

        ImGui::SameLine(0, 12.0f);

        // Build display name (use pointer into existing string to avoid allocation)
        const char* displayName;
        if (drive.tracks) {
          auto pos = driveFile.find_last_of("/\\");
          displayName = (pos != std::string::npos) ? driveFile.c_str() + pos + 1 : driveFile.c_str();
        } else {
          displayName = "(no disk)";
        }

        // Push unique ID per drive to avoid conflicts
        ImGui::PushID(drv);

        ImGui::BeginGroup();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(driveLabel);
        ImGui::SameLine(0, 2.0f);

        // Draw LED
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        float ledW = 16.0f, ledH = 8.0f;
        float yOff = (frameH - ledH) * 0.5f;
        ImVec2 p0(cursor.x, cursor.y + yOff);
        ImVec2 p1(p0.x + ledW, p0.y + ledH);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (active) {
          // Active: bright red #FF0000
          dl->AddRectFilled(p0, p1, IM_COL32(255, 0, 0, 255));
          dl->AddLine(p0, ImVec2(p1.x, p0.y), IM_COL32(255, 100, 100, 255));
          dl->AddLine(p0, ImVec2(p0.x, p1.y), IM_COL32(255, 100, 100, 255));
          dl->AddLine(ImVec2(p0.x, p1.y), p1, IM_COL32(160, 0, 0, 255));
          dl->AddLine(ImVec2(p1.x, p0.y), p1, IM_COL32(160, 0, 0, 255));
        } else {
          // Inactive: dark red
          dl->AddRectFilled(p0, p1, IM_COL32(80, 0, 0, 255));
          dl->AddLine(p0, ImVec2(p1.x, p0.y), IM_COL32(110, 20, 20, 255));
          dl->AddLine(p0, ImVec2(p0.x, p1.y), IM_COL32(110, 20, 20, 255));
          dl->AddLine(ImVec2(p0.x, p1.y), p1, IM_COL32(40, 0, 0, 255));
          dl->AddLine(ImVec2(p1.x, p0.y), p1, IM_COL32(40, 0, 0, 255));
        }

        ImGui::Dummy(ImVec2(ledW, frameH));
        ImGui::SameLine(0, 4.0f);

        // Show track number when disk is loaded
        if (drive.tracks) {
          char trkStr[8];
          snprintf(trkStr, sizeof(trkStr), "T%02d", (int)drive.current_track);
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(trkStr);
          ImGui::PopStyleColor();
          ImGui::SameLine(0, 4.0f);
        }

        // Show filename or "(no disk)" as clickable text
        ImGui::PushStyleColor(ImGuiCol_Text, drive.tracks
          ? ImVec4(0.75f, 0.75f, 0.75f, 1.0f)
          : ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(displayName);
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        // Click on the whole group (label + LED + filename)
        if (ImGui::IsItemClicked()) {
          if (drive.tracks) {
            // Ask to confirm eject
            imgui_state.eject_confirm_drive = drv;
          } else {
            // Load disk
            static const SDL_DialogFileFilter disk_filters[] = {
              { "Disk Images", "dsk;ipf;raw;zip" }
            };
            auto act = drv == 0 ? FileDialogAction::LoadDiskA_LED : FileDialogAction::LoadDiskB_LED;
            SDL_ShowOpenFileDialog(file_dialog_callback,
              reinterpret_cast<void*>(static_cast<intptr_t>(act)),
              mainSDLWindow, disk_filters, 1, CPC.current_dsk_path.c_str(), false);
          }
        }

        ImGui::PopID();
      }
    }

    // Eject confirmation popup (rendered inside topbar window)
    if (imgui_state.eject_confirm_drive >= 0) {
      ImGui::OpenPopup("Eject Disk?");
    }
    if (ImGui::BeginPopup("Eject Disk?")) {
      int drv = imgui_state.eject_confirm_drive;
      const char* name = drv == 0 ? "A" : "B";
      ImGui::Text("Eject disk from drive %s?", name);
      ImGui::Spacing();
      if (ImGui::Button("Eject", ImVec2(80, 0))) {
        t_drive& drive = drv == 0 ? driveA : driveB;
        auto& driveFile = drv == 0 ? CPC.driveA.file : CPC.driveB.file;
        dsk_eject(&drive);
        driveFile.clear();
        imgui_state.eject_confirm_drive = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        imgui_state.eject_confirm_drive = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    } else {
      imgui_state.eject_confirm_drive = -1;
    }

    // ── Tape waveform oscilloscope ──
    {
      bool tape_loaded = !pbTapeImage.empty();
      bool tape_playing = tape_loaded && CPC.tape_motor && CPC.tape_play_button;

      // Reset state when tape is ejected
      if (!tape_loaded) {
        imgui_state.tape_decoded_head = 0;
        memset(imgui_state.tape_decoded_buf, 0, sizeof(imgui_state.tape_decoded_buf));
      }

      // Sampling happens in kon_cpc_ja.cpp main loop (sub-frame rate)

      ImGui::SameLine(0, 12);
      ImGui::AlignTextToFramePadding();
      float frameH = ImGui::GetFrameHeight();

      ImU32 color_active = IM_COL32(0x00, 0xFF, 0x80, 0xFF);
      ImU32 color_dim    = IM_COL32(0x00, 0x40, 0x20, 0xFF);
      ImU32 label_color  = tape_playing ? color_active : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);

      // Update current block index from pbTapeBlock pointer (skip if unchanged)
      static byte* last_pbTapeBlock = nullptr;
      if (tape_loaded && !imgui_state.tape_block_offsets.empty() && pbTapeBlock != last_pbTapeBlock) {
        last_pbTapeBlock = pbTapeBlock;
        for (int i = 0; i < (int)imgui_state.tape_block_offsets.size(); i++) {
          if (imgui_state.tape_block_offsets[i] == pbTapeBlock) {
            imgui_state.tape_current_block = i;
            break;
          }
          // pbTapeBlock may be past last known offset (between blocks)
          if (imgui_state.tape_block_offsets[i] > pbTapeBlock) {
            imgui_state.tape_current_block = i > 0 ? i - 1 : 0;
            break;
          }
        }
      }

      // ── TAPE label ──
      ImGui::PushStyleColor(ImGuiCol_Text, label_color);
      ImGui::TextUnformatted("TAPE");
      ImGui::PopStyleColor();

      // ── Filename (clickable when no tape → load) ──
      ImGui::SameLine(0, 4);
      {
        // Use pointer into existing string to avoid allocation
        const char* tapeName;
        if (tape_loaded && !CPC.tape.file.empty()) {
          auto pos = CPC.tape.file.find_last_of("/\\");
          tapeName = (pos != std::string::npos) ? CPC.tape.file.c_str() + pos + 1 : CPC.tape.file.c_str();
        } else {
          tapeName = "(no tape)";
        }
        ImGui::PushStyleColor(ImGuiCol_Text, tape_loaded
          ? ImVec4(0.75f, 0.75f, 0.75f, 1.0f)
          : ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tapeName);
        ImGui::PopStyleColor();
        if (!tape_loaded && ImGui::IsItemClicked()) {
          static const SDL_DialogFileFilter tape_filters[] = {
            { "Tape Images", "cdt;voc;zip" }
          };
          SDL_ShowOpenFileDialog(file_dialog_callback,
            reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadTape_LED)),
            mainSDLWindow, tape_filters, 1, CPC.current_tape_path.c_str(), false);
        }
      }

      // ── Transport buttons (gray SmallButtons) ──
      ImGui::SameLine(0, 6);
      {
        // Gray button style
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.15f, 0.15f, 0.15f, 1.0f));

        bool at_start = !tape_loaded || imgui_state.tape_current_block <= 0;
        bool at_end = !tape_loaded || imgui_state.tape_block_offsets.empty();
        bool is_playing = tape_loaded && CPC.tape_play_button;

        // |◀ Prev block
        ImGui::BeginDisabled(at_start);
        if (ImGui::SmallButton("\xe2\x97\x80##prev")) { // ◀
          int prev = imgui_state.tape_current_block - 1;
          if (prev >= 0 && prev < (int)imgui_state.tape_block_offsets.size()) {
            pbTapeBlock = imgui_state.tape_block_offsets[prev];
            iTapeCycleCount = 0;
            CPC.tape_play_button = 0;
            Tape_GetNextBlock();
            imgui_state.tape_current_block = prev;
          }
        }
        { // Draw bar on left side of prev button
          ImVec2 rmin = ImGui::GetItemRectMin();
          ImVec2 rmax = ImGui::GetItemRectMax();
          float bx = rmin.x + ImGui::GetStyle().FramePadding.x - 1.0f;
          float pad = (rmax.y - rmin.y) * 0.15f;
          ImU32 barCol = at_start ? IM_COL32(0x50, 0x50, 0x50, 0xFF) : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
          ImGui::GetWindowDrawList()->AddLine(ImVec2(bx, rmin.y + pad), ImVec2(bx, rmax.y - pad), barCol, 2.0f);
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0, 2);

        // ▶ Play
        if (is_playing) {
          // Highlight play button green when playing
          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.35f, 0.18f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.45f, 0.25f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.25f, 0.12f, 1.0f));
        }
        ImGui::BeginDisabled(!tape_loaded || is_playing);
        if (ImGui::SmallButton("\xe2\x96\xb6##play")) { // ▶
          CPC.tape_play_button = 0x10;
        }
        ImGui::EndDisabled();
        if (is_playing) ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 2);

        // ⏹ Stop
        ImGui::BeginDisabled(!is_playing);
        if (ImGui::SmallButton("\xe2\x96\xa0##stop")) { // ■
          CPC.tape_play_button = 0;
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0, 2);

        // ▷| Next block
        ImGui::BeginDisabled(at_end || imgui_state.tape_current_block >= (int)imgui_state.tape_block_offsets.size() - 1);
        if (ImGui::SmallButton("\xe2\x96\xb6##next")) { // ▶
          int next = imgui_state.tape_current_block + 1;
          if (next < (int)imgui_state.tape_block_offsets.size()) {
            pbTapeBlock = imgui_state.tape_block_offsets[next];
            iTapeCycleCount = 0;
            CPC.tape_play_button = 0;
            Tape_GetNextBlock();
            imgui_state.tape_current_block = next;
          }
        }
        { // Draw bar on right side of next button
          ImVec2 rmin = ImGui::GetItemRectMin();
          ImVec2 rmax = ImGui::GetItemRectMax();
          float bx = rmax.x - ImGui::GetStyle().FramePadding.x + 1.0f;
          float pad = (rmax.y - rmin.y) * 0.15f;
          bool dis = at_end || imgui_state.tape_current_block >= (int)imgui_state.tape_block_offsets.size() - 1;
          ImU32 barCol = dis ? IM_COL32(0x50, 0x50, 0x50, 0xFF) : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
          ImGui::GetWindowDrawList()->AddLine(ImVec2(bx, rmin.y + pad), ImVec2(bx, rmax.y - pad), barCol, 2.0f);
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0, 2);

        // ⏏ Eject
        ImGui::BeginDisabled(!tape_loaded);
        if (ImGui::SmallButton("\xe2\x8f\x8f##eject")) { // ⏏
          imgui_state.eject_confirm_tape = true;
        }
        ImGui::EndDisabled();

        ImGui::PopStyleColor(3); // gray button style
      }

      // ── Block counter ──
      if (tape_loaded && !imgui_state.tape_block_offsets.empty()) {
        ImGui::SameLine(0, 4);
        char blockStr[32];
        snprintf(blockStr, sizeof(blockStr), "%d/%d",
          imgui_state.tape_current_block + 1,
          (int)imgui_state.tape_block_offsets.size());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(blockStr);
        ImGui::PopStyleColor();
      }

      ImGui::SameLine(0, 4);
      float waveW = 100.0f;
      ImVec2 cursor = ImGui::GetCursorScreenPos();
      // Vertically center the waveform box
      float yOff = (frameH - frameH * 0.8f) * 0.5f;
      ImVec2 p0(cursor.x, cursor.y + yOff);
      float boxH = frameH * 0.8f;
      ImVec2 p1(p0.x + waveW, p0.y + boxH);

      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(p0, p1, IM_COL32(0x10, 0x10, 0x10, 0xFF));
      dl->AddRect(p0, p1, tape_playing ? IM_COL32(0x00, 0x80, 0x40, 0x80) : IM_COL32(0x00, 0x30, 0x18, 0x60));

      ImU32 wave_color = tape_playing ? color_active : color_dim;
      int N = ImGuiUIState::TAPE_WAVE_SAMPLES;
      float stepX = waveW / static_cast<float>(N - 1);
      int mode = imgui_state.tape_wave_mode;

      float yBot = p1.y - 2.0f;
      float yTop = p0.y + 2.0f;

      auto yForSample = [&](byte val) -> float {
        return val ? yTop : yBot;
      };

      int oldest = imgui_state.tape_wave_head;

      if (mode == 0) {
        // ── Pulse (sub-frame scrolling waveform) ──
        // Build step waveform as polyline for batched drawing
        ImVec2 points[N * 2 + 2]; // Max: 2 points per sample + start
        int nPoints = 0;

        float prevY = yForSample(imgui_state.tape_wave_buf[oldest]);
        points[nPoints++] = ImVec2(p0.x, prevY); // Start point

        for (int i = 1; i < N; i++) {
          int idx = (oldest + i) % N;
          float curX = p0.x + i * stepX;
          float curY = yForSample(imgui_state.tape_wave_buf[idx]);
          if (curY != prevY) {
            // Level change: add horizontal endpoint, then vertical step
            points[nPoints++] = ImVec2(curX, prevY);
            points[nPoints++] = ImVec2(curX, curY);
            prevY = curY;
          }
        }
        // Final horizontal endpoint
        points[nPoints++] = ImVec2(p1.x, prevY);

        dl->AddPolyline(points, nPoints, wave_color, 0, 1.0f);
      } else {
        // ── Decoded bits (green 1px bars from Tape_ReadDataBit) ──
        int dN = ImGuiUIState::TAPE_DECODED_SAMPLES;
        int dHead = imgui_state.tape_decoded_head;
        int visCount = static_cast<int>(waveW); // 1px per bit
        if (visCount > dN) visCount = dN;
        // Walk oldest→newest for the last visCount samples
        int startIdx = (dHead - visCount + dN) % dN;
        ImU32 col_one  = tape_playing ? IM_COL32(0x00, 0xFF, 0x80, 0xFF) : IM_COL32(0x00, 0x44, 0x00, 0xFF);
        ImU32 col_zero = tape_playing ? IM_COL32(0x00, 0x44, 0x00, 0xFF) : IM_COL32(0x00, 0x18, 0x00, 0xFF);
        for (int i = 0; i < visCount; i++) {
          int idx = (startIdx + i) % dN;
          float x = p0.x + (waveW - visCount) + i;
          ImU32 c = imgui_state.tape_decoded_buf[idx] ? col_one : col_zero;
          dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + 1.0f, p1.y), c);
        }
      }

      // Advance cursor past the waveform box; click cycles mode (2 modes now)
      ImGui::Dummy(ImVec2(waveW, frameH));
      if (ImGui::IsItemClicked()) {
        imgui_state.tape_wave_mode = (imgui_state.tape_wave_mode + 1) % 2;
      }
    }

    // Tape eject confirmation popup
    if (imgui_state.eject_confirm_tape) {
      ImGui::OpenPopup("Eject Tape?");
    }
    if (ImGui::BeginPopup("Eject Tape?")) {
      ImGui::TextUnformatted("Eject tape?");
      ImGui::Spacing();
      if (ImGui::Button("Eject", ImVec2(80, 0))) {
        tape_eject();
        CPC.tape.file.clear();
        imgui_state.tape_block_offsets.clear();
        imgui_state.tape_current_block = 0;
        imgui_state.eject_confirm_tape = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        imgui_state.eject_confirm_tape = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    } else {
      imgui_state.eject_confirm_tape = false;
    }

    // ── Layout dropdown ──
    {
      // Right-align before FPS counter
      float fps_w = 0.0f;
      if (!imgui_state.topbar_fps.empty())
        fps_w = ImGui::CalcTextSize(imgui_state.topbar_fps.c_str()).x + 16.0f;
      float btn_w = ImGui::CalcTextSize("Layout").x + ImGui::GetStyle().FramePadding.x * 2.0f;
      ImGui::SameLine(ImGui::GetWindowWidth() - fps_w - btn_w - 12.0f);

      if (ImGui::Button("Layout")) {
        ImGui::OpenPopup("##LayoutPopup");
      }
      if (ImGui::BeginPopup("##LayoutPopup")) {
        // Mode selection
        if (ImGui::RadioButton("Classic Mode", CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic)) {
          CPC.workspace_layout = t_CPC::WorkspaceLayoutMode::Classic;
        }
        if (ImGui::RadioButton("Docked Mode", CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Docked)) {
          CPC.workspace_layout = t_CPC::WorkspaceLayoutMode::Docked;
        }
        ImGui::Separator();

        // Preset layouts
        if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Docked) {
          if (ImGui::MenuItem("Apply Debug Layout"))
            workspace_apply_preset(WorkspacePreset::Debug);
          if (ImGui::MenuItem("Apply IDE Layout"))
            workspace_apply_preset(WorkspacePreset::IDE);
          if (ImGui::MenuItem("Apply Hardware Layout"))
            workspace_apply_preset(WorkspacePreset::Hardware);
        } else {
          if (ImGui::MenuItem("Debug")) {
            g_devtools_ui.toggle_window("registers");
            g_devtools_ui.toggle_window("disassembly");
            g_devtools_ui.toggle_window("stack");
            g_devtools_ui.toggle_window("breakpoints");
          }
          if (ImGui::MenuItem("Memory")) {
            g_devtools_ui.toggle_window("memory_hex");
            g_devtools_ui.toggle_window("symbols");
            g_devtools_ui.toggle_window("data_areas");
          }
          if (ImGui::MenuItem("Hardware")) {
            g_devtools_ui.toggle_window("video_state");
            g_devtools_ui.toggle_window("audio_state");
            g_devtools_ui.toggle_window("asic");
            g_devtools_ui.toggle_window("silicon_disc");
          }
        }

        // Custom saved layouts
        ImGui::Separator();
        {
          static bool open_save_popup = false;
          if (ImGui::MenuItem("Save Layout..."))
            open_save_popup = true;

          auto layouts = workspace_list_layouts();

          if (ImGui::BeginMenu("Load Layout")) {
            if (layouts.empty()) {
              ImGui::MenuItem("No saved layouts", nullptr, false, false);
            } else {
              for (auto& l : layouts) {
                if (ImGui::MenuItem(l.c_str()))
                  workspace_load_layout(l);
              }
            }
            ImGui::EndMenu();
          }

          if (ImGui::BeginMenu("Delete Layout")) {
            if (layouts.empty()) {
              ImGui::MenuItem("No saved layouts", nullptr, false, false);
            } else {
              for (auto& l : layouts) {
                if (ImGui::MenuItem(l.c_str()))
                  workspace_delete_layout(l);
              }
            }
            ImGui::EndMenu();
          }

          // Deferred popup open
          if (open_save_popup) {
            ImGui::OpenPopup("Save Layout##popup");
            open_save_popup = false;
          }
        }

        // CPC Screen scale (only in docked mode)
        if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Docked) {
          ImGui::Separator();
          ImGui::TextUnformatted("CPC Screen Scale");
          if (ImGui::RadioButton("Fit",  CPC.cpc_screen_scale == t_CPC::ScreenScale::Fit)) CPC.cpc_screen_scale = t_CPC::ScreenScale::Fit;
          if (ImGui::RadioButton("1x",   CPC.cpc_screen_scale == t_CPC::ScreenScale::X1))  CPC.cpc_screen_scale = t_CPC::ScreenScale::X1;
          if (ImGui::RadioButton("2x",   CPC.cpc_screen_scale == t_CPC::ScreenScale::X2))  CPC.cpc_screen_scale = t_CPC::ScreenScale::X2;
          if (ImGui::RadioButton("3x",   CPC.cpc_screen_scale == t_CPC::ScreenScale::X3))  CPC.cpc_screen_scale = t_CPC::ScreenScale::X3;
        }

        // Save Layout popup
        {
          static char save_name[64] = "";
          static std::string save_error;
          if (ImGui::BeginPopup("Save Layout##popup")) {
            ImGui::TextUnformatted("Layout Name:");
            bool enter_pressed = ImGui::InputText("##save_name", save_name, sizeof(save_name),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsWindowAppearing())
              ImGui::SetKeyboardFocusHere(-1);

            if (!save_error.empty()) {
              ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
              ImGui::TextUnformatted(save_error.c_str());
              ImGui::PopStyleColor();
            }

            bool do_save = enter_pressed || ImGui::Button("Save");
            ImGui::SameLine();
            bool do_cancel = ImGui::Button("Cancel");

            if (do_save) {
              std::string name(save_name);
              while (!name.empty() && name.front() == ' ') name.erase(name.begin());
              while (!name.empty() && name.back() == ' ') name.pop_back();

              bool valid = !name.empty();
              if (valid) {
                for (char c : name) {
                  if (c == '/' || c == '\\' || c == '\0') { valid = false; break; }
                }
              }
              if (valid && (name == "." || name == "..")) valid = false;

              if (!valid) {
                save_error = "Invalid name";
              } else if (workspace_save_layout(name)) {
                save_name[0] = '\0';
                save_error.clear();
                ImGui::CloseCurrentPopup();
              } else {
                save_error = "Save failed";
              }
            }
            if (do_cancel) {
              save_name[0] = '\0';
              save_error.clear();
              ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
          }
        }

        ImGui::EndPopup();
      }
    }

    if (!imgui_state.topbar_fps.empty()) {
      float fps_width = ImGui::CalcTextSize(imgui_state.topbar_fps.c_str()).x;
      ImGui::SameLine(ImGui::GetWindowWidth() - fps_width - 8);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(imgui_state.topbar_fps.c_str());
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
}

// ─────────────────────────────────────────────────
// Menu
// ─────────────────────────────────────────────────

static void imgui_render_menu()
{
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  ImGui::SetNextWindowSize(ImVec2(260, 0));

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoDocking |
                           ImGuiWindowFlags_AlwaysAutoResize;


  bool menu_open = true;
  if (!ImGui::Begin("konCePCja", &menu_open, flags)) {
    if (!menu_open) close_menu();
    ImGui::End();
    return;
  }
  if (!menu_open) { close_menu(); ImGui::End(); return; }

  // Keyboard shortcuts within menu
  bool action = false;
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { close_menu(); ImGui::End(); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_O)) { imgui_state.show_options = true; action = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_M)) { imgui_state.show_memory_tool = true; action = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_D)) { imgui_state.show_devtools = true; action = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) { emulator_reset(); action = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_Q)) { imgui_state.show_quit_confirm = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_A)) { imgui_state.show_about = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) { emulator_reset(); action = true; }
  }

  float bw = ImGui::GetContentRegionAvail().x;

  if (ImGui::Button("Options (O)", ImVec2(bw, 0))) {
    imgui_state.show_options = true;
    action = true;
  }

  ImGui::Separator();

  // --- Disk operations ---
  if (ImGui::Button("Load Disk A...", ImVec2(bw, 0))) {
    static const SDL_DialogFileFilter filters[] = { { "Disk Images", "dsk;ipf;raw;zip" } };
    SDL_ShowOpenFileDialog(file_dialog_callback,
      reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadDiskA)),
      mainSDLWindow, filters, 1, CPC.current_dsk_path.c_str(), false);
    action = true;
  }
  if (ImGui::Button("Load Disk B...", ImVec2(bw, 0))) {
    static const SDL_DialogFileFilter filters[] = { { "Disk Images", "dsk;ipf;raw;zip" } };
    SDL_ShowOpenFileDialog(file_dialog_callback,
      reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadDiskB)),
      mainSDLWindow, filters, 1, CPC.current_dsk_path.c_str(), false);
    action = true;
  }
  if (ImGui::Button("Save Disk A...", ImVec2(bw, 0))) {
    if (driveA.tracks) {
      static const SDL_DialogFileFilter filters[] = { { "Disk Images", "dsk" } };
      SDL_ShowSaveFileDialog(file_dialog_callback,
        reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::SaveDiskA)),
        mainSDLWindow, filters, 1, CPC.current_dsk_path.c_str());
      action = true;
    }
  }
  if (ImGui::Button("Save Disk B...", ImVec2(bw, 0))) {
    if (driveB.tracks) {
      static const SDL_DialogFileFilter filters[] = { { "Disk Images", "dsk" } };
      SDL_ShowSaveFileDialog(file_dialog_callback,
        reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::SaveDiskB)),
        mainSDLWindow, filters, 1, CPC.current_dsk_path.c_str());
      action = true;
    }
  }

  ImGui::Separator();

  // --- Snapshot operations ---
  if (ImGui::Button("Load Snapshot...", ImVec2(bw, 0))) {
    static const SDL_DialogFileFilter filters[] = { { "Snapshots", "sna;zip" } };
    SDL_ShowOpenFileDialog(file_dialog_callback,
      reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadSnapshot)),
      mainSDLWindow, filters, 1, CPC.current_snap_path.c_str(), false);
    action = true;
  }
  if (ImGui::Button("Save Snapshot...", ImVec2(bw, 0))) {
    static const SDL_DialogFileFilter filters[] = { { "Snapshots", "sna" } };
    SDL_ShowSaveFileDialog(file_dialog_callback,
      reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::SaveSnapshot)),
      mainSDLWindow, filters, 1, CPC.current_snap_path.c_str());
    action = true;
  }

  ImGui::Separator();

  // --- Tape & Cartridge ---
  if (ImGui::Button("Load Tape...", ImVec2(bw, 0))) {
    static const SDL_DialogFileFilter filters[] = { { "Tape Images", "cdt;voc;zip" } };
    SDL_ShowOpenFileDialog(file_dialog_callback,
      reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadTape)),
      mainSDLWindow, filters, 1, CPC.current_tape_path.c_str(), false);
    action = true;
  }
  if (!pbTapeImage.empty()) {
    if (ImGui::Button("Eject Tape", ImVec2(bw, 0))) {
      tape_eject();
      CPC.tape.file.clear();
      imgui_state.tape_block_offsets.clear();
      imgui_state.tape_current_block = 0;
      action = true;
    }
  }
  if (ImGui::Button("Load Cartridge...", ImVec2(bw, 0))) {
    static const SDL_DialogFileFilter filters[] = { { "Cartridges", "cpr;zip" } };
    SDL_ShowOpenFileDialog(file_dialog_callback,
      reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadCartridge)),
      mainSDLWindow, filters, 1, CPC.current_cart_path.c_str(), false);
    action = true;
  }

  ImGui::Separator();

  // --- Tools ---
  if (ImGui::Button("Memory Tool (M)", ImVec2(bw, 0))) {
    imgui_state.show_memory_tool = true;
    action = true;
  }
  if (ImGui::Button("DevTools (D)", ImVec2(bw, 0))) {
    imgui_state.show_devtools = true;
    action = true;
  }

  ImGui::Separator();

  if (ImGui::Button("Reset (F5/R)", ImVec2(bw, 0))) {
    emulator_reset();
    action = true;
  }
  // About and Quit open sub-popups within the menu — don't close
  if (ImGui::Button("About (A)", ImVec2(bw, 0))) {
    imgui_state.show_about = true;
  }
  if (ImGui::Button("Resume (Esc)", ImVec2(bw, 0))) {
    action = true;
  }
  if (ImGui::Button("Quit (Q)", ImVec2(bw, 0))) {
    imgui_state.show_quit_confirm = true;
  }

  ImGui::End();

  if (action) close_menu();

  // --- About popup ---
  if (imgui_state.show_about) {
    ImGui::OpenPopup("About konCePCja");
    imgui_state.show_about = false;
  }
  if (ImGui::BeginPopupModal("About konCePCja", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("konCePCja %s", VERSION_STRING);
    ImGui::Separator();
    ImGui::Text("Amstrad CPC Emulator");
    ImGui::Text("Based on Caprice32 by Ulrich Doewich");
    ImGui::Spacing();
    ImGui::Text("Shortcuts:");
    ImGui::BulletText("F1 - Menu");
    ImGui::BulletText("Shift+F2 - DevTools");
    ImGui::BulletText("F5 - Reset");
    ImGui::BulletText("F10 - Quit");
    ImGui::BulletText("Ctrl+F5 - Screenshot");
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
  }

  // --- Quit confirmation popup ---
  if (imgui_state.show_quit_confirm) {
    ImGui::OpenPopup("Confirm Quit");
    imgui_state.show_quit_confirm = false;
  }
  if (ImGui::BeginPopupModal("Confirm Quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Are you sure you want to quit?");
    ImGui::Spacing();
    if (ImGui::Button("Yes", ImVec2(80, 0))) {
      cleanExit(0, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("No", ImVec2(80, 0))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// ─────────────────────────────────────────────────
// Options
// ─────────────────────────────────────────────────

// Video plugin names (mirrors CapriceOptions)
static const char* video_plugins[] = { "Direct (SDL)", "Software Scaling" };
static const char* scale_items[] = { "1x", "2x", "3x", "4x" };
static const char* sample_rates[] = { "11025", "22050", "44100", "48000", "96000" };
static int sample_rate_values[] = { 11025, 22050, 44100, 48000, 96000 };
static const char* cpc_models[] = { "CPC 464", "CPC 664", "CPC 6128", "6128+" };
static const char* ram_sizes[] = { "64 KB", "128 KB", "192 KB", "256 KB", "320 KB", "512 KB", "576 KB", "4160 KB (Yarek 4MB)" };
static int ram_size_values[] = { 64, 128, 192, 256, 320, 512, 576, 4160 };

static const char* crtc_type_labels[] = {
  "Type 0 - HD6845S (Hitachi)",
  "Type 1 - UM6845R (UMC)",
  "Type 2 - MC6845 (Motorola)",
  "Type 3 - AMS40489 (Amstrad ASIC)"
};

// find_ram_index and find_sample_rate_index moved to imgui_ui_testable.h

static void imgui_render_options()
{
  static bool first_open = true;
  static unsigned char old_crtc_type = 0;
  static bool old_m4_enabled = false;
  if (first_open) {
    imgui_state.old_cpc_settings = CPC;
    old_crtc_type = CRTC.crtc_type;
    old_m4_enabled = g_m4board.enabled;
    first_open = false;
  }

  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(480, 420), ImGuiCond_Appearing);


  bool open = true;
  if (!ImGui::Begin("Options", &open, ImGuiWindowFlags_NoCollapse)) {
    if (!open) { imgui_state.show_options = false; first_open = true; }
    ImGui::End();
    return;
  }

  if (ImGui::BeginTabBar("OptionsTabs")) {
    // ── General Tab ──
    if (ImGui::BeginTabItem("General")) {
      int model = static_cast<int>(CPC.model);
      if (ImGui::Combo("CPC Model", &model, cpc_models, IM_ARRAYSIZE(cpc_models))) {
        CPC.model = model;
      }

      int ram_idx = find_ram_index(CPC.ram_size);
      if (ImGui::Combo("RAM Size", &ram_idx, ram_sizes, IM_ARRAYSIZE(ram_sizes))) {
        CPC.ram_size = ram_size_values[ram_idx];
      }

      int crtc = static_cast<int>(CRTC.crtc_type);
      if (ImGui::Combo("CRTC Type", &crtc, crtc_type_labels, IM_ARRAYSIZE(crtc_type_labels))) {
        CRTC.crtc_type = static_cast<unsigned char>(crtc);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Auto-set by CPC Model on reset.\nOverride for compatibility testing.");
      }

      bool limit = CPC.limit_speed != 0;
      if (ImGui::Checkbox("Limit Speed", &limit)) {
        CPC.limit_speed = limit ? 1 : 0;
      }

      int speed = static_cast<int>(CPC.speed);
      if (ImGui::SliderInt("Speed", &speed, MIN_SPEED_SETTING, MAX_SPEED_SETTING)) {
        CPC.speed = speed;
      }

      bool printer = CPC.printer != 0;
      if (ImGui::Checkbox("Printer Capture", &printer)) {
        CPC.printer = printer ? 1 : 0;
      }

      bool sw = g_smartwatch.enabled;
      if (ImGui::Checkbox("SmartWatch RTC", &sw)) { g_smartwatch.enabled = sw; }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Dobbertin SmartWatch (DS1216) in upper ROM socket.\nProvides real-time clock via host system time.");
      }

      bool sf2 = g_symbiface.enabled;
      if (ImGui::Checkbox("Symbiface II", &sf2)) { g_symbiface.enabled = sf2; }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Symbiface II expansion (IDE + RTC + PS/2 Mouse).\nConfigure IDE images in config file.");
      }

      bool m4 = g_m4board.enabled;
      if (ImGui::Checkbox("M4 Board", &m4)) { g_m4board.enabled = m4; }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("M4 Board (WiFi/SD).\nSet m4_sd_path in config for virtual SD card.");
      }

      ImGui::EndTabItem();
    }

    // ── ROMs Tab ──
    if (ImGui::BeginTabItem("ROMs")) {
      ImGui::Text("Expansion ROM Slots:");
      ImGui::Spacing();
      if (ImGui::BeginTable("rom_slots", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##status", ImGuiTableColumnFlags_WidthFixed, 16.0f);
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##unload", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < MAX_ROM_SLOTS; i++) {
          ImGui::PushID(i);
          ImGui::TableNextRow();

          bool loaded = (memmap_ROM[i] != nullptr);

          // Status dot
          ImGui::TableSetColumnIndex(0);
          ImVec4 dot_color = loaded ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 0.5f);
          ImGui::TextColored(dot_color, loaded ? "●" : "○");

          // Slot number
          ImGui::TableSetColumnIndex(1);
          ImGui::Text("%d", i);

          // ROM filename (clickable to load)
          ImGui::TableSetColumnIndex(2);
          std::string display;
          if (CPC.rom_file[i].empty()) {
            display = "(empty)";
          } else if (CPC.rom_file[i] == "DEFAULT") {
            display = (CPC.model == 0) ? "(default - none)" : "amsdos.rom (default)";
          } else {
            // Show just the filename, not the full path
            size_t sep = CPC.rom_file[i].find_last_of("/\\");
            display = (sep != std::string::npos) ? CPC.rom_file[i].substr(sep + 1) : CPC.rom_file[i];
          }
          if (display.length() > 24) display = "..." + display.substr(display.length() - 21);

          if (ImGui::Selectable(display.c_str())) {
            static const SDL_DialogFileFilter filters[] = { { "ROM files", "rom;bin" } };
            imgui_state.pending_rom_slot = i;
            SDL_ShowOpenFileDialog(file_dialog_callback,
              reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadROM)),
              mainSDLWindow, filters, 1, CPC.rom_path.c_str(), false);
          }
          if (!CPC.rom_file[i].empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", CPC.rom_file[i].c_str());
          }

          // Identified ROM name
          ImGui::TableSetColumnIndex(3);
          if (loaded) {
            std::string id = rom_identify(memmap_ROM[i]);
            if (!id.empty()) {
              ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", id.c_str());
            }
          }

          // Unload button (slots 0-1 are system ROMs, protected)
          ImGui::TableSetColumnIndex(3);
          if (i >= 2 && loaded) {
            if (ImGui::SmallButton("X")) {
              delete[] memmap_ROM[i];
              memmap_ROM[i] = nullptr;
              CPC.rom_file[i] = "";
              // If this was the active upper ROM, revert to BASIC ROM
              if (GateArray.upper_ROM == static_cast<unsigned char>(i)) {
                pbExpansionROM = pbROMhi;
                if (!(GateArray.ROM_config & 0x08)) {
                  membank_read[3] = pbExpansionROM;
                }
              }
            }
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip("Unload ROM from slot %d", i);
            }
          }

          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }

    // ── Video Tab ──
    if (ImGui::BeginTabItem("Video")) {
      int plugin = static_cast<int>(CPC.scr_style);
      if (ImGui::Combo("Video Plugin", &plugin, video_plugins, IM_ARRAYSIZE(video_plugins))) {
        CPC.scr_style = plugin;
      }

      int scale = static_cast<int>(CPC.scr_scale) - 1;
      if (scale < 0) scale = 0;
      if (ImGui::Combo("Scale", &scale, scale_items, IM_ARRAYSIZE(scale_items))) {
        CPC.scr_scale = scale + 1;
      }

      bool colour = CPC.scr_tube == 0;
      if (ImGui::RadioButton("Colour", colour)) { CPC.scr_tube = 0; }
      ImGui::SameLine();
      if (ImGui::RadioButton("Mono (Green)", !colour)) { CPC.scr_tube = 1; }

      int intensity = static_cast<int>(CPC.scr_intensity);
      if (ImGui::SliderInt("Intensity", &intensity, 5, 15)) {
        CPC.scr_intensity = intensity;
      }

      bool fps = CPC.scr_fps != 0;
      if (ImGui::Checkbox("Show FPS", &fps)) { CPC.scr_fps = fps ? 1 : 0; }

      bool fullscreen = CPC.scr_window == 0;
      if (ImGui::Checkbox("Fullscreen", &fullscreen)) { CPC.scr_window = fullscreen ? 0 : 1; }

      bool aspect = CPC.scr_preserve_aspect_ratio != 0;
      if (ImGui::Checkbox("Preserve Aspect Ratio", &aspect)) {
        CPC.scr_preserve_aspect_ratio = aspect ? 1 : 0;
      }

      ImGui::EndTabItem();
    }

    // ── Audio Tab ──
    if (ImGui::BeginTabItem("Audio")) {
      bool snd = CPC.snd_enabled != 0;
      if (ImGui::Checkbox("Enable Sound", &snd)) { CPC.snd_enabled = snd ? 1 : 0; }

      int rate_idx = find_sample_rate_index(CPC.snd_playback_rate);
      if (ImGui::Combo("Sample Rate", &rate_idx, sample_rates, IM_ARRAYSIZE(sample_rates))) {
        CPC.snd_playback_rate = sample_rate_values[rate_idx];
      }

      bool stereo = CPC.snd_stereo != 0;
      if (ImGui::RadioButton("Mono", !stereo)) { CPC.snd_stereo = 0; }
      ImGui::SameLine();
      if (ImGui::RadioButton("Stereo", stereo)) { CPC.snd_stereo = 1; }

      bool bits16 = CPC.snd_bits != 0;
      if (ImGui::RadioButton("8-bit", !bits16)) { CPC.snd_bits = 0; }
      ImGui::SameLine();
      if (ImGui::RadioButton("16-bit", bits16)) { CPC.snd_bits = 1; }

      int vol = static_cast<int>(CPC.snd_volume);
      if (ImGui::SliderInt("Volume", &vol, 0, 100)) { CPC.snd_volume = vol; }

      ImGui::Separator();
      ImGui::Text("Peripherals");
      bool pp = CPC.snd_pp_device != 0;
      if (ImGui::Checkbox("Digiblaster", &pp)) { CPC.snd_pp_device = pp ? 1 : 0; }
      bool amdrum = g_amdrum.enabled;
      if (ImGui::Checkbox("AmDrum", &amdrum)) { g_amdrum.enabled = amdrum; }
      bool disk_snd = g_drive_sounds.disk_enabled;
      if (ImGui::Checkbox("Disk Drive Sounds", &disk_snd)) { g_drive_sounds.disk_enabled = disk_snd; }
      bool tape_snd = g_drive_sounds.tape_enabled;
      if (ImGui::Checkbox("Tape Sounds", &tape_snd)) { g_drive_sounds.tape_enabled = tape_snd; }

      ImGui::EndTabItem();
    }

    // ── Input Tab ──
    if (ImGui::BeginTabItem("Input")) {
      int keyboard = static_cast<int>(CPC.keyboard);
      const char* cpc_langs[] = { "English", "French", "Spanish" };
      int max_langs = IM_ARRAYSIZE(cpc_langs);
      if (static_cast<int>(CPC.keyboard) >= max_langs) keyboard = 0;
      if (ImGui::Combo("CPC Language", &keyboard, cpc_langs, max_langs)) {
        CPC.keyboard = keyboard;
      }

      bool joy_emu = CPC.joystick_emulation != 0;
      if (ImGui::Checkbox("Joystick Emulation", &joy_emu)) {
        CPC.joystick_emulation = joy_emu ? 1 : 0;
      }

      bool joysticks = CPC.joysticks != 0;
      if (ImGui::Checkbox("Use Real Joysticks", &joysticks)) {
        CPC.joysticks = joysticks ? 1 : 0;
      }

      bool amx = g_amx_mouse.enabled;
      if (ImGui::Checkbox("AMX Mouse", &amx)) { g_amx_mouse.enabled = amx; }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("AMX Mouse on joystick port.\nMaps host mouse to CPC joystick directions + buttons.");
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::Separator();
  ImGui::Spacing();

  // Bottom buttons
  if (ImGui::Button("Save", ImVec2(80, 0))) {
    std::string cfg = getConfigurationFilename(true);
    saveConfiguration(CPC, cfg);
    // Apply changes that need re-init
    if (CPC.model != imgui_state.old_cpc_settings.model ||
        CPC.ram_size != imgui_state.old_cpc_settings.ram_size ||
        CPC.keyboard != imgui_state.old_cpc_settings.keyboard ||
        g_m4board.enabled != old_m4_enabled) {
      emulator_init();
    }
    update_cpc_speed();
    video_set_palette();
    imgui_state.show_options = false;
    CPC.paused = false;
    first_open = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(80, 0))) {
    CPC = imgui_state.old_cpc_settings;
    CRTC.crtc_type = old_crtc_type;
    g_m4board.enabled = old_m4_enabled;
    imgui_state.show_options = false;
    CPC.paused = false;
    first_open = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("OK", ImVec2(80, 0))) {
    if (CPC.model != imgui_state.old_cpc_settings.model ||
        CPC.ram_size != imgui_state.old_cpc_settings.ram_size ||
        CPC.keyboard != imgui_state.old_cpc_settings.keyboard ||
        g_m4board.enabled != old_m4_enabled) {
      emulator_init();
    }
    update_cpc_speed();
    video_set_palette();
    imgui_state.show_options = false;
    CPC.paused = false;
    first_open = true;
  }

  if (!open) {
    // Window closed via X button — treat as Cancel
    CPC = imgui_state.old_cpc_settings;
    CRTC.crtc_type = old_crtc_type;
    g_m4board.enabled = old_m4_enabled;
    imgui_state.show_options = false;
    CPC.paused = false;
    first_open = true;
  }

  ImGui::End();
}

// ─────────────────────────────────────────────────
// DevTools
// ─────────────────────────────────────────────────

// parse_hex, safe_read_word/dword moved to imgui_ui_testable.h


// Format memory line into stack buffer - zero heap allocations
// Buffer size: 512 bytes handles up to 64 bytes/line with all formats
static int format_memory_line(char* buf, size_t buf_size, unsigned int base_addr,
                              int bytes_per_line, int format)
{
  if (buf_size == 0) return 0;

  int offset = 0;
  int remaining = static_cast<int>(buf_size);

  // Helper to safely advance after snprintf (clamps to remaining space)
  auto advance = [&](int written) {
    if (written < 0) return false;
    int actual = (written < remaining) ? written : remaining - 1;
    offset += actual;
    remaining -= actual;
    return remaining > 1;
  };

  // Address
  if (!advance(snprintf(buf + offset, remaining, "%04X : ", base_addr))) {
    buf[offset] = '\0';
    return offset;
  }

  // Hex bytes
  for (int j = 0; j < bytes_per_line && remaining > 1; j++) {
    if (!advance(snprintf(buf + offset, remaining, "%02X ", pbRAM[(base_addr + j) & 0xFFFF])))
      break;
  }

  // Extended formats
  if (format == 1 && remaining > 1) { // Hex & char
    if (advance(snprintf(buf + offset, remaining, " | "))) {
      for (int j = 0; j < bytes_per_line && remaining > 1; j++) {
        byte b = pbRAM[(base_addr + j) & 0xFFFF];
        buf[offset++] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
        remaining--;
      }
    }
  } else if (format == 2 && remaining > 1) { // Hex & u8
    if (advance(snprintf(buf + offset, remaining, " | "))) {
      for (int j = 0; j < bytes_per_line && remaining > 1; j++) {
        if (!advance(snprintf(buf + offset, remaining, "%3u ", pbRAM[(base_addr + j) & 0xFFFF])))
          break;
      }
    }
  }

  buf[offset] = '\0';
  return offset;
}

// Shared poke input UI with proper validation
// Returns true if poke was executed
static bool ui_poke_input(char* addr_buf, size_t addr_size,
                          char* val_buf, size_t val_size,
                          const char* id_suffix)
{
  ImGui::PushID(id_suffix);

  ImGui::SetNextItemWidth(50);
  ImGui::InputText("Addr", addr_buf, addr_size, ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(40);
  ImGui::InputText("Val", val_buf, val_size, ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();

  bool poked = false;
  if (ImGui::Button("Poke")) {
    unsigned long addr, val;
    if (parse_hex(addr_buf, &addr, 0xFFFF) && parse_hex(val_buf, &val, 0xFF)) {
      pbRAM[addr] = static_cast<byte>(val);
      poked = true;
    }
  }
  ImGui::PopID();
  return poked;
}


static bool devtools_first_open = true;

static void imgui_render_devtools()
{
  // Auto-open core windows on first DevTools open
  if (devtools_first_open) {
    if (!g_devtools_ui.any_window_open()) {
      g_devtools_ui.toggle_window("registers");
      g_devtools_ui.toggle_window("disassembly");
      g_devtools_ui.toggle_window("stack");
    }
    devtools_first_open = false;
  }

  ImGuiViewport* vp = ImGui::GetMainViewport();
  float bar_y = vp->Pos.y + static_cast<float>(s_main_topbar_h);

  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, bar_y));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, 0));  // auto-height
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.11f, 1.0f));

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoDocking |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_AlwaysAutoResize;

  if (ImGui::Begin("##devtools_bar", nullptr, flags)) {
    // ── Window dropdown buttons ──
    if (ImGui::Button("CPU")) ImGui::OpenPopup("##dt_cpu");
    if (ImGui::BeginPopup("##dt_cpu")) {
      ImGui::MenuItem("Registers",         nullptr, g_devtools_ui.window_ptr("registers"));
      ImGui::MenuItem("Disassembly",       nullptr, g_devtools_ui.window_ptr("disassembly"));
      ImGui::MenuItem("Stack",             nullptr, g_devtools_ui.window_ptr("stack"));
      ImGui::MenuItem("Breakpoints/WP/IO", nullptr, g_devtools_ui.window_ptr("breakpoints"));
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Memory")) ImGui::OpenPopup("##dt_mem");
    if (ImGui::BeginPopup("##dt_mem")) {
      ImGui::MenuItem("Memory Hex",  nullptr, g_devtools_ui.window_ptr("memory_hex"));
      ImGui::MenuItem("Data Areas",  nullptr, g_devtools_ui.window_ptr("data_areas"));
      ImGui::MenuItem("Symbols",     nullptr, g_devtools_ui.window_ptr("symbols"));
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Hardware")) ImGui::OpenPopup("##dt_hw");
    if (ImGui::BeginPopup("##dt_hw")) {
      ImGui::MenuItem("Video State",    nullptr, g_devtools_ui.window_ptr("video_state"));
      ImGui::MenuItem("Audio State",    nullptr, g_devtools_ui.window_ptr("audio_state"));
      ImGui::MenuItem("ASIC Registers", nullptr, g_devtools_ui.window_ptr("asic"));
      ImGui::MenuItem("Silicon Disc",   nullptr, g_devtools_ui.window_ptr("silicon_disc"));
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Media")) ImGui::OpenPopup("##dt_media");
    if (ImGui::BeginPopup("##dt_media")) {
      ImGui::MenuItem("Disc Tools",      nullptr, g_devtools_ui.window_ptr("disc_tools"));
      ImGui::MenuItem("Graphics Finder", nullptr, g_devtools_ui.window_ptr("gfx_finder"));
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Export")) ImGui::OpenPopup("##dt_export");
    if (ImGui::BeginPopup("##dt_export")) {
      ImGui::MenuItem("Disasm Export",      nullptr, g_devtools_ui.window_ptr("disasm_export"));
      ImGui::MenuItem("Session Recording",  nullptr, g_devtools_ui.window_ptr("session_recording"));
      ImGui::MenuItem("Recording Controls", nullptr, g_devtools_ui.window_ptr("recording_controls"));
      ImGui::EndPopup();
    }

    // ── Vertical separator ──
    ImGui::SameLine(0, 12.0f);
    {
      ImVec2 cur = ImGui::GetCursorScreenPos();
      float h = ImGui::GetFrameHeight();
      ImGui::GetWindowDrawList()->AddLine(
          ImVec2(cur.x, cur.y + 2.0f),
          ImVec2(cur.x, cur.y + h - 2.0f),
          IM_COL32(128, 128, 128, 128), 1.0f);
      ImGui::Dummy(ImVec2(1.0f, h));
    }

    // ── Step/Pause controls ──
    // Capture paused state once so BeginDisabled/EndDisabled stay balanced
    // even when a button handler sets CPC.paused = false mid-frame.
    ImGui::SameLine(0, 12.0f);
    bool was_paused = CPC.paused;
    if (!was_paused) ImGui::BeginDisabled();
    if (ImGui::Button("Step In"))  {
      z80.step_in = 1;
      z80.step_out = 0;
      z80.step_out_addresses.clear();
      CPC.paused = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Over")) {
      z80.step_in = 0;
      z80.step_out = 0;
      z80.step_out_addresses.clear();
      word pc = z80.PC.w.l;
      if (z80_is_call_or_rst(pc)) {
        z80_add_breakpoint_ephemeral(pc + z80_instruction_length(pc));
        CPC.paused = false;
      } else {
        z80.step_in = 1;
        CPC.paused = false;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Out")) {
      z80.step_out = 1;
      z80.step_out_addresses.clear();
      z80.step_in = 0;
      CPC.paused = false;
    }
    if (!was_paused) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(CPC.paused ? "Resume" : "Pause")) {
      CPC.paused = !CPC.paused;
    }

    // ── Sync devtools bar height ──
    s_devtools_bar_h = static_cast<int>(ImGui::GetWindowSize().y);
    int total = s_main_topbar_h + s_devtools_bar_h;
    if (total != video_get_topbar_height()) {
      video_set_topbar(nullptr, total);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
}

// ─────────────────────────────────────────────────
// Memory Tool
// ─────────────────────────────────────────────────

static void imgui_render_memory_tool()
{
  ImGui::SetNextWindowSize(ImVec2(400, 340), ImGuiCond_FirstUseEver);


  bool open = true;
  if (!ImGui::Begin("Memory Tool", &open, ImGuiWindowFlags_NoCollapse)) {
    if (!open) imgui_state.show_memory_tool = false;
    ImGui::End();
    return;
  }

  // Poke
  ui_poke_input(imgui_state.mem_poke_addr, sizeof(imgui_state.mem_poke_addr),
                imgui_state.mem_poke_val, sizeof(imgui_state.mem_poke_val), "mt");

  // Display address
  ImGui::SetNextItemWidth(50);
  ImGui::InputText("Display##mt", imgui_state.mem_display_addr, sizeof(imgui_state.mem_display_addr),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (ImGui::Button("Go##mt")) {
    unsigned long addr;
    if (parse_hex(imgui_state.mem_display_addr, &addr, 0xFFFF))
      imgui_state.mem_display_value = static_cast<int>(addr);
    else
      imgui_state.mem_display_value = -1;
    imgui_state.mem_filter_value = -1;
  }

  // Bytes per line
  const char* bpl_items[] = { "1", "4", "8", "16", "32", "64" };
  int bpl_values[] = { 1, 4, 8, 16, 32, 64 };
  int bpl_idx = 3;
  for (int i = 0; i < 6; i++) { if (bpl_values[i] == imgui_state.mem_bytes_per_line) bpl_idx = i; }
  ImGui::SetNextItemWidth(60);
  if (ImGui::Combo("Bytes/Line##mt", &bpl_idx, bpl_items, 6)) {
    imgui_state.mem_bytes_per_line = bpl_values[bpl_idx];
  }

  // Filter
  ImGui::SetNextItemWidth(40);
  ImGui::InputText("Filter Byte##mt", imgui_state.mem_filter_val, sizeof(imgui_state.mem_filter_val),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (ImGui::Button("Filter##mt")) {
    unsigned long val;
    if (parse_hex(imgui_state.mem_filter_val, &val, 0xFF)) {
      imgui_state.mem_filter_value = static_cast<int>(val);
      imgui_state.mem_display_value = -1;
    } else {
      imgui_state.mem_filter_value = -1;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Dump to stdout##mt")) {
    int bpl = imgui_state.mem_bytes_per_line;
    for (unsigned int i = 0; i < 65536u / bpl; i++) {
      std::cout << std::uppercase << std::setfill('0') << std::setw(4) << std::hex << i * bpl << " : ";
      for (int j = 0; j < bpl; j++) {
        std::cout << std::setw(2) << static_cast<unsigned int>(pbRAM[i * bpl + j]) << " ";
      }
      std::cout << "\n";
    }
    std::cout << std::flush;
  }

  // Hex dump
  if (ImGui::BeginChild("##mtmem", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    int bpl = imgui_state.mem_bytes_per_line;
    int total_lines = 65536 / bpl;
    bool filtering = imgui_state.mem_filter_value >= 0 && imgui_state.mem_filter_value <= 255;
    bool displaying = imgui_state.mem_display_value >= 0 && imgui_state.mem_display_value <= 0xFFFF;

    if (filtering || displaying) {
      // Can't use clipper with filtering — iterate all
      for (int i = 0; i < total_lines; i++) {
        unsigned int base = i * bpl;
        bool show = false;
        if (!filtering && !displaying) { show = true; }
        if (displaying) {
          if (base <= static_cast<unsigned int>(imgui_state.mem_display_value) &&
              static_cast<unsigned int>(imgui_state.mem_display_value) < base + bpl) show = true;
        }
        if (filtering) {
          for (int j = 0; j < bpl; j++) {
            if (pbRAM[(base + j) & 0xFFFF] == imgui_state.mem_filter_value) { show = true; break; }
          }
        }
        if (!show) continue;

        char line[512];
        format_memory_line(line, sizeof(line), base, bpl, 0);
        ImGui::TextUnformatted(line);
      }
    } else {
      // Fast path with clipper
      ImGuiListClipper clipper;
      clipper.Begin(total_lines);
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
          unsigned int base = i * bpl;
          char line[512];
          format_memory_line(line, sizeof(line), base, bpl, 0);
          ImGui::TextUnformatted(line);
        }
      }
    }
  }
  ImGui::EndChild();

  if (!open) imgui_state.show_memory_tool = false;

  ImGui::End();
}

// ─────────────────────────────────────────────────
// Virtual Keyboard – CPC 6128 layout
// Main keyboard left, numeric keypad (F0-F9) right, cursor keys below numpad
// ─────────────────────────────────────────────────

static void imgui_render_vkeyboard()
{
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(575, 265), ImGuiCond_FirstUseEver);


  if (!ImGui::Begin("CPC 6128 Keyboard", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) imgui_state.show_vkeyboard = false;
    return;
  }

  // Key dimensions
  const float K = 28.0f;   // standard key width
  const float H = 32.0f;   // key height (taller for two-line labels)
  const float S = 2.0f;    // spacing
  const float ROW = H + S; // row height

  // CPC brown/tan key color
  ImVec4 key_color(0.55f, 0.45f, 0.30f, 1.0f);
  ImVec4 key_hover(0.65f, 0.55f, 0.40f, 1.0f);
  ImVec4 key_active(0.45f, 0.35f, 0.20f, 1.0f);
  ImVec4 mod_on_color(0.3f, 0.5f, 0.3f, 1.0f);

  ImGui::PushStyleColor(ImGuiCol_Button, key_color);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, key_hover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, key_active);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

  bool caps_on = imgui_state.vkeyboard_caps_lock;
  bool shift_on = imgui_state.vkeyboard_shift_next || caps_on;  // either gives shift effect
  bool ctrl_on = imgui_state.vkeyboard_ctrl_next;

  // Shift mapping for non-letter keys
  static const std::map<char, char> shift_map = {
    {'1', '!'}, {'2', '"'}, {'3', '#'}, {'4', '$'}, {'5', '%'},
    {'6', '&'}, {'7', '\''}, {'8', '('}, {'9', ')'}, {'0', '_'},
    {'-', '='}, {'^', '\xa3'},  // £ in Latin-1
    {';', '+'}, {':', '*'}, {'[', '{'}, {']', '}'},
    {',', '<'}, {'.', '>'}, {'/', '?'}, {'\\', '`'},
    {'@', '|'}
  };

  // Helper to emit a key - sends directly to emulator
  auto emit_key = [&](const char* text) {
    if (!text) return;
    std::string to_send = text;

    // SHIFT toggle (one-shot)
    if (strcmp(text, "\x01" "SHIFT") == 0) {
      imgui_state.vkeyboard_shift_next = !imgui_state.vkeyboard_shift_next;
      return;
    }
    // CAPS LOCK toggle (sticky)
    if (strcmp(text, "\x01" "CAPS") == 0) {
      imgui_state.vkeyboard_caps_lock = !imgui_state.vkeyboard_caps_lock;
      return;
    }
    // CTRL toggle (one-shot)
    if (strcmp(text, "\x01" "CTRL") == 0) {
      imgui_state.vkeyboard_ctrl_next = !imgui_state.vkeyboard_ctrl_next;
      return;
    }

    // Apply CTRL modifier
    if (ctrl_on && to_send.length() == 1) {
      char c = to_send[0];
      if (c >= 'a' && c <= 'z') {
        to_send = std::string("\a") + static_cast<char>(CPC_CTRL_a + (c - 'a'));
      } else if (c >= '0' && c <= '9') {
        to_send = std::string("\a") + static_cast<char>(CPC_CTRL_0 + (c - '0'));
      }
      imgui_state.vkeyboard_ctrl_next = false;  // one-shot
    }
    // Apply SHIFT modifier
    else if (shift_on && to_send.length() == 1) {
      char c = to_send[0];
      if (c >= 'a' && c <= 'z') {
        to_send[0] = c - 32;  // uppercase
      } else {
        auto it = shift_map.find(c);
        if (it != shift_map.end()) {
          to_send[0] = it->second;
        }
      }
      imgui_state.vkeyboard_shift_next = false;  // one-shot (CAPS stays)
    }

    // Send directly to emulator
    koncpc_queue_virtual_keys(to_send);
  };

  // Modifier status line
  ImGui::Text("Modifiers:");
  ImGui::SameLine();
  if (caps_on)  { ImGui::TextColored(ImVec4(0,1,0.5f,1), "[CAPS]"); ImGui::SameLine(); }
  if (imgui_state.vkeyboard_shift_next) { ImGui::TextColored(ImVec4(0.5f,1,0,1), "[SHIFT]"); ImGui::SameLine(); }
  if (ctrl_on)  { ImGui::TextColored(ImVec4(1,0.5f,0,1), "[CTRL]"); ImGui::SameLine(); }
  ImGui::NewLine();

  float x0 = ImGui::GetCursorPosX();
  float y0 = ImGui::GetCursorPosY();

  // Width multipliers for special keys
  const float W_TAB    = 1.3f;
  const float W_CAPS   = 1.4f;
  const float W_LSHIFT = 1.9f;
  const float W_CTRL   = 1.5f;
  const float W_COPY   = 1.6f;
  // RETURN, right SHIFT, ENTER widths calculated dynamically to align right edges

  // Calculate main keyboard right edge: 15 standard keys + ESC in row 0
  // ESC 1 2 3 4 5 6 7 8 9 0 - ^/£ CLR DEL = 15 keys
  float main_end_x = x0 + (K + S) * 15 - S;  // right edge of DEL

  // Numpad starts after a gap from main keyboard right edge
  float np_x = main_end_x + S * 4;

  // Helper for function keys
  char fkey_emit[2] = { '\a', 0 };

  // ═══════════════════════════════════════════════════════════════════
  // ROW 0: ESC 1 2 3 4 5 6 7 8 9 0 - ^ CLR DEL | F7 F8 F9
  // ═══════════════════════════════════════════════════════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0));
  if (ImGui::Button("ESC", ImVec2(K, H))) { emit_key("\a\xbb"); } ImGui::SameLine(0, S);
  if (ImGui::Button("!\n1", ImVec2(K, H))) { emit_key("1"); } ImGui::SameLine(0, S);
  if (ImGui::Button("\"\n2", ImVec2(K, H))) { emit_key("2"); } ImGui::SameLine(0, S);
  if (ImGui::Button("#\n3", ImVec2(K, H))) { emit_key("3"); } ImGui::SameLine(0, S);
  if (ImGui::Button("$\n4", ImVec2(K, H))) { emit_key("4"); } ImGui::SameLine(0, S);
  if (ImGui::Button("%\n5", ImVec2(K, H))) { emit_key("5"); } ImGui::SameLine(0, S);
  if (ImGui::Button("&\n6", ImVec2(K, H))) { emit_key("6"); } ImGui::SameLine(0, S);
  if (ImGui::Button("'\n7", ImVec2(K, H))) { emit_key("7"); } ImGui::SameLine(0, S);
  if (ImGui::Button("(\n8", ImVec2(K, H))) { emit_key("8"); } ImGui::SameLine(0, S);
  if (ImGui::Button(")\n9", ImVec2(K, H))) { emit_key("9"); } ImGui::SameLine(0, S);
  if (ImGui::Button("_\n0", ImVec2(K, H))) { emit_key("0"); } ImGui::SameLine(0, S);
  if (ImGui::Button("=\n-", ImVec2(K, H))) { emit_key("-"); } ImGui::SameLine(0, S);
  if (ImGui::Button("\xc2\xa3\n^", ImVec2(K, H))) { emit_key("^"); } ImGui::SameLine(0, S);  // £ over ^
  if (ImGui::Button("CLR", ImVec2(K, H))) { emit_key("\a\xa5"); } ImGui::SameLine(0, S);
  if (ImGui::Button("DEL", ImVec2(K, H))) emit_key("\b");

  // Numpad row 0: F7 F8 F9
  ImGui::SetCursorPos(ImVec2(np_x, y0));
  fkey_emit[1] = static_cast<char>(CPC_F7);
  if (ImGui::Button("F7", ImVec2(K, H))) { emit_key(fkey_emit); } ImGui::SameLine(0, S);
  fkey_emit[1] = static_cast<char>(CPC_F8);
  if (ImGui::Button("F8", ImVec2(K, H))) { emit_key(fkey_emit); } ImGui::SameLine(0, S);
  fkey_emit[1] = static_cast<char>(CPC_F9);
  if (ImGui::Button("F9", ImVec2(K, H))) emit_key(fkey_emit);

  // ═══════════════════════════════════════════════════════════════════
  // ROW 1: TAB Q W E R T Y U I O P |/@ {/[  | F4 F5 F6
  // ═══════════════════════════════════════════════════════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + ROW));
  if (ImGui::Button("TAB", ImVec2(K*W_TAB, H))) { emit_key("\t"); } ImGui::SameLine(0, S);
  if (ImGui::Button("Q", ImVec2(K, H))) { emit_key("q"); } ImGui::SameLine(0, S);
  if (ImGui::Button("W", ImVec2(K, H))) { emit_key("w"); } ImGui::SameLine(0, S);
  if (ImGui::Button("E", ImVec2(K, H))) { emit_key("e"); } ImGui::SameLine(0, S);
  if (ImGui::Button("R", ImVec2(K, H))) { emit_key("r"); } ImGui::SameLine(0, S);
  if (ImGui::Button("T", ImVec2(K, H))) { emit_key("t"); } ImGui::SameLine(0, S);
  if (ImGui::Button("Y", ImVec2(K, H))) { emit_key("y"); } ImGui::SameLine(0, S);
  if (ImGui::Button("U", ImVec2(K, H))) { emit_key("u"); } ImGui::SameLine(0, S);
  if (ImGui::Button("I", ImVec2(K, H))) { emit_key("i"); } ImGui::SameLine(0, S);
  if (ImGui::Button("O", ImVec2(K, H))) { emit_key("o"); } ImGui::SameLine(0, S);
  if (ImGui::Button("P", ImVec2(K, H))) { emit_key("p"); } ImGui::SameLine(0, S);
  if (ImGui::Button("|\n@", ImVec2(K, H))) { emit_key("@"); } ImGui::SameLine(0, S);
  if (ImGui::Button("{\n[", ImVec2(K, H))) { emit_key("["); } ImGui::SameLine(0, S);
  // RETURN upper part - at end of row 1
  float ret_x = ImGui::GetCursorPosX();
  float ret_w = main_end_x - ret_x;
  if (ImGui::Button("RETURN##1", ImVec2(ret_w, H))) emit_key("\n");
  // RETURN lower part - starts S after where ] ends in row 2
  // Row 2: CAPS(1.4K) + 12 keys (A-L + ; : ]) with spacing = K*W_CAPS + S + 12*(K+S)
  float ret2_x = x0 + K*W_CAPS + S + 12*(K + S);
  float ret2_w = main_end_x - ret2_x;
  ImGui::SetCursorPos(ImVec2(ret2_x, y0 + ROW + H));
  if (ImGui::Button("##ret2", ImVec2(ret2_w, ROW))) emit_key("\n");

  // Numpad row 1: F4 F5 F6
  ImGui::SetCursorPos(ImVec2(np_x, y0 + ROW));
  fkey_emit[1] = static_cast<char>(CPC_F4);
  if (ImGui::Button("F4", ImVec2(K, H))) { emit_key(fkey_emit); } ImGui::SameLine(0, S);
  fkey_emit[1] = static_cast<char>(CPC_F5);
  if (ImGui::Button("F5", ImVec2(K, H))) { emit_key(fkey_emit); } ImGui::SameLine(0, S);
  fkey_emit[1] = static_cast<char>(CPC_F6);
  if (ImGui::Button("F6", ImVec2(K, H))) emit_key(fkey_emit);

  // ═══════════════════════════════════════════════════════════════════
  // ROW 2: CAPS A S D F G H J K L +/; */: }/] RETURN(wide) | F1 F2 F3
  // ═══════════════════════════════════════════════════════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + ROW * 2));
  if (caps_on) ImGui::PushStyleColor(ImGuiCol_Button, mod_on_color);
  if (ImGui::Button("CAPS\nLOCK", ImVec2(K*W_CAPS, H))) emit_key("\x01" "CAPS");
  if (caps_on) ImGui::PopStyleColor();
  ImGui::SameLine(0, S);
  if (ImGui::Button("A", ImVec2(K, H))) { emit_key("a"); } ImGui::SameLine(0, S);
  if (ImGui::Button("S", ImVec2(K, H))) { emit_key("s"); } ImGui::SameLine(0, S);
  if (ImGui::Button("D", ImVec2(K, H))) { emit_key("d"); } ImGui::SameLine(0, S);
  if (ImGui::Button("F", ImVec2(K, H))) { emit_key("f"); } ImGui::SameLine(0, S);
  if (ImGui::Button("G", ImVec2(K, H))) { emit_key("g"); } ImGui::SameLine(0, S);
  if (ImGui::Button("H", ImVec2(K, H))) { emit_key("h"); } ImGui::SameLine(0, S);
  if (ImGui::Button("J", ImVec2(K, H))) { emit_key("j"); } ImGui::SameLine(0, S);
  if (ImGui::Button("K", ImVec2(K, H))) { emit_key("k"); } ImGui::SameLine(0, S);
  if (ImGui::Button("L", ImVec2(K, H))) { emit_key("l"); } ImGui::SameLine(0, S);
  if (ImGui::Button("+\n;", ImVec2(K, H))) { emit_key(";"); } ImGui::SameLine(0, S);
  if (ImGui::Button("*\n:", ImVec2(K, H))) { emit_key(":"); } ImGui::SameLine(0, S);
  if (ImGui::Button("}\n]", ImVec2(K, H))) emit_key("]");

  // Numpad row 2: F1 F2 F3
  ImGui::SetCursorPos(ImVec2(np_x, y0 + ROW * 2));
  fkey_emit[1] = static_cast<char>(CPC_F1);
  if (ImGui::Button("F1", ImVec2(K, H))) { emit_key(fkey_emit); } ImGui::SameLine(0, S);
  fkey_emit[1] = static_cast<char>(CPC_F2);
  if (ImGui::Button("F2", ImVec2(K, H))) { emit_key(fkey_emit); } ImGui::SameLine(0, S);
  fkey_emit[1] = static_cast<char>(CPC_F3);
  if (ImGui::Button("F3", ImVec2(K, H))) emit_key(fkey_emit);

  // ═══════════════════════════════════════════════════════════════════
  // ROW 3: SHIFT Z X C V B N M </,  >/. ?// `/\ SHIFT RETURN | F0 ↑ .
  // RETURN lower part forms L-shape with row 2 RETURN
  // ═══════════════════════════════════════════════════════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + ROW * 3));
  bool shift_highlight = imgui_state.vkeyboard_shift_next;  // highlight both SHIFTs together
  if (shift_highlight) ImGui::PushStyleColor(ImGuiCol_Button, mod_on_color);
  if (ImGui::Button("SHIFT##L", ImVec2(K*W_LSHIFT, H))) emit_key("\x01" "SHIFT");
  if (shift_highlight) ImGui::PopStyleColor();
  ImGui::SameLine(0, S);
  if (ImGui::Button("Z", ImVec2(K, H))) { emit_key("z"); } ImGui::SameLine(0, S);
  if (ImGui::Button("X", ImVec2(K, H))) { emit_key("x"); } ImGui::SameLine(0, S);
  if (ImGui::Button("C", ImVec2(K, H))) { emit_key("c"); } ImGui::SameLine(0, S);
  if (ImGui::Button("V", ImVec2(K, H))) { emit_key("v"); } ImGui::SameLine(0, S);
  if (ImGui::Button("B", ImVec2(K, H))) { emit_key("b"); } ImGui::SameLine(0, S);
  if (ImGui::Button("N", ImVec2(K, H))) { emit_key("n"); } ImGui::SameLine(0, S);
  if (ImGui::Button("M", ImVec2(K, H))) { emit_key("m"); } ImGui::SameLine(0, S);
  if (ImGui::Button("<\n,", ImVec2(K, H))) { emit_key(","); } ImGui::SameLine(0, S);
  if (ImGui::Button(">\n.##main", ImVec2(K, H))) { emit_key("."); } ImGui::SameLine(0, S);
  if (ImGui::Button("?\n/", ImVec2(K, H))) { emit_key("/"); } ImGui::SameLine(0, S);
  if (ImGui::Button("`\n\\", ImVec2(K, H))) { emit_key("\\"); } ImGui::SameLine(0, S);
  // Right SHIFT - fills to main_end_x (naturally narrower due to wider left SHIFT)
  float rshift_x = ImGui::GetCursorPosX();
  float rshift_w = main_end_x - rshift_x;
  if (shift_highlight) ImGui::PushStyleColor(ImGuiCol_Button, mod_on_color);
  if (ImGui::Button("SHIFT##R", ImVec2(rshift_w, H))) emit_key("\x01" "SHIFT");
  if (shift_highlight) ImGui::PopStyleColor();

  // Numpad row 3: F0 ↑ .
  ImGui::SetCursorPos(ImVec2(np_x, y0 + ROW * 3));
  fkey_emit[1] = static_cast<char>(CPC_F0);
  if (ImGui::Button("F0", ImVec2(K, H))) { emit_key(fkey_emit); } ImGui::SameLine(0, S);
  if (ImGui::Button("\xe2\x86\x91##up", ImVec2(K, H))) { emit_key("\a\xae"); } ImGui::SameLine(0, S);
  if (ImGui::Button(".##np", ImVec2(K, H))) emit_key(".");

  // ═══════════════════════════════════════════════════════════════════
  // ROW 4: CTRL COPY ====SPACE==== ENTER | ← ↓ →
  // ═══════════════════════════════════════════════════════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + ROW * 4));
  if (ctrl_on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.4f, 0.2f, 1.0f));
  if (ImGui::Button("CTRL", ImVec2(K*W_CTRL, H))) emit_key("\x01" "CTRL");
  if (ctrl_on) ImGui::PopStyleColor();
  ImGui::SameLine(0, S);
  if (ImGui::Button("COPY", ImVec2(K*W_COPY, H))) emit_key("\a\xa9");
  ImGui::SameLine(0, S);
  // SPACE - fixed width, then ENTER fills to main_end_x
  float space_w = K * 8.0f;
  if (ImGui::Button("SPACE", ImVec2(space_w, H))) emit_key(" ");
  ImGui::SameLine(0, S);
  // ENTER - calculate width to reach main_end_x
  float enter_x = ImGui::GetCursorPosX();
  float enter_w = main_end_x - enter_x;
  if (ImGui::Button("ENTER", ImVec2(enter_w, H))) emit_key("\n");

  // Numpad row 4: ← ↓ →
  ImGui::SetCursorPos(ImVec2(np_x, y0 + ROW * 4));
  if (ImGui::Button("\xe2\x86\x90##left", ImVec2(K, H))) { emit_key("\a\xaf"); } ImGui::SameLine(0, S);
  if (ImGui::Button("\xe2\x86\x93##down", ImVec2(K, H))) { emit_key("\a\xb0"); } ImGui::SameLine(0, S);
  if (ImGui::Button("\xe2\x86\x92##right", ImVec2(K, H))) emit_key("\a\xb1");

  // Move cursor below keyboard for the rest
  ImGui::SetCursorPos(ImVec2(x0, y0 + ROW * 5 + S * 2));

  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);

  ImGui::Separator();

  // ── Quick commands ──
  ImGui::Text("Quick:");
  ImGui::SameLine();
  if (ImGui::SmallButton("cat")) emit_key("cat\n");
  ImGui::SameLine();
  if (ImGui::SmallButton("run\"")) emit_key("run\"\n");
  ImGui::SameLine();
  if (ImGui::SmallButton("cls")) emit_key("cls\n");
  ImGui::SameLine();
  if (ImGui::SmallButton("|tape")) emit_key("|tape\n");
  ImGui::SameLine();
  if (ImGui::SmallButton("|cpm")) emit_key("|cpm\n");
  ImGui::SameLine();
  if (ImGui::SmallButton("|a")) emit_key("|a\n");
  ImGui::SameLine();
  if (ImGui::SmallButton("|b")) emit_key("|b\n");


  if (!open) imgui_state.show_vkeyboard = false;

  ImGui::End();
}

