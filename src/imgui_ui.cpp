#include "imgui_ui.h"
#include "imgui_ui_testable.h"
#include "imgui.h"
#include "command_palette.h"
#include "menu_actions.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <iostream>

#include "koncepcja.h"
#include "keyboard.h"
#include "z80.h"
#include "z80_disassembly.h"
#include "slotshandler.h"
#include "disk.h"
#include "tape.h"
#include "video.h"
#include "symfile.h"
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
// Phase 2 debug windows
static void imgui_render_registers_window();
static void imgui_render_disassembly_window();
static void imgui_render_memory_hex_window();
static void imgui_render_stack_window();
static void imgui_render_breakpoint_list_window();
static void imgui_render_symbol_table_window();

static void close_menu();

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
      if (rom_slot >= 0 && rom_slot < 16)
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
          // Inject the emulator key as a virtual event
          SDL_Event ev{};
          ev.type = SDL_EVENT_KEY_UP;
          ev.key.key = SDLK_UNKNOWN;
          // Emulator keys are dispatched via the scancode path; for simplicity
          // we directly toggle the relevant state here.
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
        if (CPC.paused) { CPC.paused = false; } else { CPC.paused = true; }
      });
  g_command_palette.register_command("DevTools", "Open developer tools", "Shift+F2",
      []() { imgui_state.show_devtools = !imgui_state.show_devtools; });
  g_command_palette.register_command("Registers", "Show CPU registers", "",
      []() { imgui_state.show_registers = !imgui_state.show_registers; });
  g_command_palette.register_command("Disassembly", "Show disassembly view", "",
      []() { imgui_state.show_disassembly = !imgui_state.show_disassembly; });
  g_command_palette.register_command("Memory Hex", "Show memory hex view", "",
      []() { imgui_state.show_memory_hex = !imgui_state.show_memory_hex; });
  g_command_palette.register_command("Stack", "Show stack window", "",
      []() { imgui_state.show_stack_window = !imgui_state.show_stack_window; });
  g_command_palette.register_command("Breakpoints", "Show breakpoint list", "",
      []() { imgui_state.show_breakpoint_list = !imgui_state.show_breakpoint_list; });
  g_command_palette.register_command("Symbol Table", "Show symbol table", "",
      []() { imgui_state.show_symbol_table = !imgui_state.show_symbol_table; });
}

// ─────────────────────────────────────────────────
// Main dispatcher
// ─────────────────────────────────────────────────

void imgui_render_ui()
{
  process_pending_dialog();
  imgui_render_topbar();
  if (imgui_state.show_menu)        imgui_render_menu();
  if (imgui_state.show_options)     imgui_render_options();
  if (imgui_state.show_devtools)    imgui_render_devtools();
  if (imgui_state.show_memory_tool) imgui_render_memory_tool();
  if (imgui_state.show_vkeyboard)   imgui_render_vkeyboard();
  // Phase 2 debug windows
  if (imgui_state.show_registers)       imgui_render_registers_window();
  if (imgui_state.show_disassembly)     imgui_render_disassembly_window();
  if (imgui_state.show_memory_hex)      imgui_render_memory_hex_window();
  if (imgui_state.show_stack_window)    imgui_render_stack_window();
  if (imgui_state.show_breakpoint_list) imgui_render_breakpoint_list_window();
  if (imgui_state.show_symbol_table)   imgui_render_symbol_table_window();
  g_command_palette.render();

  // When no GUI window is open, the topbar is the only ImGui window and it
  // doesn't need keyboard input.  Force WantCaptureKeyboard off so all key
  // events reach the emulator.
  bool any_gui_open = imgui_state.show_menu || imgui_state.show_options ||
                      imgui_state.show_devtools || imgui_state.show_memory_tool ||
                      imgui_state.show_vkeyboard ||
                      imgui_state.show_registers || imgui_state.show_disassembly ||
                      imgui_state.show_memory_hex || imgui_state.show_stack_window ||
                      imgui_state.show_breakpoint_list ||
                      imgui_state.show_symbol_table ||
                      g_command_palette.is_open();
  if (!any_gui_open) {
    ImGui::GetIO().WantCaptureKeyboard = false;
  }
}

// ─────────────────────────────────────────────────
// Helper: close menu and resume emulation
// ─────────────────────────────────────────────────

static void close_menu()
{
  imgui_state.show_menu = false;
  imgui_state.show_options = false;
  imgui_state.show_about = false;
  imgui_state.show_quit_confirm = false;
  CPC.paused = false;
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
      int actual_h = static_cast<int>(ImGui::GetWindowSize().y);
      if (actual_h != video_get_topbar_height()) {
        video_set_topbar(nullptr, actual_h);
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
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  ImGui::SetNextWindowSize(ImVec2(260, 0));

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
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
static const char* ram_sizes[] = { "64 KB", "128 KB", "192 KB", "256 KB", "320 KB", "576 KB" };
static int ram_size_values[] = { 64, 128, 192, 256, 320, 576 };

// find_ram_index and find_sample_rate_index moved to imgui_ui_testable.h

static void imgui_render_options()
{
  static bool first_open = true;
  if (first_open) {
    imgui_state.old_cpc_settings = CPC;
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

      ImGui::EndTabItem();
    }

    // ── ROMs Tab ──
    if (ImGui::BeginTabItem("ROMs")) {
      ImGui::Text("Expansion ROM Slots:");
      ImGui::Spacing();
      for (int i = 0; i < 16; i++) {
        char label[32];
        snprintf(label, sizeof(label), "Slot %d", i);
        float col_width = (ImGui::GetContentRegionAvail().x - 8) / 2.0f;
        if (i % 2 != 0) ImGui::SameLine(col_width + 16);

        std::string display = CPC.rom_file[i].empty() ? "(empty)" : CPC.rom_file[i];
        if (display.length() > 20) display = "..." + display.substr(display.length() - 17);

        char btn_label[64];
        snprintf(btn_label, sizeof(btn_label), "%s: %s##rom%d", label, display.c_str(), i);
        if (ImGui::Button(btn_label, ImVec2(col_width, 0))) {
          static const SDL_DialogFileFilter filters[] = { { "ROM files", "rom;bin" } };
          imgui_state.pending_rom_slot = i;
          SDL_ShowOpenFileDialog(file_dialog_callback,
            reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadROM)),
            mainSDLWindow, filters, 1, CPC.rom_path.c_str(), false);
        }
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
        CPC.keyboard != imgui_state.old_cpc_settings.keyboard) {
      emulator_init();
    }
    update_cpc_speed();
    video_set_palette();
    imgui_state.show_options = false;
    first_open = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(80, 0))) {
    CPC = imgui_state.old_cpc_settings;
    imgui_state.show_options = false;
    first_open = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("OK", ImVec2(80, 0))) {
    if (CPC.model != imgui_state.old_cpc_settings.model ||
        CPC.ram_size != imgui_state.old_cpc_settings.ram_size ||
        CPC.keyboard != imgui_state.old_cpc_settings.keyboard) {
      emulator_init();
    }
    update_cpc_speed();
    video_set_palette();
    imgui_state.show_options = false;
    first_open = true;
  }

  if (!open) {
    // Window closed via X button — treat as Cancel
    CPC = imgui_state.old_cpc_settings;
    imgui_state.show_options = false;
    first_open = true;
  }

  ImGui::End();
}

// ─────────────────────────────────────────────────
// DevTools
// ─────────────────────────────────────────────────

// parse_hex, safe_read_word/dword moved to imgui_ui_testable.h

static void devtools_tab_z80()
{
  bool locked = imgui_state.devtools_regs_locked;
  ImGuiInputTextFlags hex_flags = ImGuiInputTextFlags_CharsHexadecimal |
    (locked ? ImGuiInputTextFlags_ReadOnly : 0);

  auto RegField16 = [&](const char* label, reg_pair& rp) {
    unsigned short val = rp.w.l;
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputScalar(label, ImGuiDataType_U16, &val, nullptr, nullptr, "%04X", hex_flags)) {
      if (!locked) rp.w.l = val;
    }
  };

  auto RegField8 = [&](const char* label, byte& val) {
    ImGui::SetNextItemWidth(40);
    unsigned char v = val;
    if (ImGui::InputScalar(label, ImGuiDataType_U8, &v, nullptr, nullptr, "%02X", hex_flags)) {
      if (!locked) val = v;
    }
  };

  // Register grid
  ImGui::Text("Main Registers");
  ImGui::Separator();

  ImGui::Columns(2, "z80_regs", false);
  RegField16("AF", z80.AF); ImGui::NextColumn();
  RegField16("AF'", z80.AFx); ImGui::NextColumn();
  RegField16("BC", z80.BC); ImGui::NextColumn();
  RegField16("BC'", z80.BCx); ImGui::NextColumn();
  RegField16("DE", z80.DE); ImGui::NextColumn();
  RegField16("DE'", z80.DEx); ImGui::NextColumn();
  RegField16("HL", z80.HL); ImGui::NextColumn();
  RegField16("HL'", z80.HLx); ImGui::NextColumn();
  RegField16("IX", z80.IX); ImGui::NextColumn();
  RegField16("IY", z80.IY); ImGui::NextColumn();
  RegField16("SP", z80.SP); ImGui::NextColumn();
  RegField16("PC", z80.PC); ImGui::NextColumn();
  ImGui::Columns(1);

  ImGui::Spacing();
  RegField8("I", z80.I);
  ImGui::SameLine();
  RegField8("R", z80.R);

  // Flags
  ImGui::Spacing();
  ImGui::Text("Flags");
  ImGui::Separator();
  byte f = z80.AF.b.l;
  bool s = f & Sflag, zf = f & Zflag, h = f & Hflag, pv = f & Pflag, n = f & Nflag, cf = f & Cflag;
  ImGui::Checkbox("S (Sign)", &s);     ImGui::SameLine();
  ImGui::Checkbox("Z (Zero)", &zf);    ImGui::SameLine();
  ImGui::Checkbox("H (Half)", &h);
  ImGui::Checkbox("P/V", &pv);         ImGui::SameLine();
  ImGui::Checkbox("N (Neg)", &n);      ImGui::SameLine();
  ImGui::Checkbox("C (Carry)", &cf);
  if (!locked) {
    byte new_f = 0;
    if (s)  new_f |= Sflag;
    if (zf) new_f |= Zflag;
    if (h)  new_f |= Hflag;
    if (pv) new_f |= Pflag;
    if (n)  new_f |= Nflag;
    if (cf) new_f |= Cflag;
    z80.AF.b.l = new_f | (f & Xflags);
  }

  ImGui::Spacing();
  if (ImGui::Button(locked ? "Unlock Registers" : "Lock Registers")) {
    imgui_state.devtools_regs_locked = !imgui_state.devtools_regs_locked;
  }

  // Stack display
  ImGui::Spacing();
  ImGui::Text("Stack (top 16 entries):");
  if (ImGui::BeginChild("##stack", ImVec2(120, 200), ImGuiChildFlags_Borders)) {
    word sp = z80.SP.w.l;
    for (int i = 0; i < 16; i++) {
      word addr = sp + i * 2;
      byte lo = z80_read_mem(addr);
      byte hi = z80_read_mem(addr + 1);
      ImGui::Text("%04X: %04X", addr & 0xFFFF, (hi << 8) | lo);
    }
  }
  ImGui::EndChild();
}

static void devtools_tab_asm()
{
  // Disassemble around PC
  word pc = z80.PC.w.l;
  std::vector<word> eps = { pc };
  DisassembledCode code = disassemble(eps);
  const auto& breakpoints = z80_list_breakpoints_ref();

  ImGui::Text("Disassembly around PC=%04X", pc);

  // Search
  ImGui::SetNextItemWidth(200);
  ImGui::InputText("Search", imgui_state.devtools_search, sizeof(imgui_state.devtools_search));

  if (ImGui::BeginChild("##asm", ImVec2(0, 250), ImGuiChildFlags_Borders)) {
    for (auto& line : code.lines) {
      bool is_pc = (line.address_ == pc);
      bool is_bp = false;
      for (auto& bp : breakpoints) {
        if (bp.address == line.address_) { is_bp = true; break; }
      }

      // Filter by search
      if (imgui_state.devtools_search[0] != '\0') {
        char buf[128];
        snprintf(buf, sizeof(buf), "%04X %s", line.address_, line.instruction_.c_str());
        if (strstr(buf, imgui_state.devtools_search) == nullptr) continue;
      }

      if (is_pc) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
      else if (is_bp) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));

      char label[128];
      snprintf(label, sizeof(label), "%s%04X  %s", is_bp ? "*" : " ", line.address_, line.instruction_.c_str());
      if (ImGui::Selectable(label, is_pc)) {
        // Toggle breakpoint on click
        if (is_bp)
          z80_del_breakpoint(line.address_);
        else
          z80_add_breakpoint(line.address_);
      }

      if (is_pc || is_bp) ImGui::PopStyleColor();
      if (is_pc) ImGui::SetScrollHereY(0.3f);
    }
  }
  ImGui::EndChild();

  // Breakpoint management
  ImGui::Spacing();
  ImGui::SetNextItemWidth(60);
  ImGui::InputText("BP Addr", imgui_state.devtools_bp_addr, sizeof(imgui_state.devtools_bp_addr),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (ImGui::Button("Add BP")) {
    unsigned long addr;
    if (parse_hex(imgui_state.devtools_bp_addr, &addr, 0xFFFF)) {
      z80_add_breakpoint(static_cast<word>(addr));
      imgui_state.devtools_bp_addr[0] = '\0';
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear BPs")) {
    z80_clear_breakpoints();
  }
}

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
  char addr_id[32], val_id[32], btn_id[32];
  snprintf(addr_id, sizeof(addr_id), "Addr##%s", id_suffix);
  snprintf(val_id, sizeof(val_id), "Val##%s", id_suffix);
  snprintf(btn_id, sizeof(btn_id), "Poke##%s", id_suffix);

  ImGui::SetNextItemWidth(50);
  ImGui::InputText(addr_id, addr_buf, addr_size, ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(40);
  ImGui::InputText(val_id, val_buf, val_size, ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();

  if (ImGui::Button(btn_id)) {
    unsigned long addr, val;
    if (parse_hex(addr_buf, &addr, 0xFFFF) && parse_hex(val_buf, &val, 0xFF)) {
      pbRAM[addr] = static_cast<byte>(val);
      return true;
    }
  }
  return false;
}

static void devtools_tab_memory()
{
  // Poke
  ui_poke_input(imgui_state.devtools_poke_addr, sizeof(imgui_state.devtools_poke_addr),
                imgui_state.devtools_poke_val, sizeof(imgui_state.devtools_poke_val), "dtpoke");

  // Display address
  ImGui::SetNextItemWidth(50);
  ImGui::InputText("Display##dt", imgui_state.devtools_display_addr, sizeof(imgui_state.devtools_display_addr),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (ImGui::Button("Go##dt")) {
    unsigned long addr;
    if (parse_hex(imgui_state.devtools_display_addr, &addr, 0xFFFF))
      imgui_state.devtools_display_value = static_cast<int>(addr);
    else
      imgui_state.devtools_display_value = -1;
  }

  // Bytes per line & format
  const char* bpl_items[] = { "1", "4", "8", "16", "32" };
  int bpl_values[] = { 1, 4, 8, 16, 32 };
  int bpl_idx = 3;
  for (int i = 0; i < 5; i++) { if (bpl_values[i] == imgui_state.devtools_bytes_per_line) bpl_idx = i; }
  ImGui::SetNextItemWidth(60);
  if (ImGui::Combo("Bytes/Line##dt", &bpl_idx, bpl_items, 5)) {
    imgui_state.devtools_bytes_per_line = bpl_values[bpl_idx];
  }
  ImGui::SameLine();
  const char* fmt_items[] = { "Hex", "Hex & char", "Hex & u8" };
  ImGui::SetNextItemWidth(100);
  ImGui::Combo("Format##dt", &imgui_state.devtools_mem_format, fmt_items, 3);

  // Hex dump
  if (ImGui::BeginChild("##dtmem", ImVec2(0, 250), ImGuiChildFlags_Borders)) {
    int bpl = imgui_state.devtools_bytes_per_line;
    int start_line = 0;
    int total_lines = 65536 / bpl;

    if (imgui_state.devtools_display_value >= 0 && imgui_state.devtools_display_value <= 0xFFFF) {
      start_line = imgui_state.devtools_display_value / bpl;
    }

    // Use clipper for performance
    ImGuiListClipper clipper;
    clipper.Begin(total_lines);
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
        char line[512];
        format_memory_line(line, sizeof(line), i * bpl, bpl, imgui_state.devtools_mem_format);
        ImGui::TextUnformatted(line);
      }
    }

    if (start_line > 0) {
      float scroll_y = (static_cast<float>(start_line) / total_lines) * ImGui::GetScrollMaxY();
      ImGui::SetScrollY(scroll_y);
      imgui_state.devtools_display_value = -1; // only scroll once
    }
  }
  ImGui::EndChild();

  // Watchpoints
  ImGui::Spacing();
  ImGui::Text("RAM Config: %d  Bank: %d", GateArray.RAM_config, GateArray.RAM_bank);
}

static void devtools_tab_video()
{
  ImGui::Text("CRTC Registers");
  ImGui::Separator();
  const char* crtc_names[] = {
    "R0: H Total", "R1: H Displayed", "R2: H Sync Pos", "R3: Sync Widths",
    "R4: V Total", "R5: V Total Adj", "R6: V Displayed", "R7: V Sync Pos",
    "R8: Interlace", "R9: Max Raster", "R10: Cursor Start", "R11: Cursor End",
    "R12: Start Addr H", "R13: Start Addr L", "R14: Cursor H", "R15: Cursor L",
    "R16: LPEN H", "R17: LPEN L"
  };

  for (int i = 0; i < 18; i++) {
    unsigned char val = CRTC.registers[i];
    ImGui::SetNextItemWidth(50);
    ImGui::InputScalar(crtc_names[i], ImGuiDataType_U8, &val, nullptr, nullptr, "%02X",
                       ImGuiInputTextFlags_ReadOnly);
  }

  ImGui::Spacing();
  ImGui::Text("Gate Array");
  ImGui::Separator();
  ImGui::Text("Screen Mode: %d", GateArray.scr_mode);
  ImGui::Text("ROM Config: %02X", GateArray.ROM_config);
  ImGui::Text("RAM Config: %02X", GateArray.RAM_config);
  ImGui::Text("Pen: %d", GateArray.pen);
}

static void devtools_tab_audio()
{
  ImGui::Text("PSG (AY-3-8912) Registers");
  ImGui::Separator();

  if (ImGui::BeginTable("##psg", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Channel");
    ImGui::TableSetupColumn("Tone Freq");
    ImGui::TableSetupColumn("Volume");
    ImGui::TableSetupColumn("Tone/Noise");
    ImGui::TableHeadersRow();

    auto row = [](const char* ch, unsigned short tone, unsigned char amp, bool tone_on, bool noise_on) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%s", ch);
      ImGui::TableNextColumn(); ImGui::Text("%d", tone & 0xFFF);
      ImGui::TableNextColumn(); ImGui::Text("%d", amp & 0x1F);
      ImGui::TableNextColumn(); ImGui::Text("%s/%s", tone_on ? "ON" : "off", noise_on ? "ON" : "off");
    };

    byte mixer = PSG.RegisterAY.Mixer;
    row("A", PSG.RegisterAY.TonA, PSG.RegisterAY.AmplitudeA, !(mixer & 1), !(mixer & 8));
    row("B", PSG.RegisterAY.TonB, PSG.RegisterAY.AmplitudeB, !(mixer & 2), !(mixer & 16));
    row("C", PSG.RegisterAY.TonC, PSG.RegisterAY.AmplitudeC, !(mixer & 4), !(mixer & 32));
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::Text("Noise Freq: %d", PSG.RegisterAY.Noise & 0x1F);
  ImGui::Text("Envelope: %d (Type: %d)", PSG.RegisterAY.Envelope, PSG.RegisterAY.EnvType);
}

static void imgui_render_devtools()
{
  ImGui::SetNextWindowSize(ImVec2(560, 500), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("DevTools", &open,
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar)) {
    if (!open) imgui_state.show_devtools = false;
    ImGui::End();
    return;
  }

  // Toolbar
  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("Debug")) {
      ImGui::MenuItem("Registers",       nullptr, &imgui_state.show_registers);
      ImGui::MenuItem("Disassembly",     nullptr, &imgui_state.show_disassembly);
      ImGui::MenuItem("Memory Hex",      nullptr, &imgui_state.show_memory_hex);
      ImGui::MenuItem("Stack",           nullptr, &imgui_state.show_stack_window);
      ImGui::MenuItem("Breakpoints/WP",  nullptr, &imgui_state.show_breakpoint_list);
      ImGui::MenuItem("Symbols",         nullptr, &imgui_state.show_symbol_table);
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::Button("Step In"))  { z80.step_in = 1; CPC.paused = false; }
    if (ImGui::Button("Step Over")) {
      // Step over = set ephemeral breakpoint at next instruction
      word pc = z80.PC.w.l;
      // Read instruction length by disassembling one
      std::vector<word> eps = { pc };
      DisassembledCode dc = disassemble(eps);
      auto it = dc.lines.begin();
      if (it != dc.lines.end()) {
        auto next_it = std::next(it);
        if (next_it != dc.lines.end()) {
          z80_add_breakpoint_ephemeral(next_it->address_);
        }
      }
      CPC.paused = false;
    }
    if (ImGui::Button("Step Out")) { z80.step_out = 1; CPC.paused = false; }
    ImGui::Separator();
    if (ImGui::Button(CPC.paused ? "Resume" : "Pause")) {
      CPC.paused = !CPC.paused;
    }
    ImGui::EndMenuBar();
  }

  if (ImGui::BeginTabBar("DevToolsTabs")) {
    if (ImGui::BeginTabItem("Z80"))    { devtools_tab_z80();    ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Asm"))    { devtools_tab_asm();    ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Memory")) { devtools_tab_memory(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Video"))  { devtools_tab_video();  ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Audio"))  { devtools_tab_audio();  ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }

  if (!open) {
    imgui_state.show_devtools = false;
  }

  ImGui::End();
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

// ─────────────────────────────────────────────────
// Debug Window 1: Registers
// ─────────────────────────────────────────────────

static void imgui_render_registers_window()
{
  ImGui::SetNextWindowSize(ImVec2(340, 420), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(620, 30), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Registers", &open)) {
    if (!open) imgui_state.show_registers = false;
    ImGui::End();
    return;
  }

  bool locked = !CPC.paused;
  ImGuiInputTextFlags hex_flags = ImGuiInputTextFlags_CharsHexadecimal |
    (locked ? ImGuiInputTextFlags_ReadOnly : 0);

  auto RegField16 = [&](const char* label, reg_pair& rp) {
    unsigned short val = rp.w.l;
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputScalar(label, ImGuiDataType_U16, &val, nullptr, nullptr, "%04X", hex_flags)) {
      if (!locked) rp.w.l = val;
    }
  };

  auto RegField8 = [&](const char* label, byte& val) {
    ImGui::SetNextItemWidth(40);
    unsigned char v = val;
    if (ImGui::InputScalar(label, ImGuiDataType_U8, &v, nullptr, nullptr, "%02X", hex_flags)) {
      if (!locked) val = v;
    }
  };

  // Main registers in two columns
  ImGui::Columns(2, "regs_main", false);
  RegField16("AF", z80.AF); ImGui::NextColumn();
  RegField16("AF'", z80.AFx); ImGui::NextColumn();
  RegField16("BC", z80.BC); ImGui::NextColumn();
  RegField16("BC'", z80.BCx); ImGui::NextColumn();
  RegField16("DE", z80.DE); ImGui::NextColumn();
  RegField16("DE'", z80.DEx); ImGui::NextColumn();
  RegField16("HL", z80.HL); ImGui::NextColumn();
  RegField16("HL'", z80.HLx); ImGui::NextColumn();
  RegField16("IX", z80.IX); ImGui::NextColumn();
  RegField16("IY", z80.IY); ImGui::NextColumn();
  RegField16("SP", z80.SP); ImGui::NextColumn();
  RegField16("PC", z80.PC); ImGui::NextColumn();
  ImGui::Columns(1);

  ImGui::Spacing();
  RegField8("I", z80.I);
  ImGui::SameLine();
  RegField8("R", z80.R);

  // Interrupt state
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text("IFF1: %d  IFF2: %d  IM: %d  HALT: %d",
              z80.IFF1, z80.IFF2, z80.IM, z80.HALT);
  ImGui::Text("T-states: %llu", static_cast<unsigned long long>(g_tstate_counter));

  // Flags
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text("Flags");
  byte f = z80.AF.b.l;
  bool s = f & Sflag, zf = f & Zflag, h = f & Hflag, pv = f & Pflag, n = f & Nflag, cf = f & Cflag;
  ImGui::Checkbox("S", &s);   ImGui::SameLine();
  ImGui::Checkbox("Z", &zf);  ImGui::SameLine();
  ImGui::Checkbox("H", &h);   ImGui::SameLine();
  ImGui::Checkbox("P/V", &pv); ImGui::SameLine();
  ImGui::Checkbox("N", &n);   ImGui::SameLine();
  ImGui::Checkbox("C", &cf);
  if (!locked) {
    byte new_f = 0;
    if (s)  new_f |= Sflag;
    if (zf) new_f |= Zflag;
    if (h)  new_f |= Hflag;
    if (pv) new_f |= Pflag;
    if (n)  new_f |= Nflag;
    if (cf) new_f |= Cflag;
    z80.AF.b.l = new_f | (f & Xflags);
  }

  if (!open) imgui_state.show_registers = false;
  ImGui::End();
}

// ─────────────────────────────────────────────────
// Debug Window 2: Disassembly
// ─────────────────────────────────────────────────

static void imgui_render_disassembly_window()
{
  ImGui::SetNextWindowSize(ImVec2(440, 500), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Disassembly", &open, ImGuiWindowFlags_MenuBar)) {
    if (!open) imgui_state.show_disassembly = false;
    ImGui::End();
    return;
  }

  // Toolbar
  if (ImGui::BeginMenuBar()) {
    ImGui::Checkbox("Follow PC", &imgui_state.disasm_follow_pc);
    ImGui::Separator();
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputText("Goto", imgui_state.disasm_goto_addr,
        sizeof(imgui_state.disasm_goto_addr),
        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
      unsigned long addr;
      if (parse_hex(imgui_state.disasm_goto_addr, &addr, 0xFFFF)) {
        imgui_state.disasm_goto_value = static_cast<int>(addr);
        imgui_state.disasm_follow_pc = false;
      }
    }
    ImGui::EndMenuBar();
  }

  // Determine the center address
  word center_pc = z80.PC.w.l;
  if (!imgui_state.disasm_follow_pc && imgui_state.disasm_goto_value >= 0) {
    center_pc = static_cast<word>(imgui_state.disasm_goto_value);
  }

  // Disassemble ~48 instructions starting from an estimated position before center
  // We go back ~20 instructions (estimate 2 bytes avg) then forward
  word start_addr = center_pc - 40; // rough estimate, will wrap around on 16-bit
  constexpr int NUM_LINES = 48;

  // Linear disassembly from start_addr
  DisassembledCode dummy_dc;
  std::vector<dword> dummy_eps;
  struct DisasmEntry {
    word addr;
    std::string text;
    std::string label;
  };
  std::vector<DisasmEntry> lines;
  lines.reserve(NUM_LINES);

  word addr = start_addr;
  for (int i = 0; i < NUM_LINES; i++) {
    DisasmEntry entry;
    entry.addr = addr;

    // Check for symbol at this address
    entry.label = g_symfile.lookupAddr(addr);

    auto line = disassemble_one(addr, dummy_dc, dummy_eps);
    entry.text = line.instruction_;
    int len = line.Size();
    if (len <= 0) len = 1; // safety: advance at least 1 byte
    addr = (addr + len) & 0xFFFF;
    lines.push_back(std::move(entry));
  }

  const auto& breakpoints = z80_list_breakpoints_ref();

  if (ImGui::BeginChild("##disasm_scroll", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    int scroll_to_idx = -1;

    for (int i = 0; i < static_cast<int>(lines.size()); i++) {
      const auto& entry = lines[i];
      bool is_pc = (entry.addr == z80.PC.w.l);
      bool is_bp = false;
      for (const auto& bp : breakpoints) {
        if (bp.address == entry.addr && bp.type != EPHEMERAL) { is_bp = true; break; }
      }

      // Symbol label above the instruction
      if (!entry.label.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
        ImGui::Text("  %s:", entry.label.c_str());
        ImGui::PopStyleColor();
      }

      // Build display text
      char label[160];
      snprintf(label, sizeof(label), "%s %04X  %s",
               is_bp ? "\xe2\x97\x8f" : " ",  // ● marker for breakpoint
               entry.addr, entry.text.c_str());

      // Color: green for PC, red for breakpoint
      if (is_pc) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
      } else if (is_bp) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
      }

      if (ImGui::Selectable(label, is_pc)) {
        // Left click: toggle breakpoint
        if (is_bp)
          z80_del_breakpoint(entry.addr);
        else
          z80_add_breakpoint(entry.addr);
      }

      // Right-click context menu
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Run to here")) {
          z80_add_breakpoint_ephemeral(entry.addr);
          CPC.paused = false;
        }
        if (ImGui::MenuItem("Set PC here")) {
          z80.PC.w.l = entry.addr;
        }
        if (ImGui::MenuItem("Goto this address")) {
          imgui_state.disasm_goto_value = entry.addr;
          imgui_state.disasm_follow_pc = false;
          snprintf(imgui_state.disasm_goto_addr, sizeof(imgui_state.disasm_goto_addr),
                   "%04X", entry.addr);
        }
        ImGui::EndPopup();
      }

      if (is_pc || is_bp) ImGui::PopStyleColor();

      // Track the PC line for auto-scroll
      if (is_pc) scroll_to_idx = i;
    }

    // Auto-scroll to PC line
    if (imgui_state.disasm_follow_pc && scroll_to_idx >= 0) {
      float item_height = ImGui::GetTextLineHeightWithSpacing();
      ImGui::SetScrollY(scroll_to_idx * item_height - ImGui::GetWindowHeight() * 0.3f);
    }
  }
  ImGui::EndChild();

  if (!open) imgui_state.show_disassembly = false;
  ImGui::End();
}

// ─────────────────────────────────────────────────
// Debug Window 3: Memory Hex Dump
// ─────────────────────────────────────────────────

static void imgui_render_memory_hex_window()
{
  ImGui::SetNextWindowSize(ImVec2(520, 400), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(460, 30), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Memory Hex", &open, ImGuiWindowFlags_MenuBar)) {
    if (!open) imgui_state.show_memory_hex = false;
    ImGui::End();
    return;
  }

  // Toolbar
  if (ImGui::BeginMenuBar()) {
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputText("Goto##memhex", imgui_state.memhex_goto_addr,
        sizeof(imgui_state.memhex_goto_addr),
        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
      unsigned long addr;
      if (parse_hex(imgui_state.memhex_goto_addr, &addr, 0xFFFF)) {
        imgui_state.memhex_goto_value = static_cast<int>(addr);
      }
    }
    ImGui::Separator();
    ImGui::Text("W:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(40);
    int bpr = imgui_state.memhex_bytes_per_row;
    if (ImGui::InputInt("##bpr", &bpr, 0, 0)) {
      if (bpr >= 4 && bpr <= 32) imgui_state.memhex_bytes_per_row = bpr;
    }
    ImGui::EndMenuBar();
  }

  int bytes_per_row = imgui_state.memhex_bytes_per_row;
  int total_rows = (0x10000 + bytes_per_row - 1) / bytes_per_row;

  // Collect watchpoint ranges for highlighting
  const auto& watchpoints = z80_list_watchpoints_ref();

  if (ImGui::BeginChild("##hexview", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    // Use clipper for efficient scrolling over all 64K
    ImGuiListClipper clipper;
    clipper.Begin(total_rows);

    // Handle goto
    if (imgui_state.memhex_goto_value >= 0) {
      int target_row = imgui_state.memhex_goto_value / bytes_per_row;
      float item_height = ImGui::GetTextLineHeightWithSpacing();
      ImGui::SetScrollY(target_row * item_height);
      imgui_state.memhex_goto_value = -1;
    }

    while (clipper.Step()) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
        unsigned int base_addr = row * bytes_per_row;

        // Build the full hex line with watchpoint coloring
        // Address
        ImGui::Text("%04X:", base_addr & 0xFFFF);

        // Hex bytes with watchpoint highlighting
        for (int col = 0; col < bytes_per_row; col++) {
          word a = (base_addr + col) & 0xFFFF;
          byte val = z80_read_mem(a);

          ImGui::SameLine();

          // Check watchpoint highlighting
          bool wp_r = false, wp_w = false;
          for (const auto& wp : watchpoints) {
            // Handle non-wrapping case
            if (wp.length > 0 && a >= wp.address && a < wp.address + wp.length) {
              if (wp.type == READ || wp.type == READWRITE) wp_r = true;
              if (wp.type == WRITE || wp.type == READWRITE) wp_w = true;
            }
          }

          bool colored = wp_r || wp_w;
          if (wp_r && wp_w) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.2f, 1.0f));
          } else if (wp_w) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
          } else if (wp_r) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
          }

          ImGui::Text("%02X", val);

          if (colored) ImGui::PopStyleColor();
        }

        // ASCII column
        ImGui::SameLine();
        char ascii[33];
        int asc_len = bytes_per_row < 32 ? bytes_per_row : 32;
        for (int col = 0; col < asc_len; col++) {
          byte b = z80_read_mem((base_addr + col) & 0xFFFF);
          ascii[col] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
        }
        ascii[asc_len] = '\0';
        ImGui::Text("|%s|", ascii);
      }
    }
    clipper.End();
  }
  ImGui::EndChild();

  if (!open) imgui_state.show_memory_hex = false;
  ImGui::End();
}

// ─────────────────────────────────────────────────
// Debug Window 4: Stack
// ─────────────────────────────────────────────────

static void imgui_render_stack_window()
{
  ImGui::SetNextWindowSize(ImVec2(260, 400), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(460, 440), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Stack", &open)) {
    if (!open) imgui_state.show_stack_window = false;
    ImGui::End();
    return;
  }

  word sp = z80.SP.w.l;
  ImGui::Text("SP = %04X", sp);
  ImGui::Separator();

  if (ImGui::BeginChild("##stack_entries", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    constexpr int MAX_DEPTH = 32;
    for (int i = 0; i < MAX_DEPTH; i++) {
      word addr = (sp + i * 2) & 0xFFFF;
      byte lo = z80_read_mem(addr);
      byte hi = z80_read_mem((addr + 1) & 0xFFFF);
      word value = (hi << 8) | lo;

      // Heuristic: check if the instruction before `value` is a CALL or RST
      bool is_ret_addr = false;
      // CALL nn is 3 bytes (CD xx xx), CALL cc,nn also 3 bytes
      if (value >= 3) {
        byte op3 = z80_read_mem((value - 3) & 0xFFFF);
        if (op3 == 0xCD || (op3 & 0xC7) == 0xC4) is_ret_addr = true;
      }
      // RST n is 1 byte: C7/CF/D7/DF/E7/EF/F7/FF
      if (value >= 1 && !is_ret_addr) {
        byte op1 = z80_read_mem((value - 1) & 0xFFFF);
        if ((op1 & 0xC7) == 0xC7) is_ret_addr = true;
      }

      // Look up symbol
      std::string sym = g_symfile.lookupAddr(value);

      if (is_ret_addr) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.0f, 0.5f, 1.0f));
      }

      std::string line;
      if (!sym.empty()) {
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "SP+%02X: %04X %s", i * 2, value,
                 is_ret_addr ? "[call] " : "");
        line = std::string(prefix) + sym;
      } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "SP+%02X: %04X%s",
                 i * 2, value, is_ret_addr ? "  [call]" : "");
        line = buf;
      }

      // Double-click navigates disassembly
      if (ImGui::Selectable(line.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(0)) {
          imgui_state.disasm_goto_value = value;
          imgui_state.disasm_follow_pc = false;
          snprintf(imgui_state.disasm_goto_addr, sizeof(imgui_state.disasm_goto_addr),
                   "%04X", value);
          imgui_state.show_disassembly = true;
        }
      }

      if (is_ret_addr) ImGui::PopStyleColor();
    }
  }
  ImGui::EndChild();

  if (!open) imgui_state.show_stack_window = false;
  ImGui::End();
}

// ─────────────────────────────────────────────────
// Debug Window 5: Breakpoint / Watchpoint List
// ─────────────────────────────────────────────────

static void imgui_render_breakpoint_list_window()
{
  ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(10, 540), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin("Breakpoints & Watchpoints", &open)) {
    if (!open) imgui_state.show_breakpoint_list = false;
    ImGui::End();
    return;
  }

  // Clear all buttons
  if (ImGui::Button("Clear All BPs")) z80_clear_breakpoints();
  ImGui::SameLine();
  if (ImGui::Button("Clear All WPs")) z80_clear_watchpoints();
  ImGui::SameLine();
  if (ImGui::Button("Clear All IOBPs")) z80_clear_io_breakpoints();
  ImGui::Separator();

  const auto& bps = z80_list_breakpoints_ref();
  const auto& wps = z80_list_watchpoints_ref();
  const auto& iobps = z80_list_io_breakpoints_ref();

  if (ImGui::BeginTable("bpwp_table", 5,
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("Address/Port", ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableSetupColumn("Condition", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 40);
    ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 20);
    ImGui::TableHeadersRow();

    // Breakpoints
    for (size_t i = 0; i < bps.size(); i++) {
      const auto& bp = bps[i];
      if (bp.type == EPHEMERAL) continue;

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("BP");
      ImGui::TableSetColumnIndex(1);
      std::string sym = g_symfile.lookupAddr(bp.address);
      if (!sym.empty())
        ImGui::Text("%04X %s", bp.address, sym.c_str());
      else
        ImGui::Text("%04X", bp.address);
      ImGui::TableSetColumnIndex(2);
      if (!bp.condition_str.empty())
        ImGui::TextUnformatted(bp.condition_str.c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%d", bp.hit_count);
      ImGui::TableSetColumnIndex(4);
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::SmallButton("X")) {
        z80_del_breakpoint(bp.address);
      }
      ImGui::PopID();
    }

    // Watchpoints
    for (size_t i = 0; i < wps.size(); i++) {
      const auto& wp = wps[i];
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      const char* wp_type = (wp.type == READ) ? "WP/R" :
                            (wp.type == WRITE) ? "WP/W" : "WP/RW";
      ImGui::Text("%s", wp_type);
      ImGui::TableSetColumnIndex(1);
      if (wp.length > 1)
        ImGui::Text("%04X+%d", static_cast<word>(wp.address), wp.length);
      else
        ImGui::Text("%04X", static_cast<word>(wp.address));
      ImGui::TableSetColumnIndex(2);
      if (!wp.condition_str.empty())
        ImGui::TextUnformatted(wp.condition_str.c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%d", wp.hit_count);
      ImGui::TableSetColumnIndex(4);
      ImGui::PushID(1000 + static_cast<int>(i));
      if (ImGui::SmallButton("X")) {
        z80_del_watchpoint(static_cast<int>(i));
      }
      ImGui::PopID();
    }

    // IO Breakpoints
    for (size_t i = 0; i < iobps.size(); i++) {
      const auto& iobp = iobps[i];
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      const char* dir_str = (iobp.dir == IO_IN) ? "IO/IN" :
                            (iobp.dir == IO_OUT) ? "IO/OUT" : "IO/RW";
      ImGui::Text("%s", dir_str);
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%04X/%04X", iobp.port, iobp.mask);
      ImGui::TableSetColumnIndex(2);
      if (!iobp.condition_str.empty())
        ImGui::TextUnformatted(iobp.condition_str.c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("-");
      ImGui::TableSetColumnIndex(4);
      ImGui::PushID(2000 + static_cast<int>(i));
      if (ImGui::SmallButton("X")) {
        z80_del_io_breakpoint(static_cast<int>(i));
      }
      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  if (!open) imgui_state.show_breakpoint_list = false;
  ImGui::End();
}

// ─────────────────────────────────────────────────
// Symbol Table Viewer
// ─────────────────────────────────────────────────
static void imgui_render_symbol_table_window()
{
  auto syms = g_symfile.listSymbols(imgui_state.symtable_filter);

  char title[64];
  snprintf(title, sizeof(title), "Symbols (%d)###SymbolTable", static_cast<int>(syms.size()));

  ImGui::SetNextWindowSize(ImVec2(340, 400), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(520, 540), ImGuiCond_FirstUseEver);

  bool open = true;
  if (!ImGui::Begin(title, &open)) {
    if (!open) imgui_state.show_symbol_table = false;
    ImGui::End();
    return;
  }

  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##symfilter", "Filter...", imgui_state.symtable_filter,
                           sizeof(imgui_state.symtable_filter));
  ImGui::Separator();

  if (ImGui::BeginTable("sym_table", 3,
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
      ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 20);
    ImGui::TableHeadersRow();

    for (const auto& [addr, name] : syms) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      if (ImGui::Selectable(nullptr, false, ImGuiSelectableFlags_SpanAllColumns)) {
        // Navigate disassembly to this address
        imgui_state.show_disassembly = true;
        imgui_state.disasm_follow_pc = false;
        imgui_state.disasm_goto_value = addr;
      }
      ImGui::SameLine();
      ImGui::Text("%04X", addr);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(name.c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::PushID(static_cast<int>(addr));
      if (ImGui::SmallButton("X")) {
        g_symfile.delSymbol(name);
      }
      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  if (!open) imgui_state.show_symbol_table = false;
  ImGui::End();
}