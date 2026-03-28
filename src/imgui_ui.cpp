#include "imgui_ui.h"
#include "devtools_ui.h"
#include "imgui_ui_testable.h"
#include "imgui.h"
#include "command_palette.h"
#include "menu_actions.h"
#include "workspace_layout.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <iostream>

#include "koncepcja.h"
#include "log.h"
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
#include "m4board_http.h"
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
static void imgui_render_menubar();
static void imgui_render_topbar();
static void imgui_render_statusbar();
static void imgui_render_menu();
static void imgui_render_options();
static void imgui_render_devtools();
static void imgui_render_memory_tool();
static void imgui_render_vkeyboard();

// Declared in imgui_ui.h — close menu and unpause unless a dialog is open
static void mru_push(std::vector<std::string>& list, const std::string& path);

// Height tracking for stacked menubar + topbar + devtools bar
static float s_menubar_h = 19.0f; // ImGui main menu bar default height
static int s_main_topbar_h = 25;
static int s_devtools_bar_h = 0;
static ImVec2 s_layout_btn_pos;  // set in topbar, read in layout dropdown
static bool s_topbar_height_dirty = false; // defer SDL_SetWindowSize to after render
static int s_statusbar_h = 0;
static bool s_bottombar_height_dirty = false; // defer SDL_SetWindowSize to after render

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
  auto fname = std::filesystem::path(path).filename().string();

  switch (action) {
    case FileDialogAction::LoadDiskA:
    case FileDialogAction::LoadDiskA_LED:
      CPC.driveA.file = path;
      if (file_load(CPC.driveA) == 0) {
        imgui_toast_success("Drive A: " + fname);
        mru_push(CPC.mru_disks, path);
      } else
        imgui_toast_error("Failed to load disk: " + fname);
      CPC.current_dsk_path = dir;
      if (action == FileDialogAction::LoadDiskA) imgui_close_menu();
      break;
    case FileDialogAction::LoadDiskB:
    case FileDialogAction::LoadDiskB_LED:
      CPC.driveB.file = path;
      if (file_load(CPC.driveB) == 0) {
        imgui_toast_success("Drive B: " + fname);
        mru_push(CPC.mru_disks, path);
      } else
        imgui_toast_error("Failed to load disk: " + fname);
      CPC.current_dsk_path = dir;
      if (action == FileDialogAction::LoadDiskB) imgui_close_menu();
      break;
    case FileDialogAction::SaveDiskA:
      if (dsk_save(path, &driveA) == 0)
        imgui_toast_success("Saved disk A: " + fname);
      else
        imgui_toast_error("Failed to save disk: " + fname);
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::SaveDiskB:
      if (dsk_save(path, &driveB) == 0)
        imgui_toast_success("Saved disk B: " + fname);
      else
        imgui_toast_error("Failed to save disk: " + fname);
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::LoadSnapshot:
      CPC.snapshot.file = path;
      if (file_load(CPC.snapshot) == 0) {
        imgui_toast_success("Snapshot loaded: " + fname);
        mru_push(CPC.mru_snaps, path);
      } else
        imgui_toast_error("Failed to load snapshot: " + fname);
      CPC.current_snap_path = dir;
      imgui_close_menu();
      break;
    case FileDialogAction::SaveSnapshot:
      if (snapshot_save(path) == 0)
        imgui_toast_success("Snapshot saved: " + fname);
      else
        imgui_toast_error("Failed to save snapshot: " + fname);
      CPC.current_snap_path = dir;
      break;
    case FileDialogAction::LoadTape:
      CPC.tape.file = path;
      if (file_load(CPC.tape) == 0) {
        imgui_toast_success("Tape loaded: " + fname);
        mru_push(CPC.mru_tapes, path);
        tape_scan_blocks();
      } else
        imgui_toast_error("Failed to load tape: " + fname);
      CPC.current_tape_path = dir;
      imgui_close_menu();
      break;
    case FileDialogAction::LoadTape_LED:
      CPC.tape.file = path;
      if (file_load(CPC.tape) == 0) {
        imgui_toast_success("Tape loaded: " + fname);
        mru_push(CPC.mru_tapes, path);
        tape_scan_blocks();
      } else
        imgui_toast_error("Failed to load tape: " + fname);
      CPC.current_tape_path = dir;
      break;
    case FileDialogAction::LoadCartridge:
      CPC.cartridge.file = path;
      if (file_load(CPC.cartridge) == 0) {
        imgui_toast_success("Cartridge loaded: " + fname);
        mru_push(CPC.mru_carts, path);
      } else
        imgui_toast_error("Failed to load cartridge: " + fname);
      CPC.current_cart_path = dir;
      emulator_reset();
      imgui_close_menu();
      break;
    case FileDialogAction::LoadROM:
      if (rom_slot >= 0 && rom_slot < MAX_ROM_SLOTS)
        CPC.rom_file[rom_slot] = path;
      break;
    case FileDialogAction::SelectM4SDFolder:
      g_m4board.sd_root_path = path;
      if (g_m4board.enabled) emulator_init();
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

  // Prevent accidental dragging of windows by clicking on their body.
  // Only the title bar can be used to move windows.
  io.ConfigWindowsMoveFromTitleBarOnly = true;

#if defined(__APPLE__) || defined(_WIN32)
  // Merge a system symbol font for transport control glyphs (play/stop/eject etc.)
  {
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
#ifdef __APPLE__
    io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Apple Symbols.ttf", 13.0f, &merge_cfg, symbol_ranges);
#elif defined(_WIN32)
    {
      std::filesystem::path fonts_dir = "C:\\Windows\\Fonts";
      if (const char* sys_root = getenv("SystemRoot")) {
        fonts_dir = std::filesystem::path(sys_root) / "Fonts";
      }
      // Try Segoe UI Symbol first, then Segoe UI Emoji, then Arial Unicode MS
      const char* candidates[] = { "seguisym.ttf", "seguiemj.ttf", "ARIALUNI.TTF" };
      for (const char* name : candidates) {
        auto path = (fonts_dir / name).string();
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), 13.0f, &merge_cfg, symbol_ranges))
          break;
      }
    }
#endif
  }
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
  g_command_palette.register_command("Assembler", "Z80 assembler IDE", "",
      []() { g_devtools_ui.toggle_window("assembler"); });
}

// ─────────────────────────────────────────────────
// Main dispatcher
// ─────────────────────────────────────────────────

void imgui_render_ui()
{
  // Reconcile ImGui mouse state with hardware — defense against stuck buttons.
  // Only poll hardware state when ImGui thinks a button is down, to avoid the
  // overhead of SDL_GetGlobalMouseState() every frame (slow on X11).
  {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2]) {
      SDL_MouseButtonFlags hw_buttons = SDL_GetGlobalMouseState(nullptr, nullptr);
      // SDL button indices: 1=Left, 2=Middle, 3=Right
      // ImGui button indices: 0=Left, 1=Right, 2=Middle
      static const int sdl_button[] = { SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE };
      for (int i = 0; i < 3; i++) {
        bool hw_down = (hw_buttons & SDL_BUTTON_MASK(sdl_button[i])) != 0;
        if (io.MouseDown[i] && !hw_down) {
          io.MouseDown[i] = false;
        }
      }
    }
  }

  process_pending_dialog();
  // Dockspace host must be rendered before other windows so they can dock into it
  workspace_render_dockspace();
  workspace_render_cpc_screen();
  imgui_render_menubar();
  imgui_render_topbar();
  imgui_render_statusbar();
  if (imgui_state.show_menu)        imgui_render_menu();
  if (imgui_state.show_options)     imgui_render_options();
  if (imgui_state.show_devtools)    imgui_render_devtools();
  if (imgui_state.show_memory_tool) imgui_render_memory_tool();
  if (imgui_state.show_vkeyboard)   imgui_render_vkeyboard();
  // Phase 2 debug windows (extracted to DevToolsUI)
  g_devtools_ui.render();
  g_command_palette.render();

  // ── Toast notifications ──
  {
    ImGuiIO& io = ImGui::GetIO();
    float dt = io.DeltaTime;
    float yOffset = 40.0f; // bottom margin
    float xMargin = 16.0f;
    float maxWidth = 360.0f;

    // Tick timers and remove expired
    for (auto it = imgui_state.toasts.begin(); it != imgui_state.toasts.end(); ) {
      it->timer -= dt;
      if (it->timer <= 0.0f) {
        it = imgui_state.toasts.erase(it);
      } else {
        ++it;
      }
    }

    // Render from bottom of viewport, stacking upward
    ImVec2 vpPos = ImGui::GetMainViewport()->Pos;
    ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    for (int i = static_cast<int>(imgui_state.toasts.size()) - 1; i >= 0; --i) {
      auto& t = imgui_state.toasts[i];

      // Fade in/out
      float alpha = 1.0f;
      if (t.timer < ImGuiUIState::TOAST_FADE_TIME)
        alpha = t.timer / ImGuiUIState::TOAST_FADE_TIME;
      float age = t.initial - t.timer;
      if (age < ImGuiUIState::TOAST_FADE_TIME)
        alpha = std::min(alpha, age / ImGuiUIState::TOAST_FADE_TIME);

      // Colors by level
      ImU32 bgCol, borderCol, textCol;
      switch (t.level) {
        case ImGuiUIState::ToastLevel::Success:
          bgCol     = IM_COL32(0x10, 0x30, 0x18, static_cast<int>(210 * alpha));
          borderCol = IM_COL32(0x20, 0x90, 0x40, static_cast<int>(200 * alpha));
          textCol   = IM_COL32(0x80, 0xFF, 0x80, static_cast<int>(255 * alpha));
          break;
        case ImGuiUIState::ToastLevel::Error:
          bgCol     = IM_COL32(0x30, 0x10, 0x10, static_cast<int>(210 * alpha));
          borderCol = IM_COL32(0x90, 0x20, 0x20, static_cast<int>(200 * alpha));
          textCol   = IM_COL32(0xFF, 0x80, 0x80, static_cast<int>(255 * alpha));
          break;
        default: // Info
          bgCol     = IM_COL32(0x18, 0x18, 0x20, static_cast<int>(210 * alpha));
          borderCol = IM_COL32(0x50, 0x50, 0x70, static_cast<int>(200 * alpha));
          textCol   = IM_COL32(0xD0, 0xD0, 0xD0, static_cast<int>(255 * alpha));
          break;
      }

      ImVec2 textSize = ImGui::CalcTextSize(t.message.c_str(), nullptr, false, maxWidth - 16.0f);
      float boxW = textSize.x + 16.0f;
      float boxH = textSize.y + 12.0f;

      float x = vpPos.x + vpSize.x - boxW - xMargin;
      float y = vpPos.y + vpSize.y - yOffset - boxH;

      ImDrawList* dl = ImGui::GetForegroundDrawList();
      ImVec2 p0(x, y), p1(x + boxW, y + boxH);
      dl->AddRectFilled(p0, p1, bgCol, 4.0f);
      dl->AddRect(p0, p1, borderCol, 4.0f);
      dl->AddText(nullptr, 0.0f, ImVec2(x + 8.0f, y + 6.0f), textCol,
                  t.message.c_str(), nullptr, maxWidth - 16.0f);

      yOffset += boxH + 4.0f;
    }
  }

  // --- Quit confirmation popup (rendered here so it works regardless of show_menu) ---
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
      if (!imgui_state.show_menu && !imgui_state.show_options) {
        CPC.paused = false;
      }
    }
    ImGui::EndPopup();
  }

  // Reset devtools bar height when hidden so dockspace reclaims the space
  if (!imgui_state.show_devtools && s_devtools_bar_h != 0) {
    s_devtools_bar_h = 0;
    s_topbar_height_dirty = true;
  }

  // Apply deferred topbar/bottombar resize AFTER all ImGui rendering is complete.
  // Calling SDL_SetWindowSize during the render loop causes macOS to shift
  // window coordinates mid-frame, breaking button click detection.
  if (s_topbar_height_dirty) {
    int total = static_cast<int>(s_menubar_h) + s_main_topbar_h + s_devtools_bar_h;
    if (total != video_get_topbar_height()) {
      video_set_topbar(nullptr, total);
    }
    s_topbar_height_dirty = false;
  }
  if (s_bottombar_height_dirty) {
    if (s_statusbar_h != video_get_bottombar_height()) {
      video_set_bottombar(s_statusbar_h);
    }
    s_bottombar_height_dirty = false;
  }

  // Keyboard capture policy: let physical keys reach the CPC when no
  // keyboard-consuming UI is active.
  // - Classic mode: keys go to CPC unless any UI window is open.
  //   Uses imgui_any_keyboard_ui_active() (same as event filter).
  // - Docked mode: devtools are always visible as docked tabs, so we
  //   only block on modal UI / text input. CPC screen gets keyboard
  //   when focused, even with devtools docked alongside.
  {
    if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Docked) {
      bool modal_ui = ImGui::GetIO().WantTextInput
          || imgui_state.show_menu || imgui_state.show_options
          || imgui_state.show_about || imgui_state.show_quit_confirm
          || g_command_palette.is_open()
          || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
      if (!modal_ui && imgui_state.cpc_screen_focused) {
        ImGui::GetIO().WantCaptureKeyboard = false;
      }
    } else {
      if (!imgui_any_keyboard_ui_active()) {
        ImGui::GetIO().WantCaptureKeyboard = false;
      }
    }
  }
}

// ─────────────────────────────────────────────────
// MRU (recent files) helper
// ─────────────────────────────────────────────────

static void mru_push(std::vector<std::string>& list, const std::string& path) {
  mru_list_push(list, path, t_CPC::MRU_MAX);
}

void imgui_mru_push(std::vector<std::string>& list, const std::string& path) {
  mru_push(list, path);
}

// ─────────────────────────────────────────────────
// Toast notification API
// ─────────────────────────────────────────────────

void imgui_toast(const std::string& message, ImGuiUIState::ToastLevel level)
{
  // Cap queue size
  while (static_cast<int>(imgui_state.toasts.size()) >= ImGuiUIState::MAX_TOASTS) {
    imgui_state.toasts.pop_front();
  }
  float duration = (level == ImGuiUIState::ToastLevel::Error)
    ? ImGuiUIState::TOAST_DURATION * 1.5f   // errors stay longer
    : ImGuiUIState::TOAST_DURATION;
  imgui_state.toasts.push_back({message, level, duration, duration});
}

void imgui_toast_info(const std::string& message)    { imgui_toast(message, ImGuiUIState::ToastLevel::Info); }
void imgui_toast_success(const std::string& message) { imgui_toast(message, ImGuiUIState::ToastLevel::Success); }
void imgui_toast_error(const std::string& message)   { imgui_toast(message, ImGuiUIState::ToastLevel::Error); }

// ─────────────────────────────────────────────────
// Keyboard routing: single source of truth
// ─────────────────────────────────────────────────

bool imgui_any_keyboard_ui_active()
{
  ImGuiIO& io = ImGui::GetIO();
  return io.WantTextInput
      || imgui_state.show_menu || imgui_state.show_options
      || imgui_state.show_about || imgui_state.show_quit_confirm
      || imgui_state.show_memory_tool || imgui_state.show_layout_dropdown
      || imgui_state.show_devtools || g_devtools_ui.any_window_open()
      || g_command_palette.is_open()
      || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
}

// ─────────────────────────────────────────────────
// Helper: close menu and resume emulation
// ─────────────────────────────────────────────────

void imgui_close_menu()
{
  imgui_state.show_menu = false;
  // Don't clear show_options/show_about/show_quit_confirm here —
  // they may have just been set by the menu action that triggered imgui_close_menu().
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

void tape_scan_blocks()
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
// Main Menu Bar
// ─────────────────────────────────────────────────

static void imgui_render_menubar()
{
  if (!ImGui::BeginMainMenuBar()) return;

  float h = ImGui::GetWindowSize().y;
  if (h != s_menubar_h) { s_menubar_h = h; s_topbar_height_dirty = true; }

  // ── Emulator ──
  if (ImGui::BeginMenu("Emulator")) {
    if (ImGui::MenuItem("Options...")) {
      imgui_state.show_options = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Fullscreen", "F2")) {
      koncpc_menu_action(KONCPC_FULLSCRN);
    }
    if (ImGui::MenuItem("Screenshot", "F3")) {
      koncpc_menu_action(KONCPC_SCRNSHOT);
    }
    if (ImGui::MenuItem("Paste", "F11")) {
      koncpc_menu_action(KONCPC_PASTE);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset", "F5")) {
      emulator_reset();
    }
    if (ImGui::MenuItem("About...")) {
      imgui_state.show_about = true;
    }
    if (ImGui::MenuItem("Quit", "F10")) {
      imgui_state.show_quit_confirm = true;
      CPC.paused = true;
    }
    ImGui::EndMenu();
  }

  // ── Media ──
  if (ImGui::BeginMenu("Media")) {
    if (ImGui::MenuItem("Load Disk A...")) {
      static const SDL_DialogFileFilter filters[] = { { "Disk Images", "dsk;ipf;raw;zip" } };
      SDL_ShowOpenFileDialog(file_dialog_callback,
        reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadDiskA)),
        mainSDLWindow, filters, 1, CPC.current_dsk_path.c_str(), false);
    }
    if (ImGui::MenuItem("Load Disk B...")) {
      static const SDL_DialogFileFilter filters[] = { { "Disk Images", "dsk;ipf;raw;zip" } };
      SDL_ShowOpenFileDialog(file_dialog_callback,
        reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadDiskB)),
        mainSDLWindow, filters, 1, CPC.current_dsk_path.c_str(), false);
    }
    if (ImGui::MenuItem("Save Disk A...", nullptr, false, driveA.tracks != 0)) {
      static const SDL_DialogFileFilter filters[] = { { "Disk Images", "dsk" } };
      SDL_ShowSaveFileDialog(file_dialog_callback,
        reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::SaveDiskA)),
        mainSDLWindow, filters, 1, CPC.current_dsk_path.c_str());
    }
    if (ImGui::MenuItem("Save Disk B...", nullptr, false, driveB.tracks != 0)) {
      static const SDL_DialogFileFilter filters[] = { { "Disk Images", "dsk" } };
      SDL_ShowSaveFileDialog(file_dialog_callback,
        reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::SaveDiskB)),
        mainSDLWindow, filters, 1, CPC.current_dsk_path.c_str());
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Load Snapshot...")) {
      static const SDL_DialogFileFilter filters[] = { { "Snapshots", "sna;zip" } };
      SDL_ShowOpenFileDialog(file_dialog_callback,
        reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadSnapshot)),
        mainSDLWindow, filters, 1, CPC.current_snap_path.c_str(), false);
    }
    if (ImGui::MenuItem("Save Snapshot...")) {
      static const SDL_DialogFileFilter filters[] = { { "Snapshots", "sna" } };
      SDL_ShowSaveFileDialog(file_dialog_callback,
        reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::SaveSnapshot)),
        mainSDLWindow, filters, 1, CPC.current_snap_path.c_str());
    }
    if (ImGui::MenuItem("Quick Save Snapshot", "Shift+F3")) {
      koncpc_menu_action(KONCPC_SNAPSHOT);
    }
    if (ImGui::MenuItem("Quick Load Snapshot", "Shift+F4")) {
      koncpc_menu_action(KONCPC_LD_SNAP);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Load Tape...")) {
      static const SDL_DialogFileFilter filters[] = { { "Tape Images", "cdt;voc;zip" } };
      SDL_ShowOpenFileDialog(file_dialog_callback,
        reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadTape)),
        mainSDLWindow, filters, 1, CPC.current_tape_path.c_str(), false);
    }
    if (ImGui::MenuItem("Tape Play/Stop", "F4", false, !pbTapeImage.empty())) {
      koncpc_menu_action(KONCPC_TAPEPLAY);
    }
    if (ImGui::MenuItem("Eject Tape", nullptr, false, !pbTapeImage.empty())) {
      tape_eject();
      CPC.tape.file.clear();
      imgui_state.tape_block_offsets.clear();
      imgui_state.tape_current_block = 0;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Load Cartridge...")) {
      static const SDL_DialogFileFilter filters[] = { { "Cartridges", "cpr;zip" } };
      SDL_ShowOpenFileDialog(file_dialog_callback,
        reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::LoadCartridge)),
        mainSDLWindow, filters, 1, CPC.current_cart_path.c_str(), false);
    }

    // ── Open Recent submenu ──
    bool has_any_mru = !CPC.mru_disks.empty() || !CPC.mru_tapes.empty() ||
                       !CPC.mru_snaps.empty() || !CPC.mru_carts.empty();
    ImGui::Separator();
    if (ImGui::BeginMenu("Open Recent", has_any_mru)) {
      auto render_mru_section = [&](const char* label, std::vector<std::string>& list,
                                    auto load_fn) {
        if (!list.empty() && ImGui::BeginMenu(label)) {
          for (int i = 0; i < static_cast<int>(list.size()); i++) {
            auto item_fname = std::filesystem::path(list[i]).filename().string();
            ImGui::PushID(i);
            if (ImGui::MenuItem(item_fname.c_str())) {
              load_fn(list[i]);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", list[i].c_str());
            ImGui::PopID();
          }
          ImGui::EndMenu();
        }
      };
      render_mru_section("Disks", CPC.mru_disks, [](const std::string& p) {
        CPC.driveA.file = p;
        auto f = std::filesystem::path(p).filename().string();
        if (file_load(CPC.driveA) == 0) { imgui_toast_success("Drive A: " + f); mru_push(CPC.mru_disks, p); }
        else imgui_toast_error("Failed: " + f);
      });
      render_mru_section("Tapes", CPC.mru_tapes, [](const std::string& p) {
        CPC.tape.file = p;
        auto f = std::filesystem::path(p).filename().string();
        if (file_load(CPC.tape) == 0) { imgui_toast_success("Tape: " + f); tape_scan_blocks(); mru_push(CPC.mru_tapes, p); }
        else imgui_toast_error("Failed: " + f);
      });
      render_mru_section("Snapshots", CPC.mru_snaps, [](const std::string& p) {
        CPC.snapshot.file = p;
        auto f = std::filesystem::path(p).filename().string();
        if (file_load(CPC.snapshot) == 0) { imgui_toast_success("Snapshot: " + f); mru_push(CPC.mru_snaps, p); }
        else imgui_toast_error("Failed: " + f);
      });
      render_mru_section("Cartridges", CPC.mru_carts, [](const std::string& p) {
        CPC.cartridge.file = p;
        auto f = std::filesystem::path(p).filename().string();
        if (file_load(CPC.cartridge) == 0) { imgui_toast_success("Cartridge: " + f); emulator_reset(); mru_push(CPC.mru_carts, p); }
        else imgui_toast_error("Failed: " + f);
      });
      ImGui::Separator();
      if (ImGui::MenuItem("Clear Recent")) {
        CPC.mru_disks.clear();
        CPC.mru_tapes.clear();
        CPC.mru_snaps.clear();
        CPC.mru_carts.clear();
      }
      ImGui::EndMenu();
    }

    ImGui::EndMenu();
  }

  // ── Tools ──
  if (ImGui::BeginMenu("Tools")) {
    if (ImGui::MenuItem("Memory Tool")) {
      // Open the DevTools Memory Hex window (superset of legacy Memory Tool)
      imgui_state.show_devtools = true;
      g_devtools_ui.toggle_window("memory_hex");
    }
    if (ImGui::MenuItem("DevTools", "Shift+F2")) {
      imgui_state.show_devtools = !imgui_state.show_devtools;
    }
    if (ImGui::MenuItem("Virtual Keyboard", "Shift+F1")) {
      koncpc_menu_action(KONCPC_VKBD);
    }
    if (ImGui::MenuItem("MF2 Stop", "F6")) {
      koncpc_menu_action(KONCPC_MF2STOP);
    }
    ImGui::EndMenu();
  }

  // ── Options ──
  if (ImGui::BeginMenu("Options")) {
    if (ImGui::MenuItem("Joystick Emulation", "F7", CPC.joystick_emulation != JoystickEmulation::None)) {
      koncpc_menu_action(KONCPC_JOY);
    }
    if (ImGui::MenuItem("Phazer Emulation", "Shift+F7", static_cast<bool>(CPC.phazer_emulation))) {
      koncpc_menu_action(KONCPC_PHAZER);
    }
    if (ImGui::MenuItem("Speed Limit", "F9", CPC.limit_speed != 0)) {
      koncpc_menu_action(KONCPC_SPEED);
    }
    if (ImGui::MenuItem("Show FPS", "F8", CPC.scr_fps != 0)) {
      koncpc_menu_action(KONCPC_FPS);
    }
    if (ImGui::MenuItem("Verbose Logging", "F12", log_verbose)) {
      koncpc_menu_action(KONCPC_DEBUG);
    }
    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
}

// ─────────────────────────────────────────────────
// Top Bar
// ─────────────────────────────────────────────────

int imgui_topbar_height()
{
  return static_cast<int>(s_menubar_h) + s_main_topbar_h + s_devtools_bar_h;
}

static void imgui_render_topbar()
{
  float pad_y = 2.0f;
  float bar_height = 25.0f; // topbar window only (not including menu bar)

  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + s_menubar_h));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, bar_height));
  ImGui::SetNextWindowViewport(vp->ID);  // keep on main viewport
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
      int h = static_cast<int>(ImGui::GetWindowSize().y);
      if (h != s_main_topbar_h) { s_main_topbar_h = h; s_topbar_height_dirty = true; }
    }
    bool menu_pressed = ImGui::Button("Pause (F1)");
    if (menu_pressed) {
      imgui_state.show_menu = true;
      imgui_state.menu_just_opened = true;
      CPC.paused = true;
    }
    // (Tape waveform moved to bottom status bar)

    // ── Layout dropdown ──
    // Uses a state-flag + standalone window instead of ImGui popup,
    // because popups from the fixed topbar close immediately in docked
    // mode due to focus interactions with the dockspace.
    {
      // Right-align before FPS counter
      float fps_w = 0.0f;
      if (!imgui_state.topbar_fps.empty())
        fps_w = ImGui::CalcTextSize(imgui_state.topbar_fps.c_str()).x + 16.0f;
      float btn_w = ImGui::CalcTextSize("Layout").x + ImGui::GetStyle().FramePadding.x * 2.0f;
      ImGui::SameLine(ImGui::GetWindowWidth() - fps_w - btn_w - 12.0f);

      if (ImGui::Button("Layout")) {
        imgui_state.show_layout_dropdown = !imgui_state.show_layout_dropdown;
      }
      // Remember button position for dropdown window placement
      s_layout_btn_pos = ImGui::GetItemRectMin();
      s_layout_btn_pos.y = ImGui::GetItemRectMax().y + 2.0f;
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

  // ── Layout dropdown window (rendered outside topbar) ──
  if (imgui_state.show_layout_dropdown) {
    // Position below the Layout button, clamped to stay within the main viewport
    {
      ImGuiViewport* mvp = ImGui::GetMainViewport();
      float dd_w = 220.0f;
      float x = s_layout_btn_pos.x;
      float right_edge = mvp->Pos.x + mvp->Size.x;
      if (x + dd_w > right_edge) x = right_edge - dd_w;
      ImGui::SetNextWindowPos(ImVec2(x, s_layout_btn_pos.y), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(dd_w, 0));
      ImGui::SetNextWindowViewport(mvp->ID);
    }

    ImGuiWindowFlags dd_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    if (ImGui::Begin("##LayoutDropdown", nullptr, dd_flags)) {
      // Close when clicking outside
      if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
          ImGui::IsMouseClicked(0)) {
        imgui_state.show_layout_dropdown = false;
      }

      // Mode selection
      if (ImGui::RadioButton("Classic Mode", CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic)) {
        CPC.workspace_layout = t_CPC::WorkspaceLayoutMode::Classic;
        imgui_state.show_layout_dropdown = false;
      }
      if (ImGui::RadioButton("Docked Mode", CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Docked)) {
        if (CPC.workspace_layout != t_CPC::WorkspaceLayoutMode::Docked) {
          CPC.workspace_layout = t_CPC::WorkspaceLayoutMode::Docked;
          workspace_request_initial_preset();
        }
        imgui_state.show_layout_dropdown = false;
      }
      ImGui::Separator();

      // Preset layouts
      if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Docked) {
        if (ImGui::MenuItem("Apply Debug Layout")) {
          workspace_apply_preset(WorkspacePreset::Debug);
          imgui_state.show_layout_dropdown = false;
        }
        if (ImGui::MenuItem("Apply IDE Layout")) {
          workspace_apply_preset(WorkspacePreset::IDE);
          imgui_state.show_layout_dropdown = false;
        }
        if (ImGui::MenuItem("Apply Hardware Layout")) {
          workspace_apply_preset(WorkspacePreset::Hardware);
          imgui_state.show_layout_dropdown = false;
        }
      } else {
        if (ImGui::MenuItem("Debug")) {
          g_devtools_ui.toggle_window("registers");
          g_devtools_ui.toggle_window("disassembly");
          g_devtools_ui.toggle_window("stack");
          g_devtools_ui.toggle_window("breakpoints");
          imgui_state.show_layout_dropdown = false;
        }
        if (ImGui::MenuItem("Memory")) {
          g_devtools_ui.toggle_window("memory_hex");
          g_devtools_ui.toggle_window("symbols");
          g_devtools_ui.toggle_window("data_areas");
          imgui_state.show_layout_dropdown = false;
        }
        if (ImGui::MenuItem("Hardware")) {
          g_devtools_ui.toggle_window("video_state");
          g_devtools_ui.toggle_window("audio_state");
          g_devtools_ui.toggle_window("asic");
          g_devtools_ui.toggle_window("silicon_disc");
          imgui_state.show_layout_dropdown = false;
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
              if (ImGui::MenuItem(l.c_str())) {
                workspace_load_layout(l);
                imgui_state.show_layout_dropdown = false;
              }
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

      // Save Layout popup (modal, so it won't have focus issues)
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
              imgui_state.show_layout_dropdown = false;
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
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }
}

// ─────────────────────────────────────────────────
// Marquee text helper: scrolls text horizontally within a fixed-width box
// when text is wider than boxW. Uses ping-pong with pause at start/end.
// ─────────────────────────────────────────────────
static void imgui_marquee_text(const char* text, float boxW)
{
  float textW = ImGui::CalcTextSize(text).x;
  if (textW <= boxW) {
    ImGui::TextUnformatted(text);
    return;
  }
  // Ping-pong scroll with 20px pause at each end
  float overflow = textW - boxW;
  float range = overflow + 40.0f; // 20px pause at start + 20px pause at end
  float t = fmodf(static_cast<float>(ImGui::GetTime()) * 30.0f, range * 2.0f);
  float scroll = t < range ? t : range * 2.0f - t;
  scroll = fmaxf(0.0f, scroll - 20.0f); // pause at start

  ImVec2 pos = ImGui::GetCursorScreenPos();
  float lineH = ImGui::GetTextLineHeight();
  ImGui::PushClipRect(pos, ImVec2(pos.x + boxW, pos.y + lineH), true);
  ImGui::SetCursorScreenPos(ImVec2(pos.x - scroll, pos.y));
  ImGui::TextUnformatted(text);
  ImGui::PopClipRect();
  // Advance cursor past the box
  ImGui::SetCursorScreenPos(ImVec2(pos.x + boxW, pos.y));
  ImGui::Dummy(ImVec2(0, lineH));
}

// ─────────────────────────────────────────────────
// Bottom Status Bar
// ─────────────────────────────────────────────────

static void imgui_render_statusbar()
{
  float bar_height = 22.0f;
  float pad_y = 2.0f;

  ImGuiViewport* vp = ImGui::GetMainViewport();
  float bar_y = vp->Pos.y + vp->Size.y - bar_height;

  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, bar_y));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, bar_height));
  ImGui::SetNextWindowViewport(vp->ID);  // keep on main viewport, don't spawn platform window
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, pad_y));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoDocking |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

  if (ImGui::Begin("##statusbar", nullptr, flags)) {
    // Track statusbar height for bottombar layout
    int h = static_cast<int>(ImGui::GetWindowSize().y);
    if (h != s_statusbar_h) { s_statusbar_h = h; s_bottombar_height_dirty = true; }

    // ── Drive activity LEDs ──
    {
      float frameH = ImGui::GetFrameHeight();
      for (int drv = 0; drv < 2; drv++) {
        bool active = drv == 0 ? imgui_state.drive_a_led : imgui_state.drive_b_led;
        t_drive& drive = drv == 0 ? driveA : driveB;
        auto& driveFile = drv == 0 ? CPC.driveA.file : CPC.driveB.file;
        const char* driveLabel = drv == 0 ? "A:" : "B:";

        if (drv > 0) ImGui::SameLine(0, 12.0f);

        // Build display name
        const char* fullName;
        if (drive.tracks) {
          auto pos = driveFile.find_last_of("/\\");
          fullName = (pos != std::string::npos) ? driveFile.c_str() + pos + 1 : driveFile.c_str();
        } else {
          fullName = "(no disk)";
        }

        // Push unique ID per drive to avoid conflicts
        ImGui::PushID(100 + drv); // offset IDs to avoid clashes with topbar

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

        // Show filename or "(no disk)" with marquee scrolling
        ImGui::PushStyleColor(ImGuiCol_Text, drive.tracks
          ? ImVec4(0.75f, 0.75f, 0.75f, 1.0f)
          : ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
        ImGui::AlignTextToFramePadding();
        imgui_marquee_text(fullName, 120.0f);
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

    // ── M4 Board activity LED (green, only shown when M4 is enabled) ──
    if (g_m4board.enabled) {
      float frameH = ImGui::GetFrameHeight();
      bool active = g_m4board.activity_frames > 0;

      ImGui::SameLine(0, 12.0f);
      ImGui::BeginGroup();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("M4:");
      ImGui::SameLine(0, 2.0f);

      ImVec2 cursor = ImGui::GetCursorScreenPos();
      float ledW = 16.0f, ledH = 8.0f;
      float yOff = (frameH - ledH) * 0.5f;
      ImVec2 p0(cursor.x, cursor.y + yOff);
      ImVec2 p1(p0.x + ledW, p0.y + ledH);

      ImDrawList* dl = ImGui::GetWindowDrawList();
      if (active) {
        // Active: bright green
        dl->AddRectFilled(p0, p1, IM_COL32(0, 255, 0, 255));
        dl->AddLine(p0, ImVec2(p1.x, p0.y), IM_COL32(100, 255, 100, 255));
        dl->AddLine(p0, ImVec2(p0.x, p1.y), IM_COL32(100, 255, 100, 255));
        dl->AddLine(ImVec2(p0.x, p1.y), p1, IM_COL32(0, 160, 0, 255));
        dl->AddLine(ImVec2(p1.x, p0.y), p1, IM_COL32(0, 160, 0, 255));
      } else {
        // Inactive: dark green
        dl->AddRectFilled(p0, p1, IM_COL32(0, 80, 0, 255));
        dl->AddLine(p0, ImVec2(p1.x, p0.y), IM_COL32(20, 110, 20, 255));
        dl->AddLine(p0, ImVec2(p0.x, p1.y), IM_COL32(20, 110, 20, 255));
        dl->AddLine(ImVec2(p0.x, p1.y), p1, IM_COL32(0, 40, 0, 255));
        dl->AddLine(ImVec2(p1.x, p0.y), p1, IM_COL32(0, 40, 0, 255));
      }

      ImGui::Dummy(ImVec2(ledW, frameH));

      // Show container name if inside a DSK (with marquee scrolling)
      if (g_m4board.container_type != M4Board::ContainerType::NONE) {
        ImGui::SameLine(0, 4.0f);
        auto fname = std::filesystem::path(g_m4board.container_host_path).filename().string();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.9f, 0.45f, 1.0f));
        ImGui::AlignTextToFramePadding();
        imgui_marquee_text(fname.c_str(), 120.0f);
        ImGui::PopStyleColor();
      }

      ImGui::EndGroup();
    }

    // ── Separator ──
    ImGui::SameLine(0, 12.0f);
    {
      ImVec2 cursor = ImGui::GetCursorScreenPos();
      float frameH = ImGui::GetFrameHeight();
      ImGui::GetWindowDrawList()->AddLine(
        ImVec2(cursor.x, cursor.y + 2.0f),
        ImVec2(cursor.x, cursor.y + frameH - 2.0f),
        IM_COL32(0x50, 0x50, 0x50, 0xFF), 1.0f);
      ImGui::Dummy(ImVec2(1.0f, frameH));
    }

    // ── TAPE section ──
    {
      bool tape_loaded = !pbTapeImage.empty();
      bool tape_playing = tape_loaded && CPC.tape_motor && CPC.tape_play_button;

      ImGui::SameLine(0, 8.0f);
      ImGui::AlignTextToFramePadding();

      ImU32 color_active = IM_COL32(0x00, 0xFF, 0x80, 0xFF);
      ImU32 label_color  = tape_playing ? color_active : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);

      // ── TAPE label ──
      ImGui::PushStyleColor(ImGuiCol_Text, label_color);
      ImGui::TextUnformatted("TAPE");
      ImGui::PopStyleColor();

      // ── Filename (clickable when no tape → load) ──
      ImGui::SameLine(0, 4);
      {
        const char* fullTapeName;
        if (tape_loaded && !CPC.tape.file.empty()) {
          auto pos = CPC.tape.file.find_last_of("/\\");
          fullTapeName = (pos != std::string::npos) ? CPC.tape.file.c_str() + pos + 1 : CPC.tape.file.c_str();
        } else {
          fullTapeName = "(no tape)";
        }
        ImGui::PushStyleColor(ImGuiCol_Text, tape_loaded
          ? ImVec4(0.75f, 0.75f, 0.75f, 1.0f)
          : ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
        ImGui::AlignTextToFramePadding();
        imgui_marquee_text(fullTapeName, 120.0f);
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
        if (ImGui::SmallButton("\xe2\x97\x80##sb_prev")) { // ◀
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
        if (ImGui::SmallButton("\xe2\x96\xb6##sb_play")) { // ▶
          CPC.tape_play_button = 0x10;
        }
        ImGui::EndDisabled();
        if (is_playing) ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 2);

        // ⏹ Stop
        ImGui::BeginDisabled(!is_playing);
        if (ImGui::SmallButton("\xe2\x96\xa0##sb_stop")) { // ■
          CPC.tape_play_button = 0;
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0, 2);

        // ▷| Next block
        ImGui::BeginDisabled(at_end || imgui_state.tape_current_block >= (int)imgui_state.tape_block_offsets.size() - 1);
        if (ImGui::SmallButton("\xe2\x96\xb6##sb_next")) { // ▶
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
        if (ImGui::SmallButton("\xe2\x8f\x8f##sb_eject")) { // ⏏
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

      // ── Tape waveform oscilloscope ──
      {
        // Reset state when tape is ejected
        if (!tape_loaded) {
          imgui_state.tape_decoded_head = 0;
          memset(imgui_state.tape_decoded_buf, 0, sizeof(imgui_state.tape_decoded_buf));
        }

        // Update current block index from pbTapeBlock pointer.
        // Also re-scan when the block offsets table changes (new tape loaded).
        static byte* last_pbTapeBlock = nullptr;
        static size_t last_block_count = 0;
        static const byte* last_tape_base = nullptr;
        static byte* last_block0 = nullptr;
        // Detect new tape: base pointer, block count, or first block address changed.
        const byte* tape_base = pbTapeImage.empty() ? nullptr : &pbTapeImage[0];
        byte* block0 = imgui_state.tape_block_offsets.empty() ? nullptr : imgui_state.tape_block_offsets[0];
        if (tape_base != last_tape_base ||
            imgui_state.tape_block_offsets.size() != last_block_count ||
            block0 != last_block0) {
          last_tape_base = tape_base;
          last_block_count = imgui_state.tape_block_offsets.size();
          last_block0 = block0;
          last_pbTapeBlock = nullptr;  // force re-scan
        }
        if (tape_loaded && !imgui_state.tape_block_offsets.empty() && pbTapeBlock != last_pbTapeBlock) {
          last_pbTapeBlock = pbTapeBlock;
          for (int i = 0; i < (int)imgui_state.tape_block_offsets.size(); i++) {
            if (imgui_state.tape_block_offsets[i] == pbTapeBlock) {
              imgui_state.tape_current_block = i;
              break;
            }
            if (imgui_state.tape_block_offsets[i] > pbTapeBlock) {
              imgui_state.tape_current_block = i > 0 ? i - 1 : 0;
              break;
            }
          }
        }

        ImGui::SameLine(0, 4);
        float frameH = ImGui::GetFrameHeight();
        ImU32 color_active = IM_COL32(0x00, 0xFF, 0x80, 0xFF);
        ImU32 color_dim    = IM_COL32(0x00, 0x40, 0x20, 0xFF);

        float waveW = 100.0f;
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        float yOff = (frameH - frameH * 0.8f) * 0.5f;
        ImVec2 p0(cursor.x, cursor.y + yOff);
        float boxH = frameH * 0.8f;
        ImVec2 p1(p0.x + waveW, p0.y + boxH);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, IM_COL32(0x10, 0x10, 0x10, 0xFF));
        dl->AddRect(p0, p1, tape_playing ? IM_COL32(0x00, 0x80, 0x40, 0x80) : IM_COL32(0x00, 0x30, 0x18, 0x60));

        ImU32 wave_color = tape_playing ? color_active : color_dim;
        constexpr int N = ImGuiUIState::TAPE_WAVE_SAMPLES;
        float stepX = waveW / static_cast<float>(N - 1);
        int mode = imgui_state.tape_wave_mode;

        float yBot = p1.y - 2.0f;
        float yTop = p0.y + 2.0f;
        auto yForSample = [&](byte val) -> float { return val ? yTop : yBot; };
        int oldest = imgui_state.tape_wave_head;

        if (mode == 0) {
          ImVec2 points[N * 2 + 2];
          int nPoints = 0;
          float prevY = yForSample(imgui_state.tape_wave_buf[oldest]);
          points[nPoints++] = ImVec2(p0.x, prevY);
          for (int i = 1; i < N; i++) {
            int idx = (oldest + i) % N;
            float curX = p0.x + i * stepX;
            float curY = yForSample(imgui_state.tape_wave_buf[idx]);
            if (curY != prevY) {
              points[nPoints++] = ImVec2(curX, prevY);
              points[nPoints++] = ImVec2(curX, curY);
              prevY = curY;
            }
          }
          points[nPoints++] = ImVec2(p1.x, prevY);
          dl->AddPolyline(points, nPoints, wave_color, 0, 1.0f);
        } else {
          int dN = ImGuiUIState::TAPE_DECODED_SAMPLES;
          int dHead = imgui_state.tape_decoded_head;
          int visCount = static_cast<int>(waveW);
          if (visCount > dN) visCount = dN;
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

        {
          const char* modeLabel = (mode == 0) ? "RAW" : "BITS";
          ImVec2 labelSize = ImGui::CalcTextSize(modeLabel);
          ImVec2 labelPos(p1.x - labelSize.x - 2.0f, p0.y + 1.0f);
          dl->AddText(labelPos, IM_COL32(0x80, 0x80, 0x80, 0xA0), modeLabel);
        }

        ImGui::Dummy(ImVec2(waveW, frameH));
        if (ImGui::IsItemClicked()) {
          imgui_state.tape_wave_mode = (imgui_state.tape_wave_mode + 1) % 2;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Click to cycle waveform mode (RAW pulse / decoded BITS)");
        }
      }
    }

    // ── Eject Disk confirmation popup ──
    if (imgui_state.eject_confirm_drive >= 0) {
      ImGui::OpenPopup("Eject Disk?##sb");
    }
    if (ImGui::BeginPopupModal("Eject Disk?##sb", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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

    // ── Eject Tape confirmation popup ──
    if (imgui_state.eject_confirm_tape) {
      ImGui::OpenPopup("Eject Tape?##sb");
    }
    if (ImGui::BeginPopupModal("Eject Tape?##sb", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
  ImGuiViewport* mvp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(mvp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  ImGui::SetNextWindowSize(ImVec2(260, 0));
  ImGui::SetNextWindowViewport(mvp->ID);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoDocking |
                           ImGuiWindowFlags_AlwaysAutoResize;


  bool menu_open = true;
  if (!ImGui::Begin("konCePCja", &menu_open, flags)) {
    if (!menu_open) imgui_close_menu();
    ImGui::End();
    return;
  }
  if (!menu_open) { imgui_close_menu(); ImGui::End(); return; }

  // Keyboard shortcuts within pause menu
  bool action = false;
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { imgui_close_menu(); ImGui::End(); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) { emulator_reset(); action = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_Q)) { imgui_state.show_quit_confirm = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_A)) { imgui_state.show_about = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) { emulator_reset(); action = true; }
  }

  float bw = ImGui::GetContentRegionAvail().x;

  ImGui::TextWrapped("Emulation paused. Use the menu bar above for all actions.");
  ImGui::Spacing();

  // Enable keyboard navigation for this window (arrows/tab cycle buttons)
  if (imgui_state.menu_just_opened) {
    ImGui::SetKeyboardFocusHere();
    imgui_state.menu_just_opened = false;
  }
  if (ImGui::Button("Resume (Esc)", ImVec2(bw, 0))) {
    action = true;
  }
  if (ImGui::Button("Reset (F5/R)", ImVec2(bw, 0))) {
    emulator_reset();
    action = true;
  }
  if (ImGui::Button("About (A)", ImVec2(bw, 0))) {
    imgui_state.show_about = true;
  }
  if (ImGui::Button("Quit (Q)", ImVec2(bw, 0))) {
    imgui_state.show_quit_confirm = true;
  }

  ImGui::End();

  if (action) imgui_close_menu();

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
    ImGui::BulletText("F3 - Screenshot");
#ifdef __APPLE__
    ImGui::BulletText("Cmd+K - Command Palette");
#else
    ImGui::BulletText("Ctrl+K - Command Palette");
#endif
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
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

  ImGuiViewport* mvp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(mvp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(480, 420), ImGuiCond_Appearing);
  ImGui::SetNextWindowViewport(mvp->ID);

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

      bool frameskip = CPC.frameskip != 0;
      if (ImGui::Checkbox("Auto Frameskip", &frameskip)) {
        CPC.frameskip = frameskip ? 1 : 0;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Skip rendering frames when emulation falls behind 100%% speed.\nOnly has an effect when 'Limit Speed' is enabled.");
      }

      int speed = static_cast<int>(CPC.speed);
      if (ImGui::SliderInt("Speed", &speed, MIN_SPEED_SETTING, MAX_SPEED_SETTING)) {
        CPC.speed = speed;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("CPU clock speed in MHz (default: 4).\nHigher = faster emulation.");
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
        ImGui::SetTooltip("M4 Board — Virtual WiFi/SD expansion.\nSee the M4 Board tab for full settings.");
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
          ImGui::TableSetColumnIndex(4);
          if (i < 2) {
            ImGui::TextDisabled("system");
          } else if (loaded) {
            if (ImGui::SmallButton("X")) {
              delete[] memmap_ROM[i];
              memmap_ROM[i] = nullptr;
              CPC.rom_file[i] = "";
              // If this was the active upper ROM, revert to BASIC ROM
              if (GateArray.upper_ROM == static_cast<unsigned char>(i)) {
                pbExpansionROM = pbROMhi;
                if (!(GateArray.ROM_config & 0x08)) {
                  memory_set_read_bank(3, pbExpansionROM);
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
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Window size multiplier (1x = 384x270)");
      }

      bool colour = CPC.scr_tube == 0;
      if (ImGui::RadioButton("Colour", colour)) { CPC.scr_tube = 0; }
      ImGui::SameLine();
      if (ImGui::RadioButton("Mono (Green)", !colour)) { CPC.scr_tube = 1; }

      int intensity = static_cast<int>(CPC.scr_intensity);
      if (ImGui::SliderInt("Intensity", &intensity, 5, 15)) {
        CPC.scr_intensity = intensity;
        video_set_palette();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("CRT phosphor brightness.\nHigher = brighter colours.");
      }

      bool scanlines = CPC.scr_scanlines != 0;
      if (ImGui::Checkbox("Scanlines", &scanlines)) {
        CPC.scr_scanlines = scanlines ? 1 : 0;
        if (!scanlines) {
          CPC.scr_oglscanlines = 0;
          video_set_palette();
        }
      }
      if (scanlines) {
        int sl_intensity = static_cast<int>(CPC.scr_oglscanlines);
        if (ImGui::SliderInt("Scanline Intensity", &sl_intensity, 0, 100)) {
          CPC.scr_oglscanlines = sl_intensity;
          video_set_palette();
        }
      }

      bool fps = CPC.scr_fps != 0;
      if (ImGui::Checkbox("Show FPS", &fps)) { CPC.scr_fps = fps ? 1 : 0; }

      bool fullscreen = CPC.scr_window == 0;
      if (ImGui::Checkbox("Fullscreen", &fullscreen)) { CPC.scr_window = fullscreen ? 0 : 1; }

      bool aspect = CPC.scr_preserve_aspect_ratio != 0;
      if (ImGui::Checkbox("Preserve Aspect Ratio", &aspect)) {
        CPC.scr_preserve_aspect_ratio = aspect ? 1 : 0;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When off, the CPC screen stretches\nto fill the entire window.");
      }

      ImGui::EndTabItem();
    }

    // ── Audio Tab ──
    if (ImGui::BeginTabItem("Audio")) {
      bool snd = CPC.snd_enabled != 0;
      if (ImGui::Checkbox("Enable Sound", &snd)) { CPC.snd_enabled = snd ? 1 : 0; }

      static constexpr int kDefaultSampleRateIndex = 2;  // 44100 Hz
      int rate_idx = static_cast<int>(CPC.snd_playback_rate);
      if (rate_idx < 0 || rate_idx >= static_cast<int>(IM_ARRAYSIZE(sample_rates))) {
        rate_idx = kDefaultSampleRateIndex;
        CPC.snd_playback_rate = rate_idx;  // fix invalid value immediately
      }
      if (ImGui::Combo("Sample Rate", &rate_idx, sample_rates, IM_ARRAYSIZE(sample_rates))) {
        CPC.snd_playback_rate = rate_idx;  // store index (0-4), not raw frequency
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

      int ksm = static_cast<int>(CPC.keyboard_support_mode);
      const char* ksm_modes[] = { "Direct", "Buffered Until Read", "Min. 2 Frames" };
      int max_ksm = IM_ARRAYSIZE(ksm_modes);
      if (ksm < 0 || ksm >= max_ksm) ksm = 0;
      if (ImGui::Combo("Keyboard Support Mode", &ksm, ksm_modes, max_ksm)) {
        CPC.keyboard_support_mode = static_cast<KeyboardSupportMode>(ksm);
      }

      bool joy_emu = CPC.joystick_emulation != JoystickEmulation::None;
      if (ImGui::Checkbox("Joystick Emulation", &joy_emu)) {
        CPC.joystick_emulation = joy_emu ? JoystickEmulation::Keyboard : JoystickEmulation::None;
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

    // ── M4 Board Tab ──
    if (ImGui::BeginTabItem("M4 Board")) {
      bool m4_en = g_m4board.enabled;
      if (ImGui::Checkbox("Enable M4 Board", &m4_en)) { g_m4board.enabled = m4_en; }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "M4 Board — Virtual WiFi/SD expansion\n\n"
          "Set a host directory as the virtual SD card, then reset.\n\n"
          "RSX commands (type in BASIC after reset):\n"
          "  |SD            Switch to SD card\n"
          "  |DISC          Switch back to disc drive\n"
          "  |DIR           List SD directory\n"
          "  |CD,\"path\"     Change directory\n"
          "  |ERA,\"file\"    Delete file\n"
          "  |REN,\"new\",\"old\" Rename file\n"
          "  |MKDIR,\"dir\"   Create directory\n\n"
          "After |SD, standard BASIC commands work:\n"
          "  CAT  LOAD\"file\"  SAVE\"file\"  RUN\"file\""
        );
      }

      if (m4_en) {
        // ── SD Card ──
        ImGui::Separator();
        ImGui::TextDisabled("SD Card");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80.0f);
        char sd_buf[512];
        snprintf(sd_buf, sizeof(sd_buf), "%s", g_m4board.sd_root_path.c_str());
        ImGui::InputText("##m4sd", sd_buf, sizeof(sd_buf), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Browse##m4sd")) {
          const char* default_loc = g_m4board.sd_root_path.empty()
            ? nullptr : g_m4board.sd_root_path.c_str();
          SDL_ShowOpenFolderDialog(file_dialog_callback,
            reinterpret_cast<void*>(static_cast<intptr_t>(FileDialogAction::SelectM4SDFolder)),
            mainSDLWindow, default_loc, false);
        }
        if (g_m4board.sd_root_path.empty()) {
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No SD directory set");
        }

        int slot = g_m4board.rom_slot;
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("ROM Slot##m4", &slot, 1, 1)) {
          if (slot < 0) slot = 0;
          if (slot > 31) slot = 31;
          g_m4board.rom_slot = slot;
        }

        // ── Status ──
        ImGui::Separator();
        ImGui::TextDisabled("Status");

        bool active = g_m4board.activity_frames > 0;
        ImVec4 led_color = active ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                                  : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(led_color, "%s", active ? "SD Active" : "SD Idle");
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("Dir: %s", g_m4board.current_dir.c_str());

        int open_count = 0;
        for (int i = 0; i < 4; i++) {
          if (g_m4board.open_files[i]) open_count++;
        }
        ImGui::Text("Open files: %d/4", open_count);
        if (!g_m4board.last_filename.empty()) {
          ImGui::SameLine(0, 16);
          ImGui::TextDisabled("Last: %s", g_m4board.last_filename.c_str());
        }
        if (g_m4board.cmd_count > 0) {
          ImGui::Text("Commands: %d", g_m4board.cmd_count);
        }

        // Network status
        ImGui::Text("Network: %s", g_m4board.network_enabled ? "enabled" : "disabled");
        ImGui::SameLine(0, 16);
        int sock_count = 0;
        for (int i = 0; i < M4Board::MAX_SOCKETS; i++) {
          if (g_m4board.sockets[i] != M4Board::INVALID_SOCK) sock_count++;
        }
        ImGui::TextDisabled("Sockets: %d/%d", sock_count, M4Board::MAX_SOCKETS);

        // ── HTTP Server ──
        ImGui::Separator();
        ImGui::TextDisabled("HTTP Server");

        int http_port = CPC.m4_http_port;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputInt("HTTP Port##m4http", &http_port, 1, 100)) {
          if (http_port < 1024) http_port = 1024;
          if (http_port > 65535) http_port = 65535;
          CPC.m4_http_port = http_port;
        }

        char ip_buf[64];
        snprintf(ip_buf, sizeof(ip_buf), "%s", CPC.m4_bind_ip.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80.0f);
        if (ImGui::InputText("Bind IP##m4ip", ip_buf, sizeof(ip_buf))) {
          CPC.m4_bind_ip = ip_buf;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
            "IP address to bind the HTTP server to.\n\n"
            "127.0.0.1 — localhost only (default)\n"
            "0.0.0.0   — all interfaces (LAN-accessible)\n"
            "127.0.0.2 — dedicated loopback address:\n"
            "  macOS: works without root\n"
            "  Linux: needs: sudo ip addr add 127.0.0.2/8 dev lo\n"
            "  Windows: needs admin\n"
            "Falls back to 127.0.0.1 if the address is unavailable."
          );
        }

        if (g_m4_http.is_running()) {
          ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f),
            "Listening on %s:%d", g_m4_http.bind_ip().c_str(), g_m4_http.port());
          ImGui::SameLine();
          if (ImGui::SmallButton("Stop##m4http")) {
            g_m4_http.stop();
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Open in Browser##m4http")) {
            char url[128];
            snprintf(url, sizeof(url), "http://%s:%d/",
                     g_m4_http.bind_ip().c_str(), g_m4_http.port());
            SDL_OpenURL(url);
          }
        } else {
          ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "HTTP server stopped");
          ImGui::SameLine();
          if (ImGui::SmallButton("Start##m4http")) {
            g_m4_http.start(CPC.m4_http_port, CPC.m4_bind_ip);
          }
        }

        // ── Port Forwarding ──
        ImGui::Separator();
        ImGui::TextDisabled("Port Forwarding");
        ImGui::TextWrapped("When CPC software binds a port (C_NETBIND), the emulator maps it "
          "to a host port. User overrides (white) are persisted; auto-assigned (gray) are not.");

        if (ImGui::BeginTable("##m4ports", 5,
              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
          ImGui::TableSetupColumn("CPC Port", ImGuiTableColumnFlags_WidthFixed, 70.0f);
          ImGui::TableSetupColumn("Host Port", ImGuiTableColumnFlags_WidthFixed, 70.0f);
          ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 50.0f);
          ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 50.0f);
          ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableHeadersRow();

          auto mappings = g_m4_http.get_port_mappings_snapshot();
          for (size_t mi = 0; mi < mappings.size(); mi++) {
            const auto& pm = mappings[mi];
            ImGui::PushID(static_cast<int>(mi));
            ImGui::TableNextRow();
            ImVec4 color = pm.user_override ? ImVec4(1,1,1,1) : ImVec4(0.6f,0.6f,0.6f,1);

            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%d", pm.cpc_port);

            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%d", pm.host_port);

            ImGui::TableNextColumn();
            ImGui::TextColored(pm.active ? ImVec4(0.2f,0.9f,0.2f,1) : ImVec4(0.5f,0.5f,0.5f,1),
              "%s", pm.active ? "active" : "idle");

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", pm.user_override ? "user" : "auto");

            ImGui::TableNextColumn();
            if (!pm.description.empty()) {
              ImGui::TextDisabled("%s", pm.description.c_str());
            }
            if (pm.user_override) {
              ImGui::SameLine();
              if (ImGui::SmallButton("X")) {
                g_m4_http.remove_port_mapping(pm.cpc_port);
              }
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove user override");
            }
            ImGui::PopID();
          }
          ImGui::EndTable();
        }

        // Add manual mapping row
        static int new_cpc_port = 80, new_host_port = 8080;
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt("##newcpc", &new_cpc_port, 0, 0);
        ImGui::SameLine();
        ImGui::TextDisabled("->");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt("##newhost", &new_host_port, 0, 0);
        ImGui::SameLine();
        if (ImGui::SmallButton("Add Mapping")) {
          if (new_cpc_port > 0 && new_cpc_port <= 65535 &&
              new_host_port > 0 && new_host_port <= 65535) {
            g_m4_http.set_port_mapping(
              static_cast<uint16_t>(new_cpc_port),
              static_cast<uint16_t>(new_host_port), true);
          }
        }
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
    // Start/stop M4 HTTP server based on M4 enabled state
    // Only auto-start when M4 was just enabled (not on every Save),
    // so that a manual "Stop" in the UI stays effective.
    if (g_m4board.enabled && !old_m4_enabled &&
        !g_m4board.sd_root_path.empty() && !g_m4_http.is_running()) {
      g_m4_http.start(CPC.m4_http_port, CPC.m4_bind_ip);
    } else if (!g_m4board.enabled && g_m4_http.is_running()) {
      g_m4_http.stop();
    }
    update_cpc_speed();
    video_set_palette();
    imgui_state.show_options = false;
    CPC.paused = false;
    first_open = true;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Apply changes and save to config file");
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
  if (ImGui::Button("Apply", ImVec2(80, 0))) {
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
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Apply changes for this session only\n(not saved to config file)");
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
  float bar_y = vp->Pos.y + s_menubar_h + static_cast<float>(s_main_topbar_h);

  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, bar_y));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, 0));  // auto-height
  ImGui::SetNextWindowViewport(vp->ID);  // keep on main viewport
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
    if (ImGui::Button("ASM")) g_devtools_ui.toggle_window("assembler");

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
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Execute one instruction (enters CALLs)"); }
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
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Execute one instruction (skips over CALLs/RSTs)"); }
    ImGui::SameLine();
    if (ImGui::Button("Step Out")) {
      z80.step_out = 1;
      z80.step_out_addresses.clear();
      z80.step_in = 0;
      CPC.paused = false;
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Run until the current subroutine returns"); }
    if (!was_paused) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(CPC.paused ? "Resume" : "Pause")) {
      CPC.paused = !CPC.paused;
    }

    // ── Per-window render timing ──
    {
      ImGui::SameLine(0, 16.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
      float total_us = 0;
      const auto* timings = g_devtools_ui.window_timings();
      for (int i = 0; i < DevToolsUI::NUM_WINDOWS; i++) {
        if (timings[i].last_us > 0.01f) total_us += timings[i].last_us;
      }
      char buf[32];
      snprintf(buf, sizeof(buf), "dt:%.1fms", total_us / 1000.0f);
      ImGui::TextUnformatted(buf);
      if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        for (int i = 0; i < DevToolsUI::NUM_WINDOWS; i++) {
          if (timings[i].last_us > 0.01f && timings[i].name) {
            ImGui::Text("%-14s %6.0f us", timings[i].name, timings[i].last_us);
          }
        }
        ImGui::EndTooltip();
      }
      ImGui::PopStyleColor();
    }

    // ── Audio diagnostics ──
    ImGui::SameLine(0, 8.0f);
    {
      bool has_underruns = imgui_state.audio_underruns > 0;
      ImGui::PushStyleColor(ImGuiCol_Text, has_underruns
        ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)    // red if underruns
        : ImVec4(0.4f, 0.4f, 0.4f, 1.0f));   // gray if healthy
      char abuf[32];
      snprintf(abuf, sizeof(abuf), "snd:%.0fms", imgui_state.audio_queue_avg_ms);
      ImGui::TextUnformatted(abuf);
      if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Audio queue avg: %.1f ms", imgui_state.audio_queue_avg_ms);
        ImGui::Text("Audio queue min: %.1f ms", imgui_state.audio_queue_min_ms);
        ImGui::Text("Push interval max: %.0f us", imgui_state.audio_push_interval_max_us);
        ImGui::Text("Pushes/sec: %d", imgui_state.audio_pushes);
        ImGui::Text("Underruns/sec: %d", imgui_state.audio_underruns);
        ImGui::EndTooltip();
      }
      ImGui::PopStyleColor();
    }

    // ── Sync devtools bar height ──
    {
      int h = static_cast<int>(ImGui::GetWindowSize().y);
      if (h != s_devtools_bar_h) { s_devtools_bar_h = h; s_topbar_height_dirty = true; }
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

  // Active mode indicator
  if (imgui_state.mem_filter_value >= 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
    ImGui::Text("[FILTER: %02X]", imgui_state.mem_filter_value & 0xFF);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##mtclear")) { imgui_state.mem_filter_value = -1; }
  } else if (imgui_state.mem_display_value >= 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
    ImGui::Text("[DISPLAY: %04X]", imgui_state.mem_display_value & 0xFFFF);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##mtclear")) { imgui_state.mem_display_value = -1; }
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
        format_memory_line(line, sizeof(line), base, bpl, 0, pbRAM);
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
          format_memory_line(line, sizeof(line), base, bpl, 0, pbRAM);
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

  // Helper: check if a CPC key is currently pressed in the keyboard matrix.
  // CPC_KEYS enum values are NOT matrix positions — must convert via scancode table.
  auto cpc_key_down = [](byte cpc_key) -> bool {
    extern byte keyboard_matrix[];
    extern byte bit_values[];
    CPCScancode sc = CPC.InputMapper->CPCscancodeFromCPCkey(static_cast<CPC_KEYS>(cpc_key));
    byte row = static_cast<byte>(sc >> 4);
    byte bit = static_cast<byte>(sc & 7);
    return row < 16 && !(keyboard_matrix[row] & bit_values[bit]);
  };
  // Helper: draw blue overlay on last ImGui item if CPC key is pressed
  auto highlight_if_pressed = [&](byte cpc_key) {
    if (cpc_key_down(cpc_key)) {
      ImVec2 rmin = ImGui::GetItemRectMin();
      ImVec2 rmax = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddRectFilled(rmin, rmax, IM_COL32(80, 180, 255, 180), 3.0f);
    }
  };

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

  // ── Key dispatch: render Button, emit keypress, highlight if held ──
  // vk() renders a single CPC key button with pressed-key overlay.
  // Helper to build a 2-byte "\a<CPC_KEY>" emit string from enum value
  auto cpc_emit = [](byte cpc_key) -> std::string {
    return std::string("\a") + static_cast<char>(cpc_key);
  };

  auto vk = [&](const char* label, float w, const char* es, byte cpc_key) {
    if (ImGui::Button(label, ImVec2(w, H))) emit_key(es);
    highlight_if_pressed(cpc_key);
    ImGui::SameLine(0, S);
  };
  // vk_end() — same but no SameLine (end of row)
  auto vk_end = [&](const char* label, float w, const char* es, byte cpc_key) {
    if (ImGui::Button(label, ImVec2(w, H))) emit_key(es);
    highlight_if_pressed(cpc_key);
  };
  // vk_cpc() — emit CPC key by enum (no hardcoded hex)
  auto vk_cpc = [&](const char* label, float w, byte cpc_key, bool end = false) {
    std::string es = cpc_emit(cpc_key);
    if (ImGui::Button(label, ImVec2(w, H))) emit_key(es.c_str());
    highlight_if_pressed(cpc_key);
    if (!end) ImGui::SameLine(0, S);
  };

  // ═══════════════════ ROW 0 ═══════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0));
  vk("ESC", K, cpc_emit(CPC_ESC).c_str(), CPC_ESC);
  vk("!\n1", K, "1", CPC_1);      vk("\"\n2", K, "2", CPC_2);
  vk("#\n3", K, "3", CPC_3);      vk("$\n4", K, "4", CPC_4);
  vk("%\n5", K, "5", CPC_5);      vk("&\n6", K, "6", CPC_6);
  vk("'\n7", K, "7", CPC_7);      vk("(\n8", K, "8", CPC_8);
  vk(")\n9", K, "9", CPC_9);      vk("_\n0", K, "0", CPC_0);
  vk("=\n-", K, "-", CPC_MINUS);  vk("\xc2\xa3\n^", K, "^", CPC_POWER);
  vk("CLR", K, cpc_emit(CPC_CLR).c_str(), CPC_CLR);
  vk_end("DEL", K, "\b", CPC_DEL);
  // Numpad
  ImGui::SetCursorPos(ImVec2(np_x, y0));
  vk_cpc("F7", K, CPC_F7);  vk_cpc("F8", K, CPC_F8);  vk_cpc("F9", K, CPC_F9, true);

  // ═══════════════════ ROW 1 ═══════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + ROW));
  vk("TAB", K*W_TAB, "\t", CPC_TAB);
  vk("Q", K, "q", CPC_Q);  vk("W", K, "w", CPC_W);  vk("E", K, "e", CPC_E);
  vk("R", K, "r", CPC_R);  vk("T", K, "t", CPC_T);  vk("Y", K, "y", CPC_Y);
  vk("U", K, "u", CPC_U);  vk("I", K, "i", CPC_I);  vk("O", K, "o", CPC_O);
  vk("P", K, "p", CPC_P);
  vk("|\n@", K, "@", CPC_AT);  vk("{\n[", K, "[", CPC_LBRACKET);
  // RETURN upper part — fills to main_end_x
  float ret_x = ImGui::GetCursorPosX();
  float ret_w = main_end_x - ret_x;
  vk_end("RETURN##1", ret_w, "\n", CPC_RETURN);
  // RETURN lower part (L-shape into row 2)
  float ret2_x = x0 + K*W_CAPS + S + 12*(K + S);
  float ret2_w = main_end_x - ret2_x;
  ImGui::SetCursorPos(ImVec2(ret2_x, y0 + ROW + H));
  if (ImGui::Button("##ret2", ImVec2(ret2_w, ROW))) emit_key("\n");
  highlight_if_pressed(CPC_RETURN);
  // Numpad
  ImGui::SetCursorPos(ImVec2(np_x, y0 + ROW));
  vk_cpc("F4", K, CPC_F4);  vk_cpc("F5", K, CPC_F5);  vk_cpc("F6", K, CPC_F6, true);

  // ═══════════════════ ROW 2 ═══════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + ROW * 2));
  if (caps_on) ImGui::PushStyleColor(ImGuiCol_Button, mod_on_color);
  if (ImGui::Button("CAPS\nLOCK", ImVec2(K*W_CAPS, H))) emit_key("\x01" "CAPS");
  if (caps_on) ImGui::PopStyleColor();
  highlight_if_pressed(CPC_CAPSLOCK); ImGui::SameLine(0, S);
  vk("A", K, "a", CPC_A);  vk("S", K, "s", CPC_S);  vk("D", K, "d", CPC_D);
  vk("F", K, "f", CPC_F);  vk("G", K, "g", CPC_G);  vk("H", K, "h", CPC_H);
  vk("J", K, "j", CPC_J);  vk("K", K, "k", CPC_K);  vk("L", K, "l", CPC_L);
  vk("+\n;", K, ";", CPC_SEMICOLON);  vk("*\n:", K, ":", CPC_COLON);
  vk_end("}\n]", K, "]", CPC_RBRACKET);
  // Numpad
  ImGui::SetCursorPos(ImVec2(np_x, y0 + ROW * 2));
  vk_cpc("F1", K, CPC_F1);  vk_cpc("F2", K, CPC_F2);  vk_cpc("F3", K, CPC_F3, true);

  // ═══════════════════ ROW 3 ═══════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + ROW * 3));
  bool shift_highlight = imgui_state.vkeyboard_shift_next;
  if (shift_highlight) ImGui::PushStyleColor(ImGuiCol_Button, mod_on_color);
  if (ImGui::Button("SHIFT##L", ImVec2(K*W_LSHIFT, H))) emit_key("\x01" "SHIFT");
  if (shift_highlight) ImGui::PopStyleColor();
  highlight_if_pressed(CPC_LSHIFT); ImGui::SameLine(0, S);
  vk("Z", K, "z", CPC_Z);  vk("X", K, "x", CPC_X);  vk("C", K, "c", CPC_C);
  vk("V", K, "v", CPC_V);  vk("B", K, "b", CPC_B);  vk("N", K, "n", CPC_N);
  vk("M", K, "m", CPC_M);
  vk("<\n,", K, ",", CPC_COMMA);    vk(">\n.##main", K, ".", CPC_PERIOD);
  vk("?\n/", K, "/", CPC_SLASH);    vk("`\n\\", K, "\\", CPC_BACKSLASH);
  // Right SHIFT — fills to main_end_x
  float rshift_x = ImGui::GetCursorPosX();
  float rshift_w = main_end_x - rshift_x;
  if (shift_highlight) ImGui::PushStyleColor(ImGuiCol_Button, mod_on_color);
  if (ImGui::Button("SHIFT##R", ImVec2(rshift_w, H))) emit_key("\x01" "SHIFT");
  if (shift_highlight) ImGui::PopStyleColor();
  highlight_if_pressed(CPC_RSHIFT);
  // Numpad
  ImGui::SetCursorPos(ImVec2(np_x, y0 + ROW * 3));
  vk_cpc("F0", K, CPC_F0);
  vk("\xe2\x86\x91##up", K, cpc_emit(CPC_CUR_UP).c_str(), CPC_CUR_UP);
  vk_end(".##np", K, ".", CPC_FPERIOD);

  // ═══════════════════ ROW 4 ═══════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + ROW * 4));
  if (ctrl_on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.4f, 0.2f, 1.0f));
  if (ImGui::Button("CTRL", ImVec2(K*W_CTRL, H))) emit_key("\x01" "CTRL");
  if (ctrl_on) ImGui::PopStyleColor();
  highlight_if_pressed(CPC_CONTROL); ImGui::SameLine(0, S);
  vk("COPY", K*W_COPY, cpc_emit(CPC_COPY).c_str(), CPC_COPY);
  float space_w = K * 8.0f;
  vk("SPACE", space_w, " ", CPC_SPACE);
  float enter_x = ImGui::GetCursorPosX();
  float enter_w = main_end_x - enter_x;
  vk_end("ENTER", enter_w, "\n", CPC_ENTER);
  // Numpad
  ImGui::SetCursorPos(ImVec2(np_x, y0 + ROW * 4));
  vk("\xe2\x86\x90##left", K, cpc_emit(CPC_CUR_LEFT).c_str(), CPC_CUR_LEFT);
  vk("\xe2\x86\x93##down", K, cpc_emit(CPC_CUR_DOWN).c_str(), CPC_CUR_DOWN);
  vk_end("\xe2\x86\x92##right", K, cpc_emit(CPC_CUR_RIGHT).c_str(), CPC_CUR_RIGHT);

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

