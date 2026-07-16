#include "imgui_ui.h"
#include <cstdint>
#include "subcycle_bridge.h"
#include "subcycle/machine.h"

// Speed Test (Run Tier menu): OpenPopup must fire outside the menu's ID
// scope, so the click only raises this and the modal renders post-menu.
namespace {
bool g_speedtest_open = false;
}  // namespace

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "amdrum.h"
#include "amx_mouse.h"
#include "command_palette.h"
#include "crtc_types.h"
#include "devtools_ui.h"
#include "disk_format.h"
#include "drive_sounds.h"
#include "fileutils.h"
#include "flux_save.h"
#include "hw_views.h"
#include "imgui.h"
#include "imgui_ui_testable.h"
#include "keyboard.h"
#include "koncepcja.h"
#include "log.h"
#include "m4board.h"
#include "m4board_http.h"
#include "macos_menu.h"  // koncpc_restore_keyboard_focus
#include "menu_actions.h"
#include "menu_bridge.h"
#include "plotter.h"
#include "plotter_view.h"
#include "rom_identify.h"
#include "serial_interface.h"
#include "slotshandler.h"
#include "smartwatch.h"
#include "symbiface.h"
#include "symfile.h"
#include "tape_line_in.h"
#include "video_host.h"
#include "vjoystick_map.h"
#include "workspace_layout.h"
#include "z80_disassembly.h"
#include "z80_view.h"

extern SDL_Window* mainSDLWindow;
extern t_CPC CPC;
extern t_z80regs z80;
extern t_CRTC CRTC;
extern t_GateArray GateArray;
extern t_PSG PSG;
extern t_drive driveA;
extern t_drive driveB;
extern byte* pbRAM;
extern byte* memmap_ROM[256];
extern bool g_debug;
extern byte bit_values[];
extern byte* pbExpansionROM;
extern byte* pbROMhi;

extern std::vector<byte> pbTapeImage;
extern byte* pbTapeImageEnd;
extern dword dwTapeZeroPulseCycles;
extern dword dwFrameCountOverall;

// `imgui_state` singleton lives in src/imgui_state.cpp so the symbol is
// present in both MODERN_UI=ON and OFF builds — the core writes telemetry
// into it from headless TUs that don't link imgui_ui.cpp.

// Forward declarations
namespace {
void imgui_render_menubar();
}  // namespace
namespace {
void imgui_render_topbar();
}  // namespace
namespace {
void imgui_render_statusbar();
}  // namespace
namespace {
void imgui_render_menu();
}  // namespace
namespace {
void imgui_render_options();
}  // namespace
namespace {
void imgui_render_devtools();
}  // namespace
namespace {
void imgui_render_memory_tool();
}  // namespace
namespace {
void imgui_render_vkeyboard();
}  // namespace
namespace {
void imgui_render_vjoystick();
}  // namespace

// Declared in imgui_ui.h — close menu and unpause unless a dialog is open
namespace {
void mru_push(std::vector<std::string>& list, const std::string& path);
}  // namespace

// Height tracking for stacked menubar + topbar + devtools bar
namespace {
float s_menubar_h = 19.0f;  // ImGui main menu bar default height
}  // namespace
namespace {
int s_main_topbar_h = 25;
}  // namespace
namespace {
int s_devtools_bar_h = 0;
}  // namespace
namespace {
ImVec2 s_layout_btn_pos;  // set in topbar, read in layout dropdown
}  // namespace
namespace {
bool s_topbar_height_dirty =
    false;  // defer SDL_SetWindowSize to after render
}  // namespace
namespace {
int s_statusbar_h = 0;
}  // namespace
namespace {
bool s_bottombar_height_dirty =
    false;  // defer SDL_SetWindowSize to after render
}  // namespace

// Deep-link target for the Settings/Options window.  A Machine-menu item sets
// this, then imgui_render_options() selects the matching tab for exactly one
// frame (via ImGuiTabItemFlags_SetSelected) and resets it back to None so the
// user can click other tabs freely afterward.  Order mirrors the tab bar.
namespace {
enum class OptionsTab : std::uint8_t {
  None = 0,
  General,
  ROMs,
  Video,
  Audio,
  Input,
  M4,
  Serial,
};
}  // namespace
namespace {
OptionsTab s_pending_options_tab = OptionsTab::None;
}  // namespace

// ─────────────────────────────────────────────────
// SDL3 file dialog callback
// ─────────────────────────────────────────────────

namespace {
void SDLCALL file_dialog_callback(void* userdata,
                                         const char* const* filelist,
                                         int /*filter*/) {
  // After the dialog dismisses (whether the user picked a file or
  // cancelled), restore keyboard routing to the emulator.
  //
  // The hard part is OS-level: on macOS SDL_ShowOpenFileDialog runs as
  // an NSOpenPanel sheet attached to the parent NSWindow.  When the
  // sheet dismisses, AppKit doesn't always restore the SDL content view
  // as the window's firstResponder — keystrokes then route to the
  // NSApplication menu bar instead of [SDLContentView keyDown:], so
  // SDL never enqueues a SDL_EVENT_KEY_DOWN.  No ImGui-level gating
  // matters because the event never arrives.  SDL_RaiseWindow calls
  // [window makeKeyAndOrderFront:] which forces firstResponder back to
  // the SDL view.
  //
  // Then in Docked mode also tell the workspace renderer to refocus
  // the CPC Screen panel so cpc_screen_focused becomes true again.
  koncpc_restore_keyboard_focus();  // macOS: Cocoa makeFirstResponder; no-op
                                    // elsewhere
  imgui_state.request_cpc_screen_focus = true;

  auto action =
      static_cast<FileDialogAction>(reinterpret_cast<intptr_t>(userdata));
  if (!filelist || !filelist[0]) return;  // cancelled or error
  imgui_state.pending_dialog = action;
  imgui_state.pending_dialog_result = filelist[0];
}
}  // namespace

// Save a live drive medium to `path` as `fmt`. Under engine=1 the bytes come
// from the sub-cycle FDC's live medium (so engine=1 WRITE DATA / FORMAT are
// never dropped — the driveA/driveB structs they bypass are NOT the source);
// only a legacy engine=0 run (no bridge) falls back to the driveA/driveB struct
// via dsk_save, and flux formats never reach that branch (they are engine=1
// only, gated out of the menu when the bridge is inactive).
namespace {
void save_disk_to(int unit, SaveFormat fmt, const std::string& path,
                         const std::string& fname) {
  std::string err;
  bool ok = false;
  if (subcycle_bridge_active()) {
    ok = flux_save_to_file(unit, fmt, path, err);
  } else {
    ok = dsk_save(path, unit == 0 ? &driveA : &driveB) == 0;
    if (!ok) err = "write error";
  }
  if (ok) {
    imgui_toast_success("Saved disk " + std::string(unit == 0 ? "A" : "B") +
                        ": " + fname);
  } else {
    imgui_toast_error("Failed to save disk: " + fname +
                      (err.empty() ? "" : " (" + err + ")"));
  }
}
}  // namespace

// Create a blank disc at `path` (Sector or Flux per the New-disk modal) and
// load it into drive A through the normal file_load path, so both the legacy
// struct and the sub-cycle bridge receive it (file_load transcodes .scp/.hfe to
// writable flux for engine=1).
namespace {
void create_new_disk(const std::string& path, const std::string& fname) {
  const DiskBacking backing =
      imgui_state.new_disk_flux ? DiskBacking::Flux : DiskBacking::Sector;
  const std::string err = disk_create_new(path, "data", backing, nullptr);
  if (!err.empty()) {
    imgui_toast_error("Failed to create disk: " + err);
    return;
  }
  CPC.driveA.file = path;
  if (file_load(CPC.driveA) == 0) {
    imgui_toast_success("New disk in A: " + fname);
    mru_push(CPC.mru_disks, path);
  } else {
    imgui_toast_error("Created but failed to load: " + fname);
  }
}
}  // namespace

namespace {
void process_pending_dialog() {
  if (imgui_state.pending_dialog == FileDialogAction::None) return;

  FileDialogAction const action = imgui_state.pending_dialog;
  std::string const path = imgui_state.pending_dialog_result;
  int const rom_slot = imgui_state.pending_rom_slot;

  imgui_state.pending_dialog = FileDialogAction::None;
  imgui_state.pending_dialog_result.clear();
  imgui_state.pending_rom_slot = -1;

  std::string const dir = path.substr(0, path.find_last_of("/\\"));
  auto fname = std::filesystem::path(path).filename().string();

  switch (action) {
    case FileDialogAction::LoadDiskA:
    case FileDialogAction::LoadDiskA_LED:
      CPC.driveA.file = path;
      if (file_load(CPC.driveA) == 0) {
        imgui_toast_success("Drive A: " + fname);
        mru_push(CPC.mru_disks, path);
      } else {
        imgui_toast_error("Failed to load disk: " + fname);
      }
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::LoadDiskB:
    case FileDialogAction::LoadDiskB_LED:
      CPC.driveB.file = path;
      if (file_load(CPC.driveB) == 0) {
        imgui_toast_success("Drive B: " + fname);
        mru_push(CPC.mru_disks, path);
      } else {
        imgui_toast_error("Failed to load disk: " + fname);
      }
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::SaveDiskA:
      save_disk_to(0, SaveFormat::Dsk, path, fname);
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::SaveDiskB:
      save_disk_to(1, SaveFormat::Dsk, path, fname);
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::SaveDiskA_SCP:
      save_disk_to(0, SaveFormat::Scp, path, fname);
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::SaveDiskA_HFE:
      save_disk_to(0, SaveFormat::Hfe, path, fname);
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::NewDiskA:
      create_new_disk(path, fname);
      CPC.current_dsk_path = dir;
      break;
    case FileDialogAction::LoadSnapshot:
      CPC.snapshot.file = path;
      if (file_load(CPC.snapshot) == 0) {
        imgui_toast_success("Snapshot loaded: " + fname);
        mru_push(CPC.mru_snaps, path);
      } else {
        imgui_toast_error("Failed to load snapshot: " + fname);
      }
      CPC.current_snap_path = dir;
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
      } else {
        imgui_toast_error("Failed to load tape: " + fname);
      }
      CPC.current_tape_path = dir;
      break;
    case FileDialogAction::LoadTape_LED:
      CPC.tape.file = path;
      if (file_load(CPC.tape) == 0) {
        imgui_toast_success("Tape loaded: " + fname);
        mru_push(CPC.mru_tapes, path);
        tape_scan_blocks();
      } else {
        imgui_toast_error("Failed to load tape: " + fname);
      }
      CPC.current_tape_path = dir;
      break;
    case FileDialogAction::LoadCartridge:
      CPC.cartridge.file = path;
      if (file_load(CPC.cartridge) == 0) {
        imgui_toast_success("Cartridge loaded: " + fname);
        mru_push(CPC.mru_carts, path);
      } else {
        imgui_toast_error("Failed to load cartridge: " + fname);
      }
      CPC.current_cart_path = dir;
      emulator_reset();
      break;
    case FileDialogAction::LoadROM:
      if (rom_slot >= 0 && rom_slot < MAX_ROM_SLOTS)
        CPC.rom_file[rom_slot] = path;
      break;
    case FileDialogAction::SelectM4SDFolder:
      g_m4board.sd_root_path = path;
      if (g_m4board.enabled) emulator_init();
      break;
    case FileDialogAction::SavePlotterSVG:
      if (plotter_view_export_svg(path))
        imgui_toast_success("SVG exported to " + fname);
      else
        imgui_toast_error("Failed to export SVG");
      break;
    default:
      break;
  }

  // Always close the menu after any file dialog completes.
  // For status bar LED actions, show_menu is already false (no-op).
  // For Options sub-dialogs, imgui_close_menu() skips unpause while
  // show_options is still true.
  imgui_close_menu();
  ImGui::SetWindowFocus(nullptr);
}
}  // namespace

// ─────────────────────────────────────────────────
// Theme setup
// ─────────────────────────────────────────────────

void imgui_init_ui() {
  // Merge transport symbol glyphs from system font into default font
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->AddFontDefault();

  // Prevent accidental dragging of windows by clicking on their body.
  // Only the title bar can be used to move windows.
  io.ConfigWindowsMoveFromTitleBarOnly = true;

#if defined(__APPLE__) || defined(_WIN32)
  // Merge a system symbol font for transport control glyphs (play/stop/eject
  // etc.)
  {
    ImFontConfig merge_cfg;
    merge_cfg.MergeMode = true;
    merge_cfg.PixelSnapH = true;
    static const ImWchar symbol_ranges[] = {
        0x23CF, 0x23CF,  // ⏏
        0x25A0, 0x25A0,  // ■
        0x25B6, 0x25B6,  // ▶
        0x25C0, 0x25C0,  // ◀
        0,
    };
#ifdef __APPLE__
    io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Apple Symbols.ttf",
                                 13.0f, &merge_cfg, symbol_ranges);
#elif defined(_WIN32)
    {
      std::filesystem::path fonts_dir = "C:\\Windows\\Fonts";
      if (const char* sys_root = getenv("SystemRoot")) {
        fonts_dir = std::filesystem::path(sys_root) / "Fonts";
      }
      // Try Segoe UI Symbol first, then Segoe UI Emoji, then Arial Unicode MS
      const char* candidates[] = {"seguisym.ttf", "seguiemj.ttf",
                                  "ARIALUNI.TTF"};
      for (const char* name : candidates) {
        auto path = (fonts_dir / name).string();
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), 13.0f, &merge_cfg,
                                         symbol_ranges))
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
  c[ImGuiCol_WindowBg] = ImVec4(0.102f, 0.102f, 0.118f, 1.00f);
  c[ImGuiCol_PopupBg] = ImVec4(0.120f, 0.120f, 0.140f, 0.95f);
  c[ImGuiCol_ChildBg] = ImVec4(0.090f, 0.090f, 0.105f, 1.00f);
  // Text: 0xF0F0F0
  c[ImGuiCol_Text] = ImVec4(0.941f, 0.941f, 0.941f, 1.00f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.500f, 0.500f, 0.500f, 1.00f);
  // Accent amber: 0x8A6A10
  c[ImGuiCol_Header] = ImVec4(0.541f, 0.416f, 0.063f, 0.40f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.541f, 0.416f, 0.063f, 0.60f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.541f, 0.416f, 0.063f, 0.80f);
  c[ImGuiCol_Button] = ImVec4(0.541f, 0.416f, 0.063f, 0.45f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.600f, 0.480f, 0.100f, 0.70f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.650f, 0.520f, 0.130f, 0.90f);
  // Selection blue: 0x3D5AFE
  c[ImGuiCol_Tab] = ImVec4(0.240f, 0.353f, 0.996f, 0.30f);
  c[ImGuiCol_TabHovered] = ImVec4(0.240f, 0.353f, 0.996f, 0.60f);
  c[ImGuiCol_TabSelected] = ImVec4(0.240f, 0.353f, 0.996f, 0.80f);
  c[ImGuiCol_TabSelectedOverline] = ImVec4(0.240f, 0.353f, 0.996f, 1.00f);
  // Frame/border
  c[ImGuiCol_FrameBg] = ImVec4(0.160f, 0.160f, 0.180f, 1.00f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.200f, 0.200f, 0.230f, 1.00f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.240f, 0.240f, 0.280f, 1.00f);
  c[ImGuiCol_Border] = ImVec4(0.300f, 0.300f, 0.350f, 0.50f);
  c[ImGuiCol_TitleBg] = ImVec4(0.080f, 0.080f, 0.100f, 1.00f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.120f, 0.120f, 0.150f, 1.00f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.080f, 0.080f, 0.100f, 0.60f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.300f, 0.300f, 0.350f, 0.80f);
  c[ImGuiCol_CheckMark] = ImVec4(0.541f, 0.416f, 0.063f, 1.00f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.541f, 0.416f, 0.063f, 0.80f);
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.650f, 0.520f, 0.130f, 1.00f);
  c[ImGuiCol_Separator] = ImVec4(0.300f, 0.300f, 0.350f, 0.50f);

  // When viewports are enabled, platform windows should not have rounded
  // corners
  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    style.WindowRounding = 0.0f;
    c[ImGuiCol_WindowBg].w = 1.0f;
  }

  // Register command palette entries from menu actions
  g_command_palette.clear_commands();
  for (const auto& ma : koncpc_menu_actions()) {
    if (ma.title[0] == '\0') continue;  // skip empty entries
    std::string const title = ma.title;
    KONCPC_KEYS const action_key = ma.action;
    std::string const shortcut = koncpc_action_shortcut(action_key);
    g_command_palette.register_command(title, "", shortcut, [action_key]() {
      
      applyKeypress(static_cast<CPCScancode>(action_key), keyboard_matrix,
                    true);
      applyKeypress(static_cast<CPCScancode>(action_key), keyboard_matrix,
                    false);
    });
  }
  // Extra commands
  g_command_palette.register_command(
      "Pause / Resume", "Toggle emulation pause", "Pause", []() {
        
        if (g_emu_paused.load(std::memory_order_relaxed))
          cpc_resume();
        else
          cpc_pause();
      });
  g_command_palette.register_command(
      "Registers", "Show CPU registers", "",
      []() { g_devtools_ui.toggle_window("registers"); });
  g_command_palette.register_command(
      "Disassembly", "Show disassembly view", "",
      []() { g_devtools_ui.toggle_window("disassembly"); });
  g_command_palette.register_command(
      "Memory Hex", "Show memory hex view", "",
      []() { g_devtools_ui.toggle_window("memory_hex"); });
  g_command_palette.register_command("Stack", "Show stack window", "", []() {
    g_devtools_ui.toggle_window("stack");
  });
  g_command_palette.register_command(
      "Breakpoints", "Show breakpoint list", "",
      []() { g_devtools_ui.toggle_window("breakpoints"); });
  g_command_palette.register_command(
      "Symbol Table", "Show symbol table", "",
      []() { g_devtools_ui.toggle_window("symbols"); });
  g_command_palette.register_command(
      "Session Recording", "Show session recording controls", "",
      []() { g_devtools_ui.toggle_window("session_recording"); });
  g_command_palette.register_command(
      "Graphics Finder", "Show graphics finder/tile viewer", "",
      []() { g_devtools_ui.toggle_window("gfx_finder"); });
  g_command_palette.register_command(
      "Silicon Disc", "Show Silicon Disc panel", "",
      []() { g_devtools_ui.toggle_window("silicon_disc"); });
  g_command_palette.register_command(
      "ASIC Registers", "Show ASIC register viewer", "",
      []() { g_devtools_ui.toggle_window("asic"); });
  g_command_palette.register_command(
      "Disc Tools", "Show disc file/sector tools", "",
      []() { g_devtools_ui.toggle_window("disc_tools"); });
  g_command_palette.register_command(
      "Data Areas", "Show data area manager", "",
      []() { g_devtools_ui.toggle_window("data_areas"); });
  g_command_palette.register_command(
      "Disasm Export", "Export disassembly to file", "",
      []() { g_devtools_ui.toggle_window("disasm_export"); });
  g_command_palette.register_command(
      "Recording Controls", "WAV/YM/AVI recording start/stop", "",
      []() { g_devtools_ui.toggle_window("recording_controls"); });
  g_command_palette.register_command(
      "Assembler", "Z80 assembler IDE", "",
      []() { g_devtools_ui.toggle_window("assembler"); });
  g_command_palette.register_command(
      "Drive Sound Lab", "Tune procedural drive/tape sound effects", "",
      []() { g_devtools_ui.toggle_window("drive_sound_lab"); });
  g_command_palette.register_command(
      "Serial Terminal", "Show serial terminal window", "F3", []() {
        imgui_state.show_serial_terminal = !imgui_state.show_serial_terminal;
      });
  g_command_palette.register_command(
      "Plotter Preview", "HP-GL plotter output preview and SVG export", "",
      []() {
        imgui_state.show_plotter_preview = !imgui_state.show_plotter_preview;
      });
}

// ─────────────────────────────────────────────────
// Main dispatcher
// ─────────────────────────────────────────────────

void imgui_render_ui() {
  // Reconcile ImGui mouse state with hardware — defense against stuck buttons.
  // Only poll hardware state when ImGui thinks a button is down, to avoid the
  // overhead of SDL_GetGlobalMouseState() every frame (slow on X11).
  {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2]) {
      SDL_MouseButtonFlags const hw_buttons =
          SDL_GetGlobalMouseState(nullptr, nullptr);
      // SDL button indices: 1=Left, 2=Middle, 3=Right
      // ImGui button indices: 0=Left, 1=Right, 2=Middle
      static const int sdl_button[] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT,
                                       SDL_BUTTON_MIDDLE};
      for (int i = 0; i < 3; i++) {
        bool const hw_down = (hw_buttons & SDL_BUTTON_MASK(sdl_button[i])) != 0;
        if (io.MouseDown[i] && !hw_down) {
          io.MouseDown[i] = false;
        }
      }
    }
  }

  process_pending_dialog();
  // Dockspace host must be rendered before other windows so they can dock into
  // it
  workspace_render_dockspace();
  // TODO(beads-34s): add a small dark host gutter / 1px inner bezel around the
  // emulated CPC screen so it doesn't touch the topbar/status bar. The screen
  // texture/quad is drawn in workspace_render_cpc_screen()
  // (workspace_layout.cpp), which is outside this file's edit scope — handle it
  // there.
  workspace_render_cpc_screen();
  imgui_render_menubar();
  imgui_render_topbar();
  imgui_render_statusbar();
  if (imgui_state.show_menu) imgui_render_menu();
  if (imgui_state.show_options) imgui_render_options();
  if (imgui_state.show_serial_terminal) imgui_render_serial_terminal();
  if (imgui_state.show_plotter_preview) imgui_render_plotter_preview();
  if (imgui_state.show_devtools) imgui_render_devtools();
  if (imgui_state.show_memory_tool) imgui_render_memory_tool();
  if (imgui_state.show_vkeyboard) imgui_render_vkeyboard();
  imgui_render_vjoystick();  // self-gates on show_vjoystick; always ticks so a
                             // closing window can release held matrix bits
  // Phase 2 debug windows (extracted to DevToolsUI)
  g_devtools_ui.render();
  g_command_palette.render();

  // ── Toast notifications ──
  {
    ImGuiIO const& io = ImGui::GetIO();
    float const dt = io.DeltaTime;
    float yOffset = 40.0f;  // bottom margin
    float const xMargin = 16.0f;
    float const maxWidth = 360.0f;

    // Tick timers and remove expired
    for (auto it = imgui_state.toasts.begin();
         it != imgui_state.toasts.end();) {
      it->timer -= dt;
      if (it->timer <= 0.0f) {
        it = imgui_state.toasts.erase(it);
      } else {
        ++it;
      }
    }

    // Render from bottom of viewport, stacking upward
    ImVec2 const vpPos = ImGui::GetMainViewport()->Pos;
    ImVec2 const vpSize = ImGui::GetMainViewport()->Size;
    for (int i = static_cast<int>(imgui_state.toasts.size()) - 1; i >= 0; --i) {
      auto& t = imgui_state.toasts[i];

      // Fade in/out
      float alpha = 1.0f;
      if (t.timer < ImGuiUIState::TOAST_FADE_TIME)
        alpha = t.timer / ImGuiUIState::TOAST_FADE_TIME;
      float const age = t.initial - t.timer;
      if (age < ImGuiUIState::TOAST_FADE_TIME)
        alpha = std::min(alpha, age / ImGuiUIState::TOAST_FADE_TIME);

      // Colors by level
      ImU32 bgCol, borderCol, textCol;
      switch (t.level) {
        case ImGuiUIState::ToastLevel::Success:
          bgCol = IM_COL32(0x10, 0x30, 0x18, static_cast<int>(210 * alpha));
          borderCol = IM_COL32(0x20, 0x90, 0x40, static_cast<int>(200 * alpha));
          textCol = IM_COL32(0x80, 0xFF, 0x80, static_cast<int>(255 * alpha));
          break;
        case ImGuiUIState::ToastLevel::Error:
          bgCol = IM_COL32(0x30, 0x10, 0x10, static_cast<int>(210 * alpha));
          borderCol = IM_COL32(0x90, 0x20, 0x20, static_cast<int>(200 * alpha));
          textCol = IM_COL32(0xFF, 0x80, 0x80, static_cast<int>(255 * alpha));
          break;
        default:  // Info
          bgCol = IM_COL32(0x18, 0x18, 0x20, static_cast<int>(210 * alpha));
          borderCol = IM_COL32(0x50, 0x50, 0x70, static_cast<int>(200 * alpha));
          textCol = IM_COL32(0xD0, 0xD0, 0xD0, static_cast<int>(255 * alpha));
          break;
      }

      ImVec2 const textSize = ImGui::CalcTextSize(t.message.c_str(), nullptr,
                                                  false, maxWidth - 16.0f);
      float const boxW = textSize.x + 16.0f;
      float const boxH = textSize.y + 12.0f;

      float const x = vpPos.x + vpSize.x - boxW - xMargin;
      float const y = vpPos.y + vpSize.y - yOffset - boxH;

      ImDrawList* dl = ImGui::GetForegroundDrawList();
      ImVec2 const p0(x, y);
      ImVec2 const p1(x + boxW, y + boxH);
      dl->AddRectFilled(p0, p1, bgCol, 4.0f);
      dl->AddRect(p0, p1, borderCol, 4.0f);
      dl->AddText(nullptr, 0.0f, ImVec2(x + 8.0f, y + 6.0f), textCol,
                  t.message.c_str(), nullptr, maxWidth - 16.0f);

      yOffset += boxH + 4.0f;
    }
  }

  // --- Quit confirmation popup (rendered here so it works regardless of
  // show_menu) ---
  if (imgui_state.show_quit_confirm) {
    ImGui::OpenPopup("Confirm Quit");
    imgui_state.show_quit_confirm = false;
  }
  if (ImGui::BeginPopupModal("Confirm Quit", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // P0: warn about unsaved disk changes here, since this GUI quit path passes
    // askIfUnsaved=false to cleanExit() and so skips the native unsaved-disk
    // guard that the OS window-close path enforces (beads-bgs).
    if (driveAltered()) {
      ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f),
                         "You have unsaved changes to a disk.");
      ImGui::TextUnformatted("Quit anyway? Those changes will be lost.");
    } else {
      ImGui::TextUnformatted("Are you sure you want to quit?");
    }
    ImGui::Spacing();
    if (ImGui::Button("Quit", ImVec2(90, 0))) {
      cleanExit(0, false);
    }
    ImGui::SameLine();
    bool const cancel = ImGui::Button("Cancel", ImVec2(90, 0));
    ImGui::SetItemDefaultFocus();  // focus the safe button by default
    if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
      if (!imgui_state.show_menu && !imgui_state.show_options) {
        cpc_resume();
      }
    }
    ImGui::EndPopup();
  }

  // --- New Disk backing-choice modal (Sector vs Flux) ---
  // Backing is chosen at creation because flux you don't keep can't be
  // reconstructed: a Flux disc preserves copy-protection on tracks the CPC
  // never overwrites and can export to real HxC/Gotek hardware.
  if (imgui_state.show_new_disk) {
    ImGui::OpenPopup("New Disk");
    imgui_state.show_new_disk = false;
  }
  if (ImGui::BeginPopupModal("New Disk", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Create a blank disc in drive A:");
    ImGui::Spacing();
    // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
    int backing = imgui_state.new_disk_flux ? 1 : 0;
    ImGui::RadioButton("Sector  (.dsk)", &backing, 0);
    ImGui::TextDisabled(
        "     Small, universal. No weak-bit / copy-protection capability.");
    ImGui::RadioButton("Flux  (.scp)", &backing, 1);
    ImGui::TextDisabled(
        "     Larger. Preserves copy-protection on tracks you don't overwrite;");
    ImGui::TextDisabled(
        "     exports to real HxC/Gotek hardware (.hfe) and .scp.");
    imgui_state.new_disk_flux = backing == 1;
    ImGui::Spacing();
    if (ImGui::Button("Create...", ImVec2(100, 0))) {
      ImGui::CloseCurrentPopup();
      koncpc_request_file_dialog(static_cast<int>(FileDialogAction::NewDiskA));
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  // Reset devtools bar height when hidden so dockspace reclaims the space
  if (!imgui_state.show_devtools && s_devtools_bar_h != 0) {
    s_devtools_bar_h = 0;
    s_topbar_height_dirty = true;
  }

  // Apply deferred topbar/bottombar resize AFTER all ImGui rendering is
  // complete. Calling SDL_SetWindowSize during the render loop causes macOS to
  // shift window coordinates mid-frame, breaking button click detection.
  if (s_topbar_height_dirty) {
    int const total =
        static_cast<int>(s_menubar_h) + s_main_topbar_h + s_devtools_bar_h;
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

  // Keyboard routing is handled at the SDL event level in kon_cpc_ja.cpp
  // using imgui_any_keyboard_ui_active() as the single source of truth.
  // No WantCaptureKeyboard override needed here.
}

// ─────────────────────────────────────────────────
// MRU (recent files) helper
// ─────────────────────────────────────────────────

namespace {
void mru_push(std::vector<std::string>& list, const std::string& path) {
  mru_list_push(list, path, t_CPC::MRU_MAX);
  // Persist immediately. The recent-files list is otherwise only written to
  // disk by the Options▸Save button (saveConfiguration has a single caller),
  // so a load followed by quit/crash would lose the entry. Opening a file is a
  // rare, explicit user action, so a full config write-back here is cheap.
  saveConfiguration(CPC, getConfigurationFilename(true));
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
void imgui_mru_push(std::vector<std::string>& list, const std::string& path) {
  mru_push(list, path);
}

// ─────────────────────────────────────────────────
// Toast notification API
// ─────────────────────────────────────────────────

void imgui_toast(const std::string& message, ImGuiUIState::ToastLevel level) {
  // Cap queue size
  while (static_cast<int>(imgui_state.toasts.size()) >=
         ImGuiUIState::MAX_TOASTS) {
    imgui_state.toasts.pop_front();
  }
  float const duration =
      (level == ImGuiUIState::ToastLevel::Error)
          ? ImGuiUIState::TOAST_DURATION * 1.5f  // errors stay longer
          : ImGuiUIState::TOAST_DURATION;
  imgui_state.toasts.push_back({message, level, duration, duration});
}

void imgui_toast_info(const std::string& message) {
  imgui_toast(message, ImGuiUIState::ToastLevel::Info);
}
void imgui_toast_success(const std::string& message) {
  imgui_toast(message, ImGuiUIState::ToastLevel::Success);
}
void imgui_toast_error(const std::string& message) {
  imgui_toast(message, ImGuiUIState::ToastLevel::Error);
}

// ─────────────────────────────────────────────────
// Keyboard routing: single source of truth
// ─────────────────────────────────────────────────

bool imgui_any_keyboard_ui_active() {
  ImGuiIO const& io = ImGui::GetIO();

  // Modal dialogs and popups always intercept keyboard, even when the CPC
  // screen has focus — otherwise dialogs (quit confirm, options, etc.) are
  // unreachable after the user clicked into the CPC Screen panel.
  if (imgui_state.show_menu || imgui_state.show_options ||
      imgui_state.show_about || imgui_state.show_quit_confirm ||
      imgui_state.show_layout_dropdown || g_command_palette.is_open() ||
      ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))
    return true;

  // A text-input widget is actively focused — must go to ImGui.
  if (io.WantTextInput) return true;

  // The Virtual Joystick, when it is the focused window, maps the host arrows +
  // space onto the CPC joystick (see imgui_render_vjoystick) — so those keys must
  // be kept away from the CPC keyboard. Only while it is focused (and only then).
  if (imgui_state.vjoystick_focused) return true;

  // In docked mode the CPC Screen window tracks focus explicitly.
  // When it's focused (and no modal/text-input is active), keyboard goes to
  // CPC.
  if (imgui_state.cpc_screen_focused) return false;

  // NOTE: show_devtools / g_devtools_ui.any_window_open() intentionally
  // NOT included. Debugger panels being visible does not block keyboard —
  // the CPC must remain interactive while the debugger is observing.
  // Specific text fields in devtools set WantTextInput when focused.
  return false;
}

// ─────────────────────────────────────────────────
// Helper: close menu and resume emulation
// ─────────────────────────────────────────────────

void imgui_open_menu() {
  // The one true "enter pause" path, shared by F1, the topbar Pause button and
  // the native-menu pause: open the pause/session overlay AND pause the
  // emulator (so the topbar swaps the FPS readout for "PAUSED").
  imgui_state.show_menu = true;
  imgui_state.menu_just_opened = true;
  cpc_pause();
}

void imgui_close_menu() {
  imgui_state.show_menu = false;
  // Don't clear show_options/show_about/show_quit_confirm here —
  // they may have just been set by the menu action that triggered
  // imgui_close_menu(). Each dialog is responsible for clearing its own flag on
  // close. Only unpause if no dialog is keeping the emulator paused.
  if (!imgui_state.show_options && !imgui_state.show_quit_confirm) {
    cpc_resume();
  }
}

// tape_scan_blocks() moved to src/tape.cpp (declaration in tape.h) so the
// headless build can call it without linking the modern UI.

// ─────────────────────────────────────────────────
// Main Menu Bar
// ─────────────────────────────────────────────────

// Live toggle state for a toggle-kind action (drives menu checkmarks across the
// ImGui menus and the native macOS menu).  This is the GUI-side half of the
// action registry — menu_actions.cpp holds the labels, the binding map holds
// the shortcuts, and this reads the live emulator/UI state.
bool koncpc_action_is_active(KONCPC_KEYS action) {
  switch (action) {
    case KONCPC_FPS:
      return CPC.scr_fps != 0;
    case KONCPC_SPEED:
      return CPC.limit_speed != 0;
    case KONCPC_JOY:
      return CPC.joystick_emulation != JoystickEmulation::None;
    case KONCPC_PHAZER:
      return static_cast<bool>(CPC.phazer_emulation);
    case KONCPC_DEVTOOLS:
      return imgui_state.show_devtools;
    case KONCPC_DEBUG:
      return log_verbose;
    default:
      return false;
  }
}

// Render one ImGui menu item for an action, pulling its canonical label,
// derived shortcut hint and live checkmark from the single source of truth, and
// routing the click through the one koncpc_menu_action() sink.  Every
// emulator-command menu item should go through here so labels/shortcuts can
// never drift.
namespace {
bool RenderMenuItem(KONCPC_KEYS action, bool enabled = true) {
  const MenuAction* meta = koncpc_find_action(action);
  if (meta == nullptr) return false;
  std::string const sc = koncpc_action_shortcut(action);
  const char* shortcut = sc.empty() ? nullptr : sc.c_str();
  bool const clicked =
      ImGui::MenuItem(meta->title, shortcut,
                      meta->toggle && koncpc_action_is_active(action), enabled);
  if (clicked) koncpc_menu_action(action);
  return clicked;
}
}  // namespace

// Shared debugger step actions, so the DevTools toolbar buttons and the
// keyboard shortcuts invoke identical behavior (beads-fa5).
namespace {
void dbg_step_in() {
  if (subcycle_bridge_active()) {
    cpc_pause();
    z80_step_instruction();
    return;
  }
  z80.step_in = 1;
  z80.step_out = 0;
  z80.step_out_addresses.clear();
  cpc_resume();
}
}  // namespace
namespace {
void dbg_step_over() {
  if (subcycle_bridge_active()) {
    cpc_pause();
    word const pc = z80.PC.w.l;
    if (z80_is_call_or_rst(pc)) {
      z80_add_breakpoint_ephemeral(
          static_cast<word>(pc + z80_instruction_length(pc)));
      cpc_resume();
      return;
    }
    z80_step_instruction();
    return;
  }
  z80.step_in = 0;
  z80.step_out = 0;
  z80.step_out_addresses.clear();
  word const pc = z80.PC.w.l;
  if (z80_is_call_or_rst(pc)) {
    z80_add_breakpoint_ephemeral(pc + z80_instruction_length(pc));
    cpc_resume();
  } else {
    z80.step_in = 1;
    cpc_resume();
  }
}
}  // namespace
namespace {
void dbg_step_out() {
  z80.step_out = 1;
  z80.step_out_addresses.clear();
  z80.step_in = 0;
  cpc_resume();
}
}  // namespace

// Apply a window-scale choice (0 = Fit window, 1..4 = 1x/1.5x/2x/3x): set
// CPC.scr_scale and resize the SDL window to match.  Shared by the Settings
// Video tab's Scale combo and the View > Scale menu so the two can't drift.
namespace {
void apply_scr_scale(int scale_idx) {
  if (scale_idx < 0 || scale_idx > 4) scale_idx = 0;
  CPC.scr_scale = scale_idx;
  if (scale_idx > 0 && mainSDLWindow) {
    static const float sf[] = {0.f, 1.f, 1.5f, 2.f, 3.f};
    float const f = sf[scale_idx];
    int const new_w = static_cast<int>(CPC_RENDER_WIDTH * f);
    int new_h = CPC.scr_crt_aspect
                    ? static_cast<int>(new_w * 3.f / 4.f)
                    : static_cast<int>(CPC_VISIBLE_SCR_HEIGHT * f);
    new_h += video_get_topbar_height() + video_get_bottombar_height();
    SDL_SetWindowSize(mainSDLWindow, new_w, new_h);
  }
}
}  // namespace

// ─────────────────────────────────────────────────
// Shared menu bridge (menu_bridge.h) — single source for the NON-KONCPC menu
// items consumed by BOTH the in-window ImGui bar and the native macOS bar.
// ─────────────────────────────────────────────────

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
const std::vector<SettingsTabItem>& koncpc_settings_tab_items() {
  // Order + separators mirror the Machine menu in the taxonomy.
  static std::vector<SettingsTabItem> const items = {
      {"System...", static_cast<int>(OptionsTab::General), false},
      {"ROMs...", static_cast<int>(OptionsTab::ROMs), false},
      {"Video...", static_cast<int>(OptionsTab::Video), false},
      {"Audio...", static_cast<int>(OptionsTab::Audio), false},
      {"Input Mapping...", static_cast<int>(OptionsTab::Input), false},
      {"M4 Board...", static_cast<int>(OptionsTab::M4), true},
      {"Serial Interface...", static_cast<int>(OptionsTab::Serial), false},
  };
  return items;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
const std::vector<WindowMenuItem>& koncpc_window_menu_items() {
  // 3 specials + the 17 devtools windows, grouped exactly like the in-window
  // Window menu (separator_before reproduces the section breaks).
  static std::vector<WindowMenuItem> const items = {
      {"Virtual Keyboard", "$vkbd", false},
      {"Virtual Joystick", "$vjoy", false},
      {"Serial Terminal", "$serial", false},
      {"Plotter Preview", "$plotter", false},
      // Debug Windows
      {"Registers", "registers", true},
      {"Disassembly", "disassembly", false},
      {"Memory Hex", "memory_hex", false},
      {"Stack", "stack", false},
      {"Breakpoints", "breakpoints", false},
      {"Symbols", "symbols", false},
      {"Assembler", "assembler", false},
      // Hardware
      {"Video State", "video_state", true},
      {"Audio State", "audio_state", false},
      {"Drive Sound Lab", "drive_sound_lab", false},
      {"ASIC Registers", "asic", false},
      {"Silicon Disc", "silicon_disc", false},
      {"Disc Tools", "disc_tools", false},
      // Analysis
      {"GFX Finder", "gfx_finder", true},
      {"Data Areas", "data_areas", false},
      {"Disasm Export", "disasm_export", false},
      // Recording
      {"Session Recording", "session_recording", true},
      {"Recording Controls", "recording_controls", false},
  };
  return items;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
const std::vector<const char*>& koncpc_scale_labels() {
  static std::vector<const char*> const labels = {"Fit window", "1x", "1.5x",
                                                  "2x", "3x"};
  return labels;
}

extern "C" void koncpc_show_about_dialog() { imgui_state.show_about = true; }

extern "C" void koncpc_open_settings_tab(int tab) {
  imgui_state.show_options = true;
  s_pending_options_tab = static_cast<OptionsTab>(tab);
}

extern "C" void koncpc_open_command_palette() { g_command_palette.open(); }

extern "C" void koncpc_request_file_dialog(int action) {
  // Single source for the per-action filters + default paths.  Same callback
  // (which restores keyboard focus) used by every loader/saver.
  auto fda = static_cast<FileDialogAction>(action);
  // NOLINTNEXTLINE(performance-no-int-to-ptr): intentional integer/pointer reinterpret for hardware/opaque handles
  auto ud = reinterpret_cast<void*>(static_cast<intptr_t>(action));
  switch (fda) {
    case FileDialogAction::LoadDiskA: {
      static const SDL_DialogFileFilter f[] = {
          {"Disk Images", "dsk;ipf;raw;zip"}};
      SDL_ShowOpenFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_dsk_path.c_str(), false);
      break;
    }
    case FileDialogAction::LoadDiskB: {
      static const SDL_DialogFileFilter f[] = {
          {"Disk Images", "dsk;ipf;raw;zip"}};
      SDL_ShowOpenFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_dsk_path.c_str(), false);
      break;
    }
    case FileDialogAction::SaveDiskA: {
      static const SDL_DialogFileFilter f[] = {{"Disk Images", "dsk"}};
      SDL_ShowSaveFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_dsk_path.c_str());
      break;
    }
    case FileDialogAction::SaveDiskB: {
      static const SDL_DialogFileFilter f[] = {{"Disk Images", "dsk"}};
      SDL_ShowSaveFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_dsk_path.c_str());
      break;
    }
    case FileDialogAction::SaveDiskA_SCP: {
      static const SDL_DialogFileFilter f[] = {{"SuperCard Pro flux", "scp"}};
      SDL_ShowSaveFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_dsk_path.c_str());
      break;
    }
    case FileDialogAction::SaveDiskA_HFE: {
      static const SDL_DialogFileFilter f[] = {{"HxC / Gotek flux", "hfe"}};
      SDL_ShowSaveFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_dsk_path.c_str());
      break;
    }
    case FileDialogAction::NewDiskA: {
      // Filter (and so the default extension) tracks the chosen backing: a
      // Flux disc is written as .scp, a Sector disc as .dsk.
      static const SDL_DialogFileFilter dsk_f[] = {{"Disk Images", "dsk"}};
      static const SDL_DialogFileFilter scp_f[] = {
          {"SuperCard Pro flux", "scp"}};
      const SDL_DialogFileFilter* f = imgui_state.new_disk_flux ? scp_f : dsk_f;
      SDL_ShowSaveFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_dsk_path.c_str());
      break;
    }
    case FileDialogAction::LoadTape: {
      static const SDL_DialogFileFilter f[] = {{"Tape Images", "cdt;voc;zip"}};
      SDL_ShowOpenFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_tape_path.c_str(), false);
      break;
    }
    case FileDialogAction::LoadCartridge: {
      static const SDL_DialogFileFilter f[] = {{"Cartridges", "cpr;zip"}};
      SDL_ShowOpenFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_cart_path.c_str(), false);
      break;
    }
    case FileDialogAction::LoadSnapshot: {
      static const SDL_DialogFileFilter f[] = {{"Snapshots", "sna;zip"}};
      SDL_ShowOpenFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_snap_path.c_str(), false);
      break;
    }
    case FileDialogAction::SaveSnapshot: {
      static const SDL_DialogFileFilter f[] = {{"Snapshots", "sna"}};
      SDL_ShowSaveFileDialog(file_dialog_callback, ud, mainSDLWindow, f, 1,
                             CPC.current_snap_path.c_str());
      break;
    }
    default:
      break;
  }
}

extern "C" void koncpc_window_toggle(const char* key) {
  if (key == nullptr) return;
  if (strcmp(key, "$vkbd") == 0) {
    imgui_state.show_vkeyboard = !imgui_state.show_vkeyboard;
  } else if (strcmp(key, "$vjoy") == 0) {
    imgui_state.show_vjoystick = !imgui_state.show_vjoystick;
  } else if (strcmp(key, "$serial") == 0) {
    imgui_state.show_serial_terminal = !imgui_state.show_serial_terminal;
  } else if (strcmp(key, "$plotter") == 0) {
    imgui_state.show_plotter_preview = !imgui_state.show_plotter_preview;
  } else {
    g_devtools_ui.toggle_window(key);
  }
}

extern "C" bool koncpc_window_is_open(const char* key) {
  if (key == nullptr) return false;
  if (strcmp(key, "$vkbd") == 0) return imgui_state.show_vkeyboard;
  if (strcmp(key, "$vjoy") == 0) return imgui_state.show_vjoystick;
  if (strcmp(key, "$serial") == 0) return imgui_state.show_serial_terminal;
  if (strcmp(key, "$plotter") == 0) return imgui_state.show_plotter_preview;
  bool const* p = g_devtools_ui.window_ptr(key);
  return p != nullptr && *p;
}

extern "C" void koncpc_set_scale(int idx) { apply_scr_scale(idx); }
extern "C" int koncpc_current_scale() { return CPC.scr_scale; }

extern "C" void koncpc_set_renderer(int plugin_idx) {
  CPC.scr_style = plugin_idx;
  imgui_state.video_reinit_pending = true;
}
extern "C" int koncpc_current_renderer() {
  return static_cast<int>(CPC.scr_style);
}

extern "C" int koncpc_renderer_count() {
  return static_cast<int>(video_plugin_list.size());
}
extern "C" const char* koncpc_renderer_name(int i) {
  if (i < 0 || i >= static_cast<int>(video_plugin_list.size())) return "";
  return video_plugin_list[i].name;
}
extern "C" bool koncpc_renderer_hidden(int i) {
  if (i < 0 || i >= static_cast<int>(video_plugin_list.size())) return true;
  return video_plugin_list[i].hidden;
}
extern "C" const char* koncpc_renderer_group(int i) {
  // Single source for the GPU/CPU grouping used by both menu bars.
  if (i < 0 || i >= static_cast<int>(video_plugin_list.size())) return "";
  const char* name = video_plugin_list[i].name;
  if (strncmp(name, "CRT", 3) == 0) return "GPU — CRT Shaders";
  if (strcmp(name, "Direct") == 0 || strcmp(name, "Direct (SDL)") == 0 ||
      strcmp(name, "OpenGL scaling") == 0)
    return "GPU — Direct";
  return "CPU — Software Scalers";
}

namespace {
void imgui_render_menubar() {
  if (!ImGui::BeginMainMenuBar()) return;

  float const h = ImGui::GetWindowSize().y;
  if (h != s_menubar_h) {
    s_menubar_h = h;
    s_topbar_height_dirty = true;
  }

  // ── konCePCja ──
  if (ImGui::BeginMenu("konCePCja")) {
    if (ImGui::MenuItem("About konCePCja")) {
      koncpc_show_about_dialog();
    }
    if (ImGui::MenuItem("Settings...")) {
      koncpc_open_settings_tab(static_cast<int>(OptionsTab::General));
    }
    ImGui::Separator();
    RenderMenuItem(KONCPC_EXIT);
    ImGui::EndMenu();
  }

  // ── Machine ── deep-links straight to a Settings tab + Reset.  Each item
  // opens the Options window and asks it to select that tab for one frame.
  // The tab list is single-sourced via koncpc_settings_tab_items().
  if (ImGui::BeginMenu("Machine")) {
    for (const SettingsTabItem& it : koncpc_settings_tab_items()) {
      if (it.separator_before) ImGui::Separator();
      if (ImGui::MenuItem(it.label)) {
        koncpc_open_settings_tab(it.tab);
      }
    }
    ImGui::Separator();
    if (subcycle_bridge_active() && ImGui::BeginMenu("Run Tier")) {
      ImGui::TextDisabled(
          "All tiers give identical results — they differ in\n"
          "simulation granularity (speed vs. chip-level visibility).");
      ImGui::Separator();
      const bool pinned = subcycle_bridge_tier_env_pinned() != 0;
      const BridgeTierPolicy pol = subcycle_bridge_tier_policy();
      auto tier_item = [&](const char* label, BridgeTierPolicy p,
                           const char* tip) {
        if (ImGui::MenuItem(label, nullptr, pol == p, !pinned))
          subcycle_bridge_set_tier_policy(p);
        ImGui::SetItemTooltip("%s", tip);
      };
      tier_item("Automatic — fastest, drops in for the debugger (recommended)",
                BridgeTierPolicy::Auto,
                "Runs Performance, switching to Balanced for as long as any\n"
                "breakpoint or watchpoint is set — the debugger steps at\n"
                "cycle level. Switches back when the last one clears.");
      tier_item("Performance — instruction-stepped (fast)",
                BridgeTierPolicy::Fast,
                "Instruction-granularity catch-up scheduling over the same\n"
                "chips — the fastest tier; byte-identical at every frame.");
      tier_item("Balanced — cycle-stepped, event-driven",
                BridgeTierPolicy::Wake,
                "Every chip stepped per clock cycle, skipping cycles where a\n"
                "chip provably sleeps. The debugger's tier.");
      tier_item("Microscope", BridgeTierPolicy::Soldered,
                "Every chip stepped on every master cycle of the fixed board\n"
                "— full pin-level bus truth for scopes and pin watches.");
      tier_item("Microscope, socketed chips", BridgeTierPolicy::Faithful,
                "The same per-master-cycle stepping, but through pluggable\n"
                "chip sockets — slowest; accepts any board composition.");
      ImGui::Separator();
      if (ImGui::MenuItem("Speed Test  (~5 s)")) {
        subcycle_bridge_bench_request();
        g_speedtest_open = true;
      }
      ImGui::SetItemTooltip(
          "Measures each tier's top speed on the LIVE machine state\n"
          "(snapshot-restored around every sample — fully disposable;\n"
          "the emulation keeps running underneath).");
      ImGui::Separator();
      ImGui::TextDisabled("Running: %s%s",
                          subcycle_bridge_effective_tier_label(),
                          pinned ? " (pinned by env)" : "");
      ImGui::EndMenu();
    }
    RenderMenuItem(KONCPC_RESET);
    ImGui::EndMenu();
  }

  if (g_speedtest_open) {
    ImGui::OpenPopup("Speed Test");
    g_speedtest_open = false;
  }
  if (ImGui::BeginPopupModal("Speed Test", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    static const char* const kTierNames[4] = {
        "Performance", "Balanced", "Microscope", "Microscope, socketed chips"};
    const int running = subcycle_bridge_bench_running();
    if (ImGui::BeginTable("speedtest", 3)) {
      for (int t = 0; t < 4; ++t) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kTierNames[t]);
        const int fps = subcycle_bridge_bench_fps(t);
        ImGui::TableNextColumn();
        if (fps >= 0)
          ImGui::Text("up to ~%d FPS", fps);
        else if (t == running)
          ImGui::TextUnformatted("testing...");
        else
          ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (fps >= 0) ImGui::Text("%.0fx realtime", fps / 50.0);
      }
      ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextDisabled(running >= 0
                            ? "Measuring - the emulation keeps running..."
                            : "Done. Measured on live machine state.");
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // ── Edit ──
  if (ImGui::BeginMenu("Edit")) {
    RenderMenuItem(KONCPC_PASTE);
    ImGui::EndMenu();
  }

  // ── Media ── (loaders/savers single-sourced via koncpc_request_file_dialog)
  if (ImGui::BeginMenu("Media")) {
    if (ImGui::MenuItem("Load Disk A...")) {
      koncpc_request_file_dialog(static_cast<int>(FileDialogAction::LoadDiskA));
    }
    if (ImGui::MenuItem("Load Disk B...")) {
      koncpc_request_file_dialog(static_cast<int>(FileDialogAction::LoadDiskB));
    }
    if (ImGui::MenuItem("New Disk...")) {
      imgui_state.show_new_disk = true;
    }
    // Save-As: the format picker's options track the drive's live backing.
    // Under engine=1 the caps come from the sub-cycle FDC medium; on a legacy
    // engine=0 run they fall back to the driveA/driveB struct (sector-only).
    const bool engine_live = subcycle_bridge_active();
    const FluxSaveCaps caps_a = engine_live ? flux_save_caps(0) : FluxSaveCaps{};
    const FluxSaveCaps caps_b = engine_live ? flux_save_caps(1) : FluxSaveCaps{};
    const bool a_dsk = engine_live ? caps_a.can_dsk : (driveA.tracks != 0);
    const bool b_dsk = engine_live ? caps_b.can_dsk : (driveB.tracks != 0);
    if (ImGui::BeginMenu("Save Disk A", a_dsk || caps_a.can_scp)) {
      if (ImGui::MenuItem("As .dsk...", nullptr, false, a_dsk)) {
        koncpc_request_file_dialog(static_cast<int>(FileDialogAction::SaveDiskA));
      }
      if (ImGui::MenuItem("As .scp (flux)...", nullptr, false, caps_a.can_scp)) {
        koncpc_request_file_dialog(
            static_cast<int>(FileDialogAction::SaveDiskA_SCP));
      }
      if (ImGui::MenuItem("As .hfe (flux)...", nullptr, false, caps_a.can_hfe)) {
        koncpc_request_file_dialog(
            static_cast<int>(FileDialogAction::SaveDiskA_HFE));
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Save Disk B...", nullptr, false, b_dsk)) {
      koncpc_request_file_dialog(static_cast<int>(FileDialogAction::SaveDiskB));
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Load Tape...")) {
      koncpc_request_file_dialog(static_cast<int>(FileDialogAction::LoadTape));
    }
    RenderMenuItem(KONCPC_TAPEPLAY, !pbTapeImage.empty());
    if (ImGui::MenuItem("Eject Tape", nullptr, false, !pbTapeImage.empty())) {
      tape_eject();
      CPC.tape.file.clear();
      imgui_state.tape_block_offsets.clear();
      imgui_state.tape_current_block = 0;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Load Cartridge...")) {
      koncpc_request_file_dialog(
          static_cast<int>(FileDialogAction::LoadCartridge));
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Load Snapshot...")) {
      koncpc_request_file_dialog(
          static_cast<int>(FileDialogAction::LoadSnapshot));
    }
    if (ImGui::MenuItem("Save Snapshot...")) {
      koncpc_request_file_dialog(
          static_cast<int>(FileDialogAction::SaveSnapshot));
    }
    // beads-5aa: group the default-slot quick snapshot actions so their labels
    // (which read like speed adjectives in isolation) are clearly about the
    // default slot. Canonical labels live in menu_actions.cpp (not edited).
    RenderMenuItem(KONCPC_SNAPSHOT);
    RenderMenuItem(KONCPC_LD_SNAP);
    ImGui::Separator();
    RenderMenuItem(KONCPC_NEXTDISKA);

    // ── Open Recent submenu ──
    bool const has_any_mru = !CPC.mru_disks.empty() || !CPC.mru_tapes.empty() ||
                             !CPC.mru_snaps.empty() || !CPC.mru_carts.empty();
    ImGui::Separator();
    if (ImGui::BeginMenu("Open Recent", has_any_mru)) {
      auto render_mru_section =
          [&](const char* label, std::vector<std::string>& list, auto load_fn) {
            if (!list.empty() && ImGui::BeginMenu(label)) {
              for (int i = 0; i < static_cast<int>(list.size()); i++) {
                auto item_fname =
                    std::filesystem::path(list[i]).filename().string();
                ImGui::PushID(i);
                if (ImGui::MenuItem(item_fname.c_str())) {
                  load_fn(list[i]);
                }
                if (ImGui::IsItemHovered())
                  ImGui::SetTooltip("%s", list[i].c_str());
                ImGui::PopID();
              }
              ImGui::EndMenu();
            }
          };
      render_mru_section("Disks", CPC.mru_disks, [](const std::string& p) {
        CPC.driveA.file = p;
        auto f = std::filesystem::path(p).filename().string();
        if (file_load(CPC.driveA) == 0) {
          imgui_toast_success("Drive A: " + f);
          mru_push(CPC.mru_disks, p);
        } else {
          imgui_toast_error("Failed: " + f);
        }
      });
      render_mru_section("Tapes", CPC.mru_tapes, [](const std::string& p) {
        CPC.tape.file = p;
        auto f = std::filesystem::path(p).filename().string();
        if (file_load(CPC.tape) == 0) {
          imgui_toast_success("Tape: " + f);
          tape_scan_blocks();
          mru_push(CPC.mru_tapes, p);
        } else {
          imgui_toast_error("Failed: " + f);
        }
      });
      render_mru_section("Snapshots", CPC.mru_snaps, [](const std::string& p) {
        CPC.snapshot.file = p;
        auto f = std::filesystem::path(p).filename().string();
        if (file_load(CPC.snapshot) == 0) {
          imgui_toast_success("Snapshot: " + f);
          mru_push(CPC.mru_snaps, p);
        } else {
          imgui_toast_error("Failed: " + f);
        }
      });
      render_mru_section("Cartridges", CPC.mru_carts, [](const std::string& p) {
        CPC.cartridge.file = p;
        auto f = std::filesystem::path(p).filename().string();
        if (file_load(CPC.cartridge) == 0) {
          imgui_toast_success("Cartridge: " + f);
          emulator_reset();
          mru_push(CPC.mru_carts, p);
        } else {
          imgui_toast_error("Failed: " + f);
        }
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

  // ── View ──
  if (ImGui::BeginMenu("View")) {
    RenderMenuItem(KONCPC_FULLSCRN);

    // Scale ▸ — window scale factor (mirrors Settings ▸ Video ▸ Scale, same
    // apply path via the bridge).  Checkmark on the current factor.
    if (ImGui::BeginMenu("Scale")) {
      const auto& labels = koncpc_scale_labels();
      for (int i = 0; i < static_cast<int>(labels.size()); i++) {
        if (ImGui::MenuItem(labels[i], nullptr, koncpc_current_scale() == i)) {
          koncpc_set_scale(i);
        }
      }
      ImGui::EndMenu();
    }

    // Renderer ▸ — video plugin (mirrors Settings ▸ Video ▸ Video Plugin),
    // grouped GPU/CPU via the bridge so the two stay in sync.
    if (ImGui::BeginMenu("Renderer")) {
      const char* prev_group = nullptr;
      for (int i = 0; i < koncpc_renderer_count(); i++) {
        if (koncpc_renderer_hidden(i)) continue;
        const char* group = koncpc_renderer_group(i);
        if (!prev_group || strcmp(prev_group, group) != 0) {
          ImGui::SeparatorText(group);
          prev_group = group;
        }
        if (ImGui::MenuItem(koncpc_renderer_name(i), nullptr,
                            koncpc_current_renderer() == i)) {
          koncpc_set_renderer(i);
        }
      }
      ImGui::EndMenu();
    }

    ImGui::Separator();
    RenderMenuItem(KONCPC_SCRNSHOT);
    RenderMenuItem(KONCPC_FPS);
    ImGui::EndMenu();
  }

  // ── Input ──
  if (ImGui::BeginMenu("Input")) {
    RenderMenuItem(KONCPC_JOY);
    RenderMenuItem(KONCPC_PHAZER);
    RenderMenuItem(KONCPC_SPEED);
    ImGui::EndMenu();
  }

  // ── Tools ──
  if (ImGui::BeginMenu("Tools")) {
    RenderMenuItem(KONCPC_DEVTOOLS);
    // beads-qnf: surface the Cmd+K command palette (previously only mentioned
    // in the About box) as a discoverable menu entry.
    if (ImGui::MenuItem("Command Palette", "Cmd+K")) {
      koncpc_open_command_palette();
    }
    RenderMenuItem(KONCPC_MF2STOP);
    // beads-41p: developer/diagnostics group — Verbose Logging moved here from
    // the Options menu (where it sat among player toggles).
    ImGui::Separator();
    if (ImGui::BeginMenu("Diagnostics")) {
      RenderMenuItem(KONCPC_DEBUG);
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  // ── Window ── single-sourced via koncpc_window_menu_items().  The section
  // labels under each separator are added here for the in-window bar's denser
  // layout (the native bar shows the separators alone).
  if (ImGui::BeginMenu("Window")) {
    static const char* const kSectionLabel[] = {
        "Debug Windows", "Hardware", "Analysis", "Recording"};
    // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
    int section = 0;
    // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
    bool first = true;
    for (const WindowMenuItem& it : koncpc_window_menu_items()) {
      if (it.separator_before) {
        ImGui::Separator();
        if (!first && section < IM_ARRAYSIZE(kSectionLabel)) {
          ImGui::TextDisabled("%s", kSectionLabel[section]);
          section++;
        }
      }
      first = false;
      // Virtual Keyboard keeps its discoverability hint; specials have no live
      // toggle pointer, so route clicks through the bridge toggle.
      const char* shortcut = (strcmp(it.key, "$vkbd") == 0)   ? "Shift+F1"
                             // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): compact selection expression kept intentionally
                             : (strcmp(it.key, "$vjoy") == 0) ? "Shift+F6"
                                                              : nullptr;
      if (ImGui::MenuItem(it.label, shortcut, koncpc_window_is_open(it.key))) {
        koncpc_window_toggle(it.key);
      }
    }
    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
}
}  // namespace

// ─────────────────────────────────────────────────
// Top Bar
// ─────────────────────────────────────────────────

int imgui_topbar_height() {
  return static_cast<int>(s_menubar_h) + s_main_topbar_h + s_devtools_bar_h;
}

namespace {
void render_layout_dropdown() {
  if (!imgui_state.show_layout_dropdown) return;

  // Position below the Layout button, clamped to stay within the main viewport
  {
    ImGuiViewport const* mvp = ImGui::GetMainViewport();
    float const dd_w = 220.0f;
    float x = s_layout_btn_pos.x;
    float const right_edge = mvp->Pos.x + mvp->Size.x;
    if (x + dd_w > right_edge) x = right_edge - dd_w;
    ImGui::SetNextWindowPos(ImVec2(x, s_layout_btn_pos.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(dd_w, 0));
    ImGui::SetNextWindowViewport(mvp->ID);
  }

  ImGuiWindowFlags const dd_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
  if (ImGui::Begin("##LayoutDropdown", nullptr, dd_flags)) {
    // Close when clicking outside
    if (!ImGui::IsWindowHovered(
            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseClicked(0)) {
      imgui_state.show_layout_dropdown = false;
    }

    // Mode selection
    if (ImGui::RadioButton(
            "Classic Mode",
            CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Classic)) {
      CPC.workspace_layout = t_CPC::WorkspaceLayoutMode::Classic;
      imgui_state.show_layout_dropdown = false;
    }
    if (ImGui::RadioButton(
            "Docked Mode",
            CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Docked)) {
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
    }

    // Custom saved layouts
    ImGui::Separator();
    {
      static bool open_save_popup = false;
      if (ImGui::MenuItem("Save Layout...")) open_save_popup = true;

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
            if (ImGui::MenuItem(l.c_str())) workspace_delete_layout(l);
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
      if (ImGui::RadioButton("Fit",
                             CPC.cpc_screen_scale == t_CPC::ScreenScale::Fit))
        CPC.cpc_screen_scale = t_CPC::ScreenScale::Fit;
      if (ImGui::RadioButton("1x",
                             CPC.cpc_screen_scale == t_CPC::ScreenScale::X1))
        CPC.cpc_screen_scale = t_CPC::ScreenScale::X1;
      if (ImGui::RadioButton("2x",
                             CPC.cpc_screen_scale == t_CPC::ScreenScale::X2))
        CPC.cpc_screen_scale = t_CPC::ScreenScale::X2;
      if (ImGui::RadioButton("3x",
                             CPC.cpc_screen_scale == t_CPC::ScreenScale::X3))
        CPC.cpc_screen_scale = t_CPC::ScreenScale::X3;
    }

    // Save Layout popup (modal, so it won't have focus issues)
    {
      static char save_name[64] = "";
      static std::string save_error;
      if (ImGui::BeginPopup("Save Layout##popup")) {
        ImGui::TextUnformatted("Layout Name:");
        bool const enter_pressed =
            ImGui::InputText("##save_name", save_name, sizeof(save_name),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

        if (!save_error.empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
          ImGui::TextUnformatted(save_error.c_str());
          ImGui::PopStyleColor();
        }

        bool const do_save = enter_pressed || ImGui::Button("Save");
        ImGui::SameLine();
        bool const do_cancel = ImGui::Button("Cancel");

        if (do_save) {
          std::string name(save_name);
          while (!name.empty() && name.front() == ' ') name.erase(name.begin());
          while (!name.empty() && name.back() == ' ') name.pop_back();

          bool valid = !name.empty();
          if (valid) {
            for (char const c : name) {
              if (c == '/' || c == '\\' || c == '\0') {
                valid = false;
                break;
              }
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
}  // namespace

namespace {
void imgui_render_topbar() {
  float const pad_y = 2.0f;
  float const bar_height =
      25.0f;  // topbar window only (not including menu bar)

  ImGuiViewport const* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + s_menubar_h));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, bar_height));
  ImGui::SetNextWindowViewport(vp->ID);  // keep on main viewport
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, pad_y));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg,
                        ImVec4(0.094f, 0.094f, 0.094f, 1.0f));

  ImGuiWindowFlags const flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav;

  if (ImGui::Begin("##topbar", nullptr, flags)) {
    {
      int const h = static_cast<int>(ImGui::GetWindowSize().y);
      if (h != s_main_topbar_h) {
        s_main_topbar_h = h;
        s_topbar_height_dirty = true;
      }
    }
    // Label reflects live pause state (atomic flag — CPC.paused is written by
    // the Z80 thread). When paused, the button resumes; when running, it opens
    // the F1 menu and pauses, exactly as before.
    const bool is_paused = g_emu_paused.load(std::memory_order_relaxed);
    // beads-uvo: reserve the gold/amber accent for the PAUSED state. While
    // running, the Pause button gets a flat neutral fill so the gold only
    // means "something is halted"; when paused, the Resume button keeps the
    // theme's gold so the call-to-action stands out.
    if (!is_paused) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.23f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.28f, 0.28f, 0.32f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                            ImVec4(0.34f, 0.34f, 0.38f, 1.0f));
    }
    if (ImGui::Button(is_paused ? "Resume (Esc)" : "Pause (F1)")) {
      if (is_paused) {
        cpc_resume();
        imgui_state.show_menu = false;
      } else {
        imgui_open_menu();
      }
    }
    if (!is_paused) ImGui::PopStyleColor(3);
    // (Tape waveform moved to bottom status bar)

    // beads-rwc: reformat the run-on FPS string ("50FPS 100%") produced by the
    // emulator core into "50 FPS · 100%" (space + middle-dot separator) for
    // legibility. Done here at the display site because the producer lives in
    // another translation unit.
    std::string fps_display;
    if (!imgui_state.topbar_fps.empty()) {
      const std::string& raw = imgui_state.topbar_fps;
      size_t const fpos = raw.find("FPS");
      if (fpos != std::string::npos) {
        // Left part = digits before "FPS"; right part = remainder after "FPS".
        std::string left = raw.substr(0, fpos);
        std::string right = raw.substr(fpos + 3);
        // Trim surrounding spaces from both halves.
        auto trim = [](std::string& s) {
          size_t const a = s.find_first_not_of(' ');
          size_t const b = s.find_last_not_of(' ');
          s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
        };
        trim(left);
        trim(right);
        fps_display = left + " FPS \xc2\xb7 " + right;  // U+00B7 middle dot
      } else {
        fps_display = raw;
      }
    }

    // ── Right status cluster: Layout button + (PAUSED | FPS) ──
    // Uses a state-flag + standalone window instead of ImGui popup, because
    // popups from the fixed topbar close immediately in docked mode due to
    // focus interactions with the dockspace.
    {
      // Right-align before rightmost element (PAUSED or FPS counter)
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      float right_w = 0.0f;
      if (is_paused) {
        right_w = ImGui::CalcTextSize("PAUSED").x + 16.0f;
      } else if (!fps_display.empty()) {
        right_w = ImGui::CalcTextSize(fps_display.c_str()).x + 16.0f;
      }
      float const btn_w = ImGui::CalcTextSize("Layout").x +
                          (ImGui::GetStyle().FramePadding.x * 2.0f);
      ImGui::SameLine(ImGui::GetWindowWidth() - right_w - btn_w - 12.0f);

      if (ImGui::Button("Layout")) {
        imgui_state.show_layout_dropdown = !imgui_state.show_layout_dropdown;
      }
      // Remember button position for dropdown window placement
      s_layout_btn_pos = ImGui::GetItemRectMin();
      s_layout_btn_pos.y = ImGui::GetItemRectMax().y + 2.0f;
    }

    if (is_paused) {
      float const pause_width = ImGui::CalcTextSize("PAUSED").x;
      ImGui::SameLine(ImGui::GetWindowWidth() - pause_width - 8);
      ImGui::AlignTextToFramePadding();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
      ImGui::TextUnformatted("PAUSED");
      ImGui::PopStyleColor();
    } else if (!fps_display.empty()) {
      float const fps_width = ImGui::CalcTextSize(fps_display.c_str()).x;
      ImGui::SameLine(ImGui::GetWindowWidth() - fps_width - 8);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(fps_display.c_str());
    }

    // beads-sxd: subtle bottom separator so the native title bar, menu bar, and
    // topbar don't merge into one undifferentiated dark band. Drawn 1px lighter
    // than the topbar background at the bottom edge of the window.
    {
      ImVec2 const wp = ImGui::GetWindowPos();
      ImVec2 const ws = ImGui::GetWindowSize();
      float const yb = wp.y + ws.y - 0.5f;
      ImGui::GetWindowDrawList()->AddLine(
          ImVec2(wp.x, yb), ImVec2(wp.x + ws.x, yb),
          IM_COL32(0x2A, 0x2A, 0x2E, 0xFF), 1.0f);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);

  // ── Layout dropdown window (rendered outside topbar) ──
  render_layout_dropdown();
}
}  // namespace

// ─────────────────────────────────────────────────
// Marquee text helper: scrolls text horizontally within a fixed-width box
// when text is wider than boxW. Uses ping-pong with pause at start/end.
// ─────────────────────────────────────────────────
namespace {
void imgui_marquee_text(const char* text, float boxW) {
  // AlignTextToFramePadding is called by the caller — capture position AFTER
  // alignment.
  ImVec2 const pos = ImGui::GetCursorScreenPos();
  float const frameH = ImGui::GetFrameHeight();
  float const lineH = ImGui::GetTextLineHeight();
  float const textY =
      pos.y + ((frameH - lineH) * 0.5f);  // vertically center text in frame
  float const textW = ImGui::CalcTextSize(text).x;
  ImU32 const color = ImGui::GetColorU32(ImGuiCol_Text);

  if (textW <= boxW) {
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, textY), color, text);
    ImGui::Dummy(ImVec2(boxW, frameH));
    return;
  }

  // Ping-pong scroll with 20px pause at each end
  float const overflow = textW - boxW;
  float const range = overflow + 40.0f;
  float const t =
      fmodf(static_cast<float>(ImGui::GetTime()) * 30.0f, range * 2.0f);
  float scroll = t < range ? t : (range * 2.0f) - t;
  scroll = fmaxf(0.0f, scroll - 20.0f);

  ImGui::PushClipRect(pos, ImVec2(pos.x + boxW, pos.y + frameH), true);
  ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x - scroll, textY), color,
                                      text);
  ImGui::PopClipRect();

  ImGui::Dummy(ImVec2(boxW, frameH));
}
}  // namespace

// Draw a beveled LED indicator (16x8 px) with active/inactive state.
// r,g,b are the base color channel values (e.g. 255,0,0 for red).
namespace {
void draw_status_led(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool active,
                            int r, int g, int b) {
  if (active) {
    dl->AddRectFilled(p0, p1, IM_COL32(r, g, b, 255));
    ImU32 const hi = IM_COL32(std::min(255, r + 100), std::min(255, g + 100),
                              std::min(255, b + 100), 255);
    ImU32 const lo = IM_COL32(r * 63 / 100, g * 63 / 100, b * 63 / 100, 255);
    dl->AddLine(p0, ImVec2(p1.x, p0.y), hi);
    dl->AddLine(p0, ImVec2(p0.x, p1.y), hi);
    dl->AddLine(ImVec2(p0.x, p1.y), p1, lo);
    dl->AddLine(ImVec2(p1.x, p0.y), p1, lo);
  } else {
    dl->AddRectFilled(p0, p1, IM_COL32(r / 3, g / 3, b / 3, 255));
    ImU32 const hi = IM_COL32((r / 3) + 30, (g / 3) + 30, (b / 3) + 30, 255);
    ImU32 const lo = IM_COL32(r / 6, g / 6, b / 6, 255);
    dl->AddLine(p0, ImVec2(p1.x, p0.y), hi);
    dl->AddLine(p0, ImVec2(p0.x, p1.y), hi);
    dl->AddLine(ImVec2(p0.x, p1.y), p1, lo);
    dl->AddLine(ImVec2(p1.x, p0.y), p1, lo);
  }
}
}  // namespace

// ─────────────────────────────────────────────────
// Bottom Status Bar
// ─────────────────────────────────────────────────

namespace {
void imgui_render_statusbar() {
  float const bar_height = 22.0f;
  float const pad_y = 2.0f;

  ImGuiViewport const* vp = ImGui::GetMainViewport();
  float const bar_y = vp->Pos.y + vp->Size.y - bar_height;

  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, bar_y));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, bar_height));
  ImGui::SetNextWindowViewport(
      vp->ID);  // keep on main viewport, don't spawn platform window
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, pad_y));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));

  ImGuiWindowFlags const flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav;

  if (ImGui::Begin("##statusbar", nullptr, flags)) {
    // Track statusbar height for bottombar layout
    int const h = static_cast<int>(ImGui::GetWindowSize().y);
    if (h != s_statusbar_h) {
      s_statusbar_h = h;
      s_bottombar_height_dirty = true;
    }

    // ── Drive activity LEDs ──
    {
      float const frameH = ImGui::GetFrameHeight();
      for (int drv = 0; drv < 2; drv++) {
        bool const active =
            drv == 0 ? imgui_state.drive_a_led : imgui_state.drive_b_led;
        t_drive const& drive = drv == 0 ? driveA : driveB;
        auto& driveFile = drv == 0 ? CPC.driveA.file : CPC.driveB.file;
        const char* driveLabel = drv == 0 ? "A:" : "B:";

        if (drv > 0) ImGui::SameLine(0, 12.0f);

        // Build display name
        const char* fullName;
        if (drive.tracks) {
          auto pos = driveFile.find_last_of("/\\");
          fullName = (pos != std::string::npos) ? driveFile.c_str() + pos + 1
                                                : driveFile.c_str();
        } else {
          fullName = "(no disk)";
        }

        // Push unique ID per drive to avoid conflicts
        ImGui::PushID(100 + drv);  // offset IDs to avoid clashes with topbar

        ImGui::BeginGroup();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(driveLabel);
        ImGui::SameLine(0, 2.0f);

        // Draw LED
        ImVec2 const cursor = ImGui::GetCursorScreenPos();
        float const ledW = 16.0f;
        float const ledH = 8.0f;
        float const yOff = (frameH - ledH) * 0.5f;
        ImVec2 const p0(cursor.x, cursor.y + yOff);
        ImVec2 const p1(p0.x + ledW, p0.y + ledH);

        draw_status_led(ImGui::GetWindowDrawList(), p0, p1, active, 255, 0, 0);

        ImGui::Dummy(ImVec2(ledW, frameH));
        ImGui::SameLine(0, 4.0f);

        // Show track number when disk is loaded
        if (drive.tracks) {
          char trkStr[8];
          snprintf(trkStr, sizeof(trkStr), "T%02d",
                   static_cast<int>(drive.current_track));
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(trkStr);
          ImGui::PopStyleColor();
          ImGui::SameLine(0, 4.0f);
        }

        // Show filename or "(no disk)" with marquee scrolling
        ImGui::PushStyleColor(ImGuiCol_Text,
                              drive.tracks ? ImVec4(0.75f, 0.75f, 0.75f, 1.0f)
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
                {"Disk Images", "dsk;ipf;raw;zip"}};
            auto act = drv == 0 ? FileDialogAction::LoadDiskA_LED
                                : FileDialogAction::LoadDiskB_LED;
            SDL_ShowOpenFileDialog(
                file_dialog_callback,
                // NOLINTNEXTLINE(performance-no-int-to-ptr): intentional integer/pointer reinterpret for hardware/opaque handles
                reinterpret_cast<void*>(static_cast<intptr_t>(act)),
                mainSDLWindow, disk_filters, 1, CPC.current_dsk_path.c_str(),
                false);
          }
        }

        ImGui::PopID();
      }
    }

    // ── M4 Board activity LED (green, only shown when M4 is enabled) ──
    if (g_m4board.enabled) {
      float const frameH = ImGui::GetFrameHeight();
      bool const active = g_m4board.activity_frames > 0;

      ImGui::SameLine(0, 12.0f);
      ImGui::BeginGroup();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("M4:");
      ImGui::SameLine(0, 2.0f);

      ImVec2 const cursor = ImGui::GetCursorScreenPos();
      float const ledW = 16.0f;
      float const ledH = 8.0f;
      float const yOff = (frameH - ledH) * 0.5f;
      ImVec2 const p0(cursor.x, cursor.y + yOff);
      ImVec2 const p1(p0.x + ledW, p0.y + ledH);

      draw_status_led(ImGui::GetWindowDrawList(), p0, p1, active, 0, 255, 0);

      ImGui::Dummy(ImVec2(ledW, frameH));

      // Show container name if inside a DSK (with marquee scrolling)
      if (g_m4board.container_type != M4Board::ContainerType::NONE) {
        ImGui::SameLine(0, 4.0f);
        // Cache the container filename — only changes on container open/close.
        static std::string cached_path;
        static std::string cached_fname;
        if (g_m4board.container_host_path != cached_path) {
          cached_path = g_m4board.container_host_path;
          auto pos = cached_path.find_last_of("/\\");
          cached_fname = (pos != std::string::npos)
                             ? cached_path.substr(pos + 1)
                             : cached_path;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.9f, 0.45f, 1.0f));
        ImGui::AlignTextToFramePadding();
        imgui_marquee_text(cached_fname.c_str(), 120.0f);
        ImGui::PopStyleColor();
      }

      ImGui::EndGroup();
    }

    // ── Separator ──
    ImGui::SameLine(0, 12.0f);
    {
      ImVec2 const cursor = ImGui::GetCursorScreenPos();
      float const frameH = ImGui::GetFrameHeight();
      ImGui::GetWindowDrawList()->AddLine(
          ImVec2(cursor.x, cursor.y + 2.0f),
          ImVec2(cursor.x, cursor.y + frameH - 2.0f),
          IM_COL32(0x50, 0x50, 0x50, 0xFF), 1.0f);
      ImGui::Dummy(ImVec2(1.0f, frameH));
    }

    // ── TAPE section ──
    {
      bool const tape_loaded = !pbTapeImage.empty();
      bool const tape_playing =
          tape_loaded && CPC.tape_motor && CPC.tape_play_button;

      ImGui::SameLine(0, 8.0f);
      ImGui::AlignTextToFramePadding();

      ImU32 const color_active = IM_COL32(0x00, 0xFF, 0x80, 0xFF);
      ImU32 const label_color =
          tape_playing ? color_active : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);

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
          fullTapeName = (pos != std::string::npos)
                             ? CPC.tape.file.c_str() + pos + 1
                             : CPC.tape.file.c_str();
        } else {
          fullTapeName = "(no tape)";
        }
        ImGui::PushStyleColor(ImGuiCol_Text,
                              tape_loaded ? ImVec4(0.75f, 0.75f, 0.75f, 1.0f)
                                          : ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
        ImGui::AlignTextToFramePadding();
        imgui_marquee_text(fullTapeName, 120.0f);
        ImGui::PopStyleColor();
        if (!tape_loaded && ImGui::IsItemClicked()) {
          static const SDL_DialogFileFilter tape_filters[] = {
              {"Tape Images", "cdt;voc;zip"}};
          SDL_ShowOpenFileDialog(file_dialog_callback,
                                 // NOLINTNEXTLINE(performance-no-int-to-ptr): intentional integer/pointer reinterpret for hardware/opaque handles
                                 reinterpret_cast<void*>(static_cast<intptr_t>(
                                     FileDialogAction::LoadTape_LED)),
                                 mainSDLWindow, tape_filters, 1,
                                 CPC.current_tape_path.c_str(), false);
        }
      }

      // ── Transport buttons (gray SmallButtons) ──
      ImGui::SameLine(0, 6);
      {
        // Gray button style
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.15f, 0.15f, 0.15f, 1.0f));

        bool const at_start =
            !tape_loaded || imgui_state.tape_current_block <= 0;
        bool const at_end =
            !tape_loaded || imgui_state.tape_block_offsets.empty();
        bool const is_playing = tape_loaded && CPC.tape_play_button;

        // |◀ Prev block
        ImGui::BeginDisabled(at_start);
        if (ImGui::SmallButton("\xe2\x97\x80##sb_prev")) {  // ◀
          int const prev = imgui_state.tape_current_block - 1;
          if (prev >= 0 &&
              prev < static_cast<int>(imgui_state.tape_block_offsets.size())) {
            CPC.tape_play_button = 0;
            imgui_state.tape_current_block = prev;
            // engine=1: the sub-cycle deck is a separate device — request the
            // same block seek by ordinal (applied on the Z80 thread at the frame
            // boundary; the deck walks its own cdt to that block).
            subcycle_bridge_request_tape_seek(static_cast<uint32_t>(prev));
          }
        }
        {  // Draw bar on left side of prev button
          ImVec2 const rmin = ImGui::GetItemRectMin();
          ImVec2 const rmax = ImGui::GetItemRectMax();
          float const bx = rmin.x + ImGui::GetStyle().FramePadding.x - 1.0f;
          float const pad = (rmax.y - rmin.y) * 0.15f;
          ImU32 const barCol = at_start ? IM_COL32(0x50, 0x50, 0x50, 0xFF)
                                        : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
          ImGui::GetWindowDrawList()->AddLine(
              ImVec2(bx, rmin.y + pad), ImVec2(bx, rmax.y - pad), barCol, 2.0f);
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0, 2);

        // ▶ Play
        if (is_playing) {
          // Highlight play button green when playing
          ImGui::PushStyleColor(ImGuiCol_Button,
                                ImVec4(0.0f, 0.35f, 0.18f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                ImVec4(0.0f, 0.45f, 0.25f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                ImVec4(0.0f, 0.25f, 0.12f, 1.0f));
        }
        ImGui::BeginDisabled(!tape_loaded || is_playing);
        if (ImGui::SmallButton("\xe2\x96\xb6##sb_play")) {  // ▶
          CPC.tape_play_button = 0x10;
        }
        ImGui::EndDisabled();
        if (is_playing) ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 2);

        // ⏹ Stop
        ImGui::BeginDisabled(!is_playing);
        if (ImGui::SmallButton("\xe2\x96\xa0##sb_stop")) {  // ■
          CPC.tape_play_button = 0;
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0, 2);

        // ▷| Next block
        ImGui::BeginDisabled(
            at_end ||
            imgui_state.tape_current_block >=
                static_cast<int>(imgui_state.tape_block_offsets.size()) - 1);
        if (ImGui::SmallButton("\xe2\x96\xb6##sb_next")) {  // ▶
          int const next = imgui_state.tape_current_block + 1;
          if (next < static_cast<int>(imgui_state.tape_block_offsets.size())) {
            CPC.tape_play_button = 0;
            imgui_state.tape_current_block = next;
            // engine=1: mirror the seek onto the sub-cycle deck by ordinal (see Prev).
            subcycle_bridge_request_tape_seek(static_cast<uint32_t>(next));
          }
        }
        {  // Draw bar on right side of next button
          ImVec2 const rmin = ImGui::GetItemRectMin();
          ImVec2 const rmax = ImGui::GetItemRectMax();
          float const bx = rmax.x - ImGui::GetStyle().FramePadding.x + 1.0f;
          float const pad = (rmax.y - rmin.y) * 0.15f;
          bool const dis =
              at_end ||
              imgui_state.tape_current_block >=
                  static_cast<int>(imgui_state.tape_block_offsets.size()) - 1;
          ImU32 const barCol = dis ? IM_COL32(0x50, 0x50, 0x50, 0xFF)
                                   : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
          ImGui::GetWindowDrawList()->AddLine(
              ImVec2(bx, rmin.y + pad), ImVec2(bx, rmax.y - pad), barCol, 2.0f);
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0, 2);

        // ⏏ Eject
        ImGui::BeginDisabled(!tape_loaded);
        if (ImGui::SmallButton("\xe2\x8f\x8f##sb_eject")) {  // ⏏
          imgui_state.eject_confirm_tape = true;
        }
        ImGui::EndDisabled();

        ImGui::PopStyleColor(3);  // gray button style
      }

      // ── Block counter ──
      if (tape_loaded && !imgui_state.tape_block_offsets.empty()) {
        ImGui::SameLine(0, 4);
        char blockStr[32];
        snprintf(blockStr, sizeof(blockStr), "%d/%d",
                 imgui_state.tape_current_block + 1,
                 static_cast<int>(imgui_state.tape_block_offsets.size()));
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
          memset(imgui_state.tape_decoded_buf, 0,
                 sizeof(imgui_state.tape_decoded_buf));
        }

        ImGui::SameLine(0, 4);
        float const frameH = ImGui::GetFrameHeight();
        ImU32 const color_active = IM_COL32(0x00, 0xFF, 0x80, 0xFF);
        ImU32 const color_dim = IM_COL32(0x00, 0x40, 0x20, 0xFF);

        float const waveW = 100.0f;
        ImVec2 const cursor = ImGui::GetCursorScreenPos();
        float const yOff = (frameH - (frameH * 0.8f)) * 0.5f;
        ImVec2 const p0(cursor.x, cursor.y + yOff);
        float const boxH = frameH * 0.8f;
        ImVec2 const p1(p0.x + waveW, p0.y + boxH);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, IM_COL32(0x10, 0x10, 0x10, 0xFF));
        dl->AddRect(p0, p1,
                    tape_playing ? IM_COL32(0x00, 0x80, 0x40, 0x80)
                                 : IM_COL32(0x00, 0x30, 0x18, 0x60));

        ImU32 const wave_color = tape_playing ? color_active : color_dim;
        constexpr int N = ImGuiUIState::TAPE_WAVE_SAMPLES;
        float const stepX = waveW / static_cast<float>(N - 1);
        int const mode = imgui_state.tape_wave_mode;

        float yBot = p1.y - 2.0f;
        float yTop = p0.y + 2.0f;
        auto yForSample = [&](byte val) -> float { return val ? yTop : yBot; };
        int const oldest = imgui_state.tape_wave_head;

        if (mode == 0) {
          ImVec2 points[(N * 2) + 2];
          int nPoints = 0;
          float prevY = yForSample(imgui_state.tape_wave_buf[oldest]);
          points[nPoints++] = ImVec2(p0.x, prevY);
          for (int i = 1; i < N; i++) {
            int const idx = (oldest + i) % N;
            float const curX = p0.x + (i * stepX);
            float const curY = yForSample(imgui_state.tape_wave_buf[idx]);
            if (curY != prevY) {
              points[nPoints++] = ImVec2(curX, prevY);
              points[nPoints++] = ImVec2(curX, curY);
              prevY = curY;
            }
          }
          points[nPoints++] = ImVec2(p1.x, prevY);
          dl->AddPolyline(points, nPoints, wave_color, 0, 1.0f);
        } else {
          int const dN = ImGuiUIState::TAPE_DECODED_SAMPLES;
          int const dHead = imgui_state.tape_decoded_head;
          int visCount = static_cast<int>(waveW);
          visCount = std::min(visCount, dN);
          int const startIdx = (dHead - visCount + dN) % dN;
          ImU32 const col_one = tape_playing ? IM_COL32(0x00, 0xFF, 0x80, 0xFF)
                                             : IM_COL32(0x00, 0x44, 0x00, 0xFF);
          ImU32 const col_zero = tape_playing
                                     ? IM_COL32(0x00, 0x44, 0x00, 0xFF)
                                     : IM_COL32(0x00, 0x18, 0x00, 0xFF);
          for (int i = 0; i < visCount; i++) {
            int const idx = (startIdx + i) % dN;
            float const x = p0.x + (waveW - visCount) + i;
            ImU32 const c =
                imgui_state.tape_decoded_buf[idx] ? col_one : col_zero;
            dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + 1.0f, p1.y), c);
          }
        }

        {
          const char* modeLabel = (mode == 0) ? "RAW" : "BITS";
          ImVec2 const labelSize = ImGui::CalcTextSize(modeLabel);
          ImVec2 const labelPos(p1.x - labelSize.x - 2.0f, p0.y + 1.0f);
          dl->AddText(labelPos, IM_COL32(0x80, 0x80, 0x80, 0xA0), modeLabel);
        }

        ImGui::Dummy(ImVec2(waveW, frameH));
        if (ImGui::IsItemClicked()) {
          imgui_state.tape_wave_mode = (imgui_state.tape_wave_mode + 1) % 2;
        }
        // Right-click: volume for the auto-armed tape data monitor (the
        // "screech"). Its own SDL stream — never the AY — so this scales only
        // that signal. Persisted as [sound] tape_data_volume.
        if (ImGui::BeginPopupContextItem("##tape_scope_ctx")) {
          ImGui::TextUnformatted("Tape data sound");
          // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
          float vol = tape_line_out_volume() * 100.0f;
          ImGui::SetNextItemWidth(140.0f);
          if (ImGui::SliderFloat("##tape_data_vol", &vol, 0.0f, 100.0f, "%.0f%%"))
            tape_line_out_set_volume(vol / 100.0f);
          ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "Left-click: cycle waveform (RAW pulse / decoded BITS)\n"
              "Right-click: tape data sound volume");
        }
      }
    }

    // ── First-run empty-state hint ──
    // beads-mng: when no media is loaded anywhere, show an unobtrusive,
    // right-aligned dim hint so a fresh launch isn't a blank dead-end.
    if (driveA.tracks == 0 && driveB.tracks == 0 && pbTapeImage.empty()) {
      const char* hint = "Drop a .dsk/.cdt or press F1 to load";
      float const hint_w = ImGui::CalcTextSize(hint).x;
      ImGui::SameLine(ImGui::GetWindowWidth() - hint_w - 10.0f);
      ImGui::AlignTextToFramePadding();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
      ImGui::TextUnformatted(hint);
      ImGui::PopStyleColor();
    }

    // ── Eject Disk confirmation popup ──
    // Latch the drive index when opening the popup — the popup may not
    // open until the next frame, and eject_confirm_drive could be reset
    // by the else branch before BeginPopupModal succeeds.
    static int popup_eject_drive = -1;
    if (imgui_state.eject_confirm_drive >= 0) {
      popup_eject_drive = imgui_state.eject_confirm_drive;
      imgui_state.eject_confirm_drive = -1;
      ImGui::OpenPopup("Eject Disk?##sb");
    }
    if (ImGui::BeginPopupModal("Eject Disk?##sb", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      const char* name = popup_eject_drive == 0 ? "A" : "B";
      ImGui::Text("Eject disk from drive %s?", name);
      ImGui::Spacing();
      if (ImGui::Button("Eject", ImVec2(80, 0))) {
        t_drive& drive = popup_eject_drive == 0 ? driveA : driveB;
        auto& driveFile =
            popup_eject_drive == 0 ? CPC.driveA.file : CPC.driveB.file;
        dsk_eject(&drive);
        driveFile.clear();
        popup_eject_drive = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        popup_eject_drive = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    // ── Eject Tape confirmation popup ──
    if (imgui_state.eject_confirm_tape) {
      ImGui::OpenPopup("Eject Tape?##sb");
    }
    if (ImGui::BeginPopupModal("Eject Tape?##sb", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
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
}  // namespace

// ─────────────────────────────────────────────────
// Save-state slot helpers (pause-menu state manager)
// ─────────────────────────────────────────────────

// Directory holding numbered save-state slots, derived from CPC.snap_path.
// CPC.snap_path may or may not end in a separator, so normalise it.
namespace {
std::string state_slots_dir() {
  std::string base = CPC.snap_path;
  if (base.empty()) base = ".";
  if (base.back() != '/' && base.back() != '\\') base += '/';
  std::string dir = base + "states/";
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}
}  // namespace

namespace {
std::string state_slot_path(int i) {
  return state_slots_dir() + "state" + std::to_string(i) + ".sna";
}
}  // namespace

namespace {
std::string state_slot_png(int i) {
  return state_slots_dir() + "state" + std::to_string(i) + ".png";
}
}  // namespace

// Raw RGBA thumbnail (".kthm") captured at save time for the pause-screen grid.
namespace {
std::string state_slot_thumb(int i) {
  return state_slots_dir() + "state" + std::to_string(i) + ".kthm";
}
}  // namespace

// Per-slot thumbnail texture cache (slots 1..8 -> index 1..8; index 0 unused).
// `sig` is the thumbnail file's last-write-time count, used to detect
// staleness.
namespace {
struct SlotThumb {
  uintptr_t tex = 0;
  std::string sig;
};
SlotThumb g_slot_thumb[9];

// Free + clear a cached slot texture (e.g. before re-saving over it).
void invalidate_slot_thumb(int i) {
  if (i < 0 || i > 8) return;
  if (g_slot_thumb[i].tex) {
    video_free_rgba_texture(g_slot_thumb[i].tex);
    g_slot_thumb[i].tex = 0;
  }
  g_slot_thumb[i].sig.clear();
}
}  // namespace

// Public: free ALL cached slot thumbnail textures.  Called before
// video_shutdown() on a renderer switch so the GPU handles are freed against
// the live device and the grid reloads fresh textures from the new backend.
void imgui_invalidate_slot_thumbs() {
  for (int i = 0; i < 9; ++i) invalidate_slot_thumb(i);
}

namespace {
bool state_slot_exists(int i) {
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  std::error_code ec;
  return std::filesystem::exists(state_slot_path(i), ec);
}
}  // namespace

// Last-write time of a slot, formatted "MMM DD HH:MM", or "" if missing.
namespace {
std::string state_slot_time(int i) {
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  std::error_code ec;
  std::string const path = state_slot_path(i);
  if (!std::filesystem::exists(path, ec)) return "";
  auto ftime = std::filesystem::last_write_time(path, ec);
  if (ec) return "";
  // Portable file_time -> system_clock conversion (avoids C++20 clock_cast).
  auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - std::filesystem::file_time_type::clock::now() +
      std::chrono::system_clock::now());
  std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
  std::tm tm_buf{};
#ifdef _WIN32
  if (localtime_s(&tm_buf, &tt) != 0) return "";
#else
  if (localtime_r(&tt, &tm_buf) == nullptr) return "";
#endif
  char out[32];
  if (std::strftime(out, sizeof(out), "%b %d %H:%M", &tm_buf) == 0) return "";
  return out;
}
}  // namespace

namespace {
void save_state_slot(int i) {
  std::string const path = state_slot_path(i);  // also creates the directory
  int const rc = snapshot_save(path);
  if (rc == 0) {
    video_request_window_screenshot(state_slot_png(i));
    // Capture a small raw-RGBA thumbnail for the pause-screen grid, then drop
    // the cached texture so the grid reloads the fresh image.
    video_capture_cpc_thumbnail(state_slot_thumb(i), 160);
    invalidate_slot_thumb(i);
    set_osd_message("Saved state " + std::to_string(i));
  } else {
    set_osd_message("Save state " + std::to_string(i) + " failed");
  }
}
}  // namespace

namespace {
void load_state_slot(int i) {
  // snapshot_load pauses internally; we are already paused here.
  snapshot_load(state_slot_path(i));
  set_osd_message("Loaded state " + std::to_string(i));
}
}  // namespace

// ─────────────────────────────────────────────────
// Menu
// ─────────────────────────────────────────────────

namespace {
void imgui_render_menu() {
  ImGuiViewport* mvp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(mvp->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  ImGui::SetNextWindowSize(ImVec2(360, 0));
  ImGui::SetNextWindowViewport(mvp->ID);

  // Darken the emulator behind the overlay (the classic dimmed pause backdrop).
  // Drawn on the background list so it sits over the CPC screen but under this
  // window.
  ImGui::GetBackgroundDrawList(mvp)->AddRectFilled(
      mvp->Pos, ImVec2(mvp->Pos.x + mvp->Size.x, mvp->Pos.y + mvp->Size.y),
      IM_COL32(0, 0, 0, 150));

  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
      ImGuiWindowFlags_AlwaysAutoResize;

  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  bool menu_open = true;
  if (!ImGui::Begin("konCePCja", &menu_open, flags)) {
    if (!menu_open) imgui_close_menu();
    ImGui::End();
    return;
  }
  if (!menu_open) {
    imgui_close_menu();
    ImGui::End();
    return;
  }

  // Keyboard shortcuts within pause menu
  bool action = false;
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      imgui_close_menu();
      ImGui::End();
      return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
      emulator_reset();
      action = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Q)) {
      imgui_state.show_quit_confirm = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_A)) {
      imgui_state.show_about = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
      emulator_reset();
      action = true;
    }
  }

  float const bw = ImGui::GetContentRegionAvail().x;

  // ── Section 1: Transport ────────────────────────────────────────────
  ImGui::TextDisabled("PAUSED");
  ImGui::Spacing();

  // Enable keyboard navigation for this window (arrows/tab cycle buttons)
  if (imgui_state.menu_just_opened) {
    ImGui::SetKeyboardFocusHere();
    imgui_state.menu_just_opened = false;
  }
  if (ImGui::Button("Resume (Esc)", ImVec2(bw, 0))) {
    action = true;
  }
  {
    float const half = (bw - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    std::string const reset_lbl =
        "Reset (" + koncpc_action_shortcut(KONCPC_RESET) + ")";
    if (ImGui::Button(reset_lbl.c_str(), ImVec2(half, 0))) {
      emulator_reset();
      action = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Screenshot", ImVec2(half, 0))) {
      std::string dir = CPC.sdump_dir;
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      std::error_code ec;
      if (dir.empty() || !std::filesystem::is_directory(dir, ec)) dir = ".";
      std::string const shot = dir + "/screenshot_" + getDateString() + ".png";
      video_request_window_screenshot(shot);
      set_osd_message("Screenshot saved");
    }
  }

  // ── Section 2: Status dashboard ─────────────────────────────────────
  ImGui::Separator();
  ImGui::TextDisabled("Status");
  ImGui::Spacing();

  auto basename_or_dash = [](const std::string& p) -> std::string {
    if (p.empty()) return "—";
    return std::filesystem::path(p).filename().string();
  };

  ImGui::Columns(2, "pause_status", false);
  ImGui::Text("Disk A:");
  ImGui::NextColumn();
  ImGui::TextDisabled("%s", basename_or_dash(CPC.driveA.file).c_str());
  ImGui::NextColumn();
  ImGui::Text("Disk B:");
  ImGui::NextColumn();
  ImGui::TextDisabled("%s", basename_or_dash(CPC.driveB.file).c_str());
  ImGui::NextColumn();
  ImGui::Text("Tape:");
  ImGui::NextColumn();
  ImGui::TextDisabled("%s", basename_or_dash(CPC.tape.file).c_str());
  ImGui::NextColumn();
  ImGui::Text("Cartridge:");
  ImGui::NextColumn();
  ImGui::TextDisabled("%s", basename_or_dash(CPC.cartridge.file).c_str());
  ImGui::NextColumn();

  static const char* const kModelNames[] = {"CPC 464", "CPC 664",
                                                  "CPC 6128", "6128+"};
  const char* model_name =
      (CPC.model < IM_ARRAYSIZE(kModelNames)) ? kModelNames[CPC.model] : "?";
  ImGui::Text("Model:");
  ImGui::NextColumn();
  ImGui::TextDisabled("%s", model_name);
  ImGui::NextColumn();
  ImGui::Text("RAM:");
  ImGui::NextColumn();
  ImGui::TextDisabled("%u KB", CPC.ram_size);
  ImGui::NextColumn();
  ImGui::Text("CRTC:");
  ImGui::NextColumn();
  ImGui::TextDisabled("%s", crtc_type_chip_name(CRTC.crtc_type));
  ImGui::NextColumn();

  // Uptime derived from the global frame counter (50 frames per second).
  dword const secs = dwFrameCountOverall / 50;
  ImGui::Text("Uptime:");
  ImGui::NextColumn();
  ImGui::TextDisabled("%u:%02u", secs / 60, secs % 60);
  ImGui::NextColumn();
  ImGui::Columns(1);

  // ── Section 3: Save-state manager ───────────────────────────────────
  ImGui::Separator();
  ImGui::TextDisabled("Save States");
  ImGui::Spacing();

  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  static int state_mode = 0; // 0 = Load, 1 = Save
  ImGui::RadioButton("Load", &state_mode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Save", &state_mode, 1);
  ImGui::Spacing();

  const int kCols = 4;
  float const spacing = ImGui::GetStyle().ItemSpacing.x;
  float cell_w = (bw - (spacing * (kCols - 1))) / kCols;
  // Every slot is one uniform 4:3 cell (4 wide : 3 tall): the thumbnail (or a
  // dark "empty" fill) with the label centered on top — built in the loop.
  float const cell_h = cell_w * 3.0f / 4.0f;

  // Return a cached/ refreshed thumbnail texture for slot i, or 0 if none.
  auto slot_thumb_texture = [](int i) -> uintptr_t {
    // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
    std::string tp = state_slot_thumb(i);
    // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
    std::error_code ec;
    if (!std::filesystem::exists(tp, ec)) {
      invalidate_slot_thumb(i);
      return 0;
    }
    auto wt = std::filesystem::last_write_time(tp, ec);
    std::string const sig = ec ? std::string()
                               : std::to_string(static_cast<long long>(
                                     wt.time_since_epoch().count()));
    if (g_slot_thumb[i].tex && g_slot_thumb[i].sig == sig) {
      return g_slot_thumb[i].tex;  // cache hit
    }
    // Miss or stale: free old texture, reload from disk.
    invalidate_slot_thumb(i);
    std::vector<unsigned char> rgba;
    int tw = 0, th = 0;
    if (video_load_rgba_thumbnail(tp, rgba, tw, th)) {
      uintptr_t const tex = video_make_rgba_texture(rgba.data(), tw, th);
      if (tex) {
        g_slot_thumb[i].tex = tex;
        g_slot_thumb[i].sig = sig;
        return tex;
      }
    }
    return 0;
  };

  for (int i = 1; i <= 8; ++i) {
    bool const exists = state_slot_exists(i);
    std::string const when = exists ? state_slot_time(i) : std::string("Empty");
    uintptr_t const thumb = exists ? slot_thumb_texture(i) : 0;

    bool const disabled = (state_mode == 0 && !exists);
    if (disabled) ImGui::BeginDisabled();

    // Uniform 4:3 cell: an invisible button for hit-testing, then we paint the
    // image (or empty fill) and the centered label on top via the draw list.
    char id[16];
    snprintf(id, sizeof(id), "##slot%d", i);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool const clicked = ImGui::InvisibleButton(id, ImVec2(cell_w, cell_h));
    bool const hovered = ImGui::IsItemHovered();
    ImVec2 const p1(p0.x + cell_w, p0.y + cell_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (thumb) {
      dl->AddImage(static_cast<ImTextureID>(thumb), p0, p1);
      // Scrim so the centered label stays readable over a bright frame.
      dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 70));
    } else {
      dl->AddRectFilled(p0, p1, IM_COL32(28, 28, 34, 255));
    }
    dl->AddRect(
        p0, p1,
        hovered ? IM_COL32(120, 170, 255, 255) : IM_COL32(80, 80, 90, 255),
        0.0f, 0, hovered ? 2.0f : 1.0f);

    // Centered label (normal font): slot number, then the date and time on
    // separate lines (or "Empty").
    auto draw_centered = [&](const char* str, float cy) {
      ImVec2 const ts = ImGui::CalcTextSize(str);
      ImVec2 const tp(p0.x + ((cell_w - ts.x) * 0.5f), cy);
      dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), str);
      dl->AddText(tp, IM_COL32(255, 255, 255, 255), str);
    };
    char num[8];
    snprintf(num, sizeof(num), "%d", i);
    std::string l_date = when, l_time;
    if (exists) {
      // when is "Mon DD HH:MM" — split the trailing time onto its own line.
      size_t const sp = when.find_last_of(' ');
      if (sp != std::string::npos) {
        l_date = when.substr(0, sp);
        l_time = when.substr(sp + 1);
      }
    }
    float const lh = ImGui::GetTextLineHeight();
    int const nlines = l_time.empty() ? 2 : 3;
    float const y0 = p0.y + ((cell_h - (lh * nlines)) * 0.5f);
    draw_centered(num, y0);
    draw_centered(l_date.c_str(), y0 + lh);
    if (!l_time.empty()) draw_centered(l_time.c_str(), y0 + (lh * 2.0f));

    if (clicked) {
      if (state_mode == 1) {
        save_state_slot(i);
      } else if (exists) {
        load_state_slot(i);
        action = true;
      }
    }
    if (disabled) ImGui::EndDisabled();

    if (i % kCols != 0 && i != 8) ImGui::SameLine();
  }

  ImGui::End();

  if (action) imgui_close_menu();

  // --- About popup ---
  if (imgui_state.show_about) {
    ImGui::OpenPopup("About konCePCja");
    imgui_state.show_about = false;
  }
  if (ImGui::BeginPopupModal("About konCePCja", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("konCePCja %s", VERSION_STRING);
    ImGui::Separator();
    ImGui::Text("Amstrad CPC Emulator");
    ImGui::Text("Clean-room, hardware-modelled emulation");
    ImGui::Text("Heritage: originally forked from Caprice32 (Ulrich Doewich)");
    ImGui::Spacing();
    ImGui::Text("Shortcuts:");
    ImGui::BulletText("%s - Menu", koncpc_action_shortcut(KONCPC_GUI).c_str());
    ImGui::BulletText("%s - DevTools",
                      koncpc_action_shortcut(KONCPC_DEVTOOLS).c_str());
    ImGui::BulletText("%s - Reset",
                      koncpc_action_shortcut(KONCPC_RESET).c_str());
    ImGui::BulletText("%s - Quit", koncpc_action_shortcut(KONCPC_EXIT).c_str());
    ImGui::BulletText("%s - Screenshot",
                      koncpc_action_shortcut(KONCPC_SCRNSHOT).c_str());
#ifdef __APPLE__
    ImGui::BulletText("Cmd+K - Command Palette");
#else
    ImGui::BulletText("Ctrl+K - Command Palette");
#endif
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(120, 0))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}
}  // namespace

// ─────────────────────────────────────────────────
// Options
// ─────────────────────────────────────────────────

// No hardcoded video plugin list — combo is built dynamically from
// video_plugin_list.
namespace {
const char* scale_items[] = {"Fit window", "1x", "1.5x", "2x", "3x"};
}  // namespace
namespace {
const char* sample_rates[] = {"11025", "22050", "44100", "48000",
                                     "96000"};
}  // namespace
namespace {
const char* cpc_models[] = {"CPC 464", "CPC 664", "CPC 6128", "6128+"};
}  // namespace
namespace {
const char* ram_sizes[] = {
    "64 KB",  "128 KB", "192 KB", "256 KB",
    "320 KB", "512 KB", "576 KB", "4160 KB (Yarek 4MB)"};
}  // namespace
namespace {
int ram_size_values[] = {64, 128, 192, 256, 320, 512, 576, 4160};
}  // namespace

namespace {
const char* crtc_type_labels[] = {
    "Type 0 - HD6845S (Hitachi)", "Type 1 - UM6845R (UMC)",
    "Type 2 - MC6845 (Motorola)", "Type 3 - AMS40489 (Amstrad ASIC)"};
}  // namespace

// find_ram_index and find_sample_rate_index moved to imgui_ui_testable.h

namespace {
void imgui_render_options() {
  static bool first_open = true;
  static unsigned char old_crtc_type = 0;
  static bool old_m4_enabled = false;
  if (first_open) {
    imgui_state.old_cpc_settings = CPC;
    old_crtc_type = CRTC.crtc_type;
    old_m4_enabled = g_m4board.enabled;
    first_open = false;
  }

  ImGuiViewport const* mvp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(mvp->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(480, 420), ImGuiCond_Appearing);
  ImGui::SetNextWindowViewport(mvp->ID);

  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  bool open = true;
  if (!ImGui::Begin("Options", &open, ImGuiWindowFlags_NoCollapse)) {
    if (!open) {
      imgui_state.show_options = false;
      first_open = true;
    }
    ImGui::End();
    return;
  }

  if (ImGui::BeginTabBar("OptionsTabs")) {
    // ── General Tab ──
    if (ImGui::BeginTabItem("General", nullptr,
                            s_pending_options_tab == OptionsTab::General
                                ? ImGuiTabItemFlags_SetSelected
                                : 0)) {
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      int model = static_cast<int>(CPC.model);
      if (ImGui::Combo("CPC Model", &model, cpc_models,
                       IM_ARRAYSIZE(cpc_models))) {
        CPC.model = model;
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      int ram_idx = find_ram_index(CPC.ram_size);
      if (ImGui::Combo("RAM Size", &ram_idx, ram_sizes,
                       IM_ARRAYSIZE(ram_sizes))) {
        CPC.ram_size = ram_size_values[ram_idx];
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      int crtc = static_cast<int>(CRTC.crtc_type);
      if (ImGui::Combo("CRTC Type", &crtc, crtc_type_labels,
                       IM_ARRAYSIZE(crtc_type_labels))) {
        CRTC.crtc_type = static_cast<unsigned char>(crtc);
          if (subcycle::Machine* m = subcycle_bridge_machine())
            m->set_crtc_type(static_cast<uint8_t>(crtc));
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Auto-set by CPC Model on reset.\nOverride for compatibility "
            "testing.");
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool limit = CPC.limit_speed != 0;
      if (ImGui::Checkbox("Limit Speed", &limit)) {
        CPC.limit_speed = limit ? 1 : 0;
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool frameskip = CPC.frameskip != 0;
      if (ImGui::Checkbox("Auto Frameskip", &frameskip)) {
        CPC.frameskip = frameskip ? 1 : 0;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Skip rendering frames when emulation falls behind 100%% "
            "speed.\nOnly has an effect when 'Limit Speed' is enabled.");
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      int speed = static_cast<int>(CPC.speed);
      if (ImGui::SliderInt("Speed", &speed, MIN_SPEED_SETTING,
                           MAX_SPEED_SETTING)) {
        CPC.speed = speed;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "CPU clock speed in MHz (default: 4).\nHigher = faster emulation.");
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool printer = CPC.printer != 0;
      if (ImGui::Checkbox("Printer Capture", &printer)) {
        CPC.printer = printer ? 1 : 0;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Saves CPC printer output to:\n%s",
                          CPC.printer_file.c_str());
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool sw = g_smartwatch.enabled;
      if (ImGui::Checkbox("SmartWatch RTC", &sw)) {
        g_smartwatch.enabled = sw;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Dobbertin SmartWatch (DS1216) in upper ROM socket.\nProvides "
            "real-time clock via host system time.");
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool sf2 = g_symbiface.enabled;
      if (ImGui::Checkbox("Symbiface II", &sf2)) {
        g_symbiface.enabled = sf2;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Symbiface II expansion (IDE + RTC + PS/2 Mouse).\nConfigure IDE "
            "images in config file.");
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool m4 = g_m4board.enabled;
      if (ImGui::Checkbox("M4 Board", &m4)) {
        g_m4board.enabled = m4;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "M4 Board — Virtual WiFi/SD expansion.\nSee the M4 Board tab for "
            "full settings.");
      }

      ImGui::EndTabItem();
    }

    // ── ROMs Tab ──
    if (ImGui::BeginTabItem("ROMs", nullptr,
                            s_pending_options_tab == OptionsTab::ROMs
                                ? ImGuiTabItemFlags_SetSelected
                                : 0)) {
      ImGui::Text("Expansion ROM Slots:");
      ImGui::Spacing();
      if (ImGui::BeginTable(
              "rom_slots", 5,
              ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##status", ImGuiTableColumnFlags_WidthFixed,
                                16.0f);
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed,
                                34.0f);
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch,
                                1.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch,
                                1.0f);
        ImGui::TableSetupColumn("##unload", ImGuiTableColumnFlags_WidthFixed,
                                24.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < MAX_ROM_SLOTS; i++) {
          ImGui::PushID(i);
          ImGui::TableNextRow();

          bool const loaded = (memmap_ROM[i] != nullptr);

          // Status dot
          ImGui::TableSetColumnIndex(0);
          ImVec4 const dot_color = loaded ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                                          : ImVec4(0.5f, 0.5f, 0.5f, 0.5f);
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
            display =
                (CPC.model == 0) ? "(default - none)" : "amsdos.rom (default)";
          } else {
            // Show just the filename, not the full path
            size_t const sep = CPC.rom_file[i].find_last_of("/\\");
            display = (sep != std::string::npos)
                          ? CPC.rom_file[i].substr(sep + 1)
                          : CPC.rom_file[i];
          }
          if (display.length() > 24)
            display = "..." + display.substr(display.length() - 21);

          if (ImGui::Selectable(display.c_str())) {
            static const SDL_DialogFileFilter filters[] = {
                {"ROM files", "rom;bin"}};
            imgui_state.pending_rom_slot = i;
            SDL_ShowOpenFileDialog(
                file_dialog_callback,
                // NOLINTNEXTLINE(performance-no-int-to-ptr): intentional integer/pointer reinterpret for hardware/opaque handles
                reinterpret_cast<void*>(
                    static_cast<intptr_t>(FileDialogAction::LoadROM)),
                mainSDLWindow, filters, 1, CPC.rom_path.c_str(), false);
          }
          if (!CPC.rom_file[i].empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", CPC.rom_file[i].c_str());
          }

          // Identified ROM name
          ImGui::TableSetColumnIndex(3);
          if (loaded) {
            std::string const id = rom_identify(memmap_ROM[i]);
            if (!id.empty()) {
              ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                                 id.c_str());
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
    if (ImGui::BeginTabItem("Video", nullptr,
                            s_pending_options_tab == OptionsTab::Video
                                ? ImGuiTabItemFlags_SetSelected
                                : 0)) {
      // Build combo dynamically from video_plugin_list, skipping hidden
      // entries. Group plugins by type with section headers.
      const char* preview = video_plugin_list[CPC.scr_style].name;
      if (ImGui::BeginCombo("Video Plugin", preview)) {
        const char* prev_group = nullptr;
        for (size_t i = 0; i < video_plugin_list.size(); i++) {
          if (video_plugin_list[i].hidden) continue;
          // Determine group from plugin name
          const char* name = video_plugin_list[i].name;
          const char* group;
          if (strncmp(name, "CRT", 3) == 0)
            group = "GPU — CRT Shaders";
          else if (strcmp(name, "Direct") == 0 ||
                   strcmp(name, "Direct (SDL)") == 0 ||
                   strcmp(name, "OpenGL scaling") == 0)
            group = "GPU — Direct";
          else
            group = "CPU — Software Scalers";
          if (!prev_group || strcmp(prev_group, group) != 0) {
            if (prev_group) ImGui::Spacing();
            ImGui::SeparatorText(group);
            prev_group = group;
          }
          bool const selected =
              (static_cast<int>(i) == static_cast<int>(CPC.scr_style));
          if (ImGui::Selectable(video_plugin_list[i].name, selected)) {
            CPC.scr_style = static_cast<int>(i);
            imgui_state.video_reinit_pending = true;
          }
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "GPU plugins render on the graphics card (fast).\nCPU plugins "
            "compute pixels in software (slow at high res).");
      }

      // Scale combo: 0 = Fit window, 1-4 = 1x-4x
      // scr_scale 0 means "fit window" (no fixed multiplier).
      int scale_idx = static_cast<int>(CPC.scr_scale);
      if (scale_idx < 0 || scale_idx > 4) scale_idx = 0;
      if (ImGui::Combo("Scale", &scale_idx, scale_items,
                       IM_ARRAYSIZE(scale_items))) {
        apply_scr_scale(scale_idx);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Fit window: CPC image fills available space.\n1x-4x: fixed pixel "
            "size (cropped if window is smaller).");
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool crt_aspect = CPC.scr_crt_aspect != 0;
      if (ImGui::Checkbox("CRT aspect ratio (4:3)", &crt_aspect)) {
        CPC.scr_crt_aspect = crt_aspect ? 1 : 0;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Stretch to 4:3 like a real CPC monitor.\nOff = square pixels "
            "(768:270 raw ratio).");
      }

      bool const colour = CPC.scr_tube == 0;
      if (ImGui::RadioButton("Colour", colour)) {
        CPC.scr_tube = 0;
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("Mono (Green)", !colour)) {
        CPC.scr_tube = 1;
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      int intensity = static_cast<int>(CPC.scr_intensity);
      if (ImGui::SliderInt("Intensity", &intensity, 5, 15)) {
        CPC.scr_intensity = intensity;
        video_set_palette();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "CRT phosphor brightness.\nHigher = brighter colours.");
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool scanlines = CPC.scr_scanlines != 0;
      if (ImGui::Checkbox("Scanlines", &scanlines)) {
        CPC.scr_scanlines = scanlines ? 1 : 0;
        if (!scanlines) {
          CPC.scr_oglscanlines = 0;
          video_set_palette();
        }
      }
      if (scanlines) {
        // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
        int sl_intensity = static_cast<int>(CPC.scr_oglscanlines);
        if (ImGui::SliderInt("Scanline Intensity", &sl_intensity, 0, 100)) {
          CPC.scr_oglscanlines = sl_intensity;
          video_set_palette();
        }
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool fps = CPC.scr_fps != 0;
      if (ImGui::Checkbox("Show FPS", &fps)) {
        CPC.scr_fps = fps ? 1 : 0;
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool fullscreen = CPC.scr_window == 0;
      if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
        CPC.scr_window = fullscreen ? 0 : 1;
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool aspect = CPC.scr_preserve_aspect_ratio != 0;
      if (ImGui::Checkbox("Preserve Aspect Ratio", &aspect)) {
        CPC.scr_preserve_aspect_ratio = aspect ? 1 : 0;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "When off, the CPC screen stretches\nto fill the entire window.");
      }

      ImGui::EndTabItem();
    }

    // ── Audio Tab ──
    if (ImGui::BeginTabItem("Audio", nullptr,
                            s_pending_options_tab == OptionsTab::Audio
                                ? ImGuiTabItemFlags_SetSelected
                                : 0)) {
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool snd = CPC.snd_enabled != 0;
      if (ImGui::Checkbox("Enable Sound", &snd)) {
        CPC.snd_enabled = snd ? 1 : 0;
      }

      static constexpr int kDefaultSampleRateIndex = 2;  // 44100 Hz
      int rate_idx = static_cast<int>(CPC.snd_playback_rate);
      // NOLINTNEXTLINE(readability-redundant-casting): cast guards the macro from a clang-tidy mis-fix
      if (rate_idx < 0 || rate_idx >= static_cast<int>(IM_ARRAYSIZE(sample_rates))) {
        rate_idx = kDefaultSampleRateIndex;
        CPC.snd_playback_rate = rate_idx;  // fix invalid value immediately
      }
      if (ImGui::Combo("Sample Rate", &rate_idx, sample_rates,
                       IM_ARRAYSIZE(sample_rates))) {
        CPC.snd_playback_rate =
            rate_idx;  // store index (0-4), not raw frequency
      }

      bool const stereo = CPC.snd_stereo != 0;
      if (ImGui::RadioButton("Mono", !stereo)) {
        CPC.snd_stereo = 0;
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("Stereo", stereo)) {
        CPC.snd_stereo = 1;
      }

      bool const bits16 = CPC.snd_bits != 0;
      if (ImGui::RadioButton("8-bit", !bits16)) {
        CPC.snd_bits = 0;
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("16-bit", bits16)) {
        CPC.snd_bits = 1;
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      int vol = static_cast<int>(CPC.snd_volume);
      if (ImGui::SliderInt("Volume", &vol, 0, 100)) {
        CPC.snd_volume = vol;
      }

      ImGui::Separator();
      ImGui::Text("Peripherals");
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool pp = CPC.snd_pp_device != 0;
      if (ImGui::Checkbox("Digiblaster", &pp)) {
        CPC.snd_pp_device = pp ? 1 : 0;
      }
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool amdrum = g_amdrum.enabled;
      if (ImGui::Checkbox("AmDrum", &amdrum)) {
        g_amdrum.enabled = amdrum;
      }
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool disk_snd = g_drive_sounds.disk_enabled;
      if (ImGui::Checkbox("Disk Drive Sounds", &disk_snd)) {
        g_drive_sounds.disk_enabled = disk_snd;
      }
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool tape_snd = g_drive_sounds.tape_enabled;
      if (ImGui::Checkbox("Tape Sounds", &tape_snd)) {
        g_drive_sounds.tape_enabled = tape_snd;
      }

      ImGui::EndTabItem();
    }

    // ── Input Tab ──
    if (ImGui::BeginTabItem("Input", nullptr,
                            s_pending_options_tab == OptionsTab::Input
                                ? ImGuiTabItemFlags_SetSelected
                                : 0)) {
      int keyboard = static_cast<int>(CPC.keyboard);
      const char* const cpc_langs[] = {"English", "French", "Spanish"};
      int const max_langs = IM_ARRAYSIZE(cpc_langs);
      if (static_cast<int>(CPC.keyboard) >= max_langs) keyboard = 0;
      if (ImGui::Combo("CPC Language", &keyboard, cpc_langs, max_langs)) {
        CPC.keyboard = keyboard;
      }

      int ksm = static_cast<int>(CPC.keyboard_support_mode);
      const char* const ksm_modes[] = {"Direct", "Buffered Until Read",
                                             "Min. 2 Frames"};
      int const max_ksm = IM_ARRAYSIZE(ksm_modes);
      if (ksm < 0 || ksm >= max_ksm) ksm = 0;
      if (ImGui::Combo("Keyboard Support Mode", &ksm, ksm_modes, max_ksm)) {
        CPC.keyboard_support_mode = static_cast<KeyboardSupportMode>(ksm);
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool joy_emu = CPC.joystick_emulation != JoystickEmulation::None;
      if (ImGui::Checkbox("Joystick Emulation", &joy_emu)) {
        CPC.joystick_emulation =
            joy_emu ? JoystickEmulation::Keyboard : JoystickEmulation::None;
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool joysticks = CPC.joysticks != 0;
      if (ImGui::Checkbox("Use Real Joysticks", &joysticks)) {
        CPC.joysticks = joysticks ? 1 : 0;
      }

      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool amx = g_amx_mouse.enabled;
      if (ImGui::Checkbox("AMX Mouse", &amx)) {
        g_amx_mouse.enabled = amx;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "AMX Mouse on joystick port.\nMaps host mouse to CPC joystick "
            "directions + buttons.");
      }

      ImGui::EndTabItem();
    }

    // ── M4 Board Tab ──
    if (ImGui::BeginTabItem("M4 Board", nullptr,
                            s_pending_options_tab == OptionsTab::M4
                                ? ImGuiTabItemFlags_SetSelected
                                : 0)) {
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool m4_en = g_m4board.enabled;
      if (ImGui::Checkbox("Enable M4 Board", &m4_en)) {
        g_m4board.enabled = m4_en;
      }
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
            "  CAT  LOAD\"file\"  SAVE\"file\"  RUN\"file\"");
      }

      if (m4_en) {
        // ── SD Card ──
        ImGui::Separator();
        ImGui::TextDisabled("SD Card");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80.0f);
        char sd_buf[512];
        snprintf(sd_buf, sizeof(sd_buf), "%s", g_m4board.sd_root_path.c_str());
        ImGui::InputText("##m4sd", sd_buf, sizeof(sd_buf),
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Browse##m4sd")) {
          const char* default_loc = g_m4board.sd_root_path.empty()
                                        ? nullptr
                                        : g_m4board.sd_root_path.c_str();
          SDL_ShowOpenFolderDialog(
              file_dialog_callback,
              // NOLINTNEXTLINE(performance-no-int-to-ptr): intentional integer/pointer reinterpret for hardware/opaque handles
              reinterpret_cast<void*>(
                  static_cast<intptr_t>(FileDialogAction::SelectM4SDFolder)),
              mainSDLWindow, default_loc, false);
        }
        if (g_m4board.sd_root_path.empty()) {
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                             "No SD directory set");
        }

        int slot = g_m4board.rom_slot;
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("ROM Slot##m4", &slot, 1, 1)) {
          slot = std::max(slot, 0);
          slot = std::min(slot, 31);
          g_m4board.rom_slot = slot;
        }

        // ── Status ──
        ImGui::Separator();
        ImGui::TextDisabled("Status");

        bool const active = g_m4board.activity_frames > 0;
        ImVec4 const led_color = active ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                                        : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(led_color, "%s", active ? "SD Active" : "SD Idle");
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("Dir: %s", g_m4board.current_dir.c_str());

        int open_count = 0;
        for (auto& open_file : g_m4board.open_files) {
          if (open_file) open_count++;
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
        ImGui::Text("Network: %s",
                    g_m4board.network_enabled ? "enabled" : "disabled");
        ImGui::SameLine(0, 16);
        int sock_count = 0;
        for (int const socket : g_m4board.sockets) {
          if (socket != M4Board::INVALID_SOCK) sock_count++;
        }
        ImGui::TextDisabled("Sockets: %d/%d", sock_count, M4Board::MAX_SOCKETS);

        // ── HTTP Server ──
        ImGui::Separator();
        ImGui::TextDisabled("HTTP Server");

        int http_port = CPC.m4_http_port;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputInt("HTTP Port##m4http", &http_port, 1, 100)) {
          http_port = std::max(http_port, 1024);
          http_port = std::min(http_port, 65535);
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
              "Falls back to 127.0.0.1 if the address is unavailable.");
        }

        if (g_m4_http.is_running()) {
          ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f),
                             "Listening on %s:%d", g_m4_http.bind_ip().c_str(),
                             g_m4_http.port());
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
          ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                             "HTTP server stopped");
          ImGui::SameLine();
          if (ImGui::SmallButton("Start##m4http")) {
            g_m4_http.start(CPC.m4_http_port, CPC.m4_bind_ip);
          }
        }

        // ── Port Forwarding ──
        ImGui::Separator();
        ImGui::TextDisabled("Port Forwarding");
        ImGui::TextWrapped(
            "When CPC software binds a port (C_NETBIND), the emulator maps it "
            "to a host port. User overrides (white) are persisted; "
            "auto-assigned (gray) are not.");

        if (ImGui::BeginTable("##m4ports", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit)) {
          ImGui::TableSetupColumn("CPC Port", ImGuiTableColumnFlags_WidthFixed,
                                  70.0f);
          ImGui::TableSetupColumn("Host Port", ImGuiTableColumnFlags_WidthFixed,
                                  70.0f);
          ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,
                                  50.0f);
          ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed,
                                  50.0f);
          ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableHeadersRow();

          auto mappings = g_m4_http.get_port_mappings_snapshot();
          for (size_t mi = 0; mi < mappings.size(); mi++) {
            const auto& pm = mappings[mi];
            ImGui::PushID(static_cast<int>(mi));
            ImGui::TableNextRow();
            ImVec4 const color = pm.user_override ? ImVec4(1, 1, 1, 1)
                                                  : ImVec4(0.6f, 0.6f, 0.6f, 1);

            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%d", pm.cpc_port);

            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%d", pm.host_port);

            ImGui::TableNextColumn();
            ImGui::TextColored(pm.active ? ImVec4(0.2f, 0.9f, 0.2f, 1)
                                         : ImVec4(0.5f, 0.5f, 0.5f, 1),
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
              if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove user override");
            }
            ImGui::PopID();
          }
          ImGui::EndTable();
        }

        // Add manual mapping row
        // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — &var passed to ImGui as a mutable in/out pointer
        static int new_cpc_port = 80;
        // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — &var passed to ImGui as a mutable in/out pointer
        static int new_host_port = 8080;
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt("##newcpc", &new_cpc_port, 0, 0);
        ImGui::SameLine();
        ImGui::TextDisabled("->");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt("##newhost", &new_host_port, 0, 0);
        ImGui::SameLine();
        if (ImGui::SmallButton("Add Mapping")) {
          if (new_cpc_port > 0 && new_cpc_port <= 65535 && new_host_port > 0 &&
              new_host_port <= 65535) {
            g_m4_http.set_port_mapping(static_cast<uint16_t>(new_cpc_port),
                                       static_cast<uint16_t>(new_host_port),
                                       true);
          }
        }
      }

      ImGui::EndTabItem();
    }

    // ── Serial Interface Tab ──
    if (ImGui::BeginTabItem("Serial Interface", nullptr,
                            s_pending_options_tab == OptionsTab::Serial
                                ? ImGuiTabItemFlags_SetSelected
                                : 0)) {
      SerialConfig cfg = g_serial_interface.get_config();
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      bool serial_en = cfg.enabled;

      if (ImGui::Checkbox("Enable Serial Interface", &serial_en)) {
        cfg.enabled = serial_en;
        g_serial_interface.set_config(cfg);
        g_serial_interface.apply_config();
      }

      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Serial Interface — AMSIf-compatible serial port emulation\n\n"
            "Emulates Z80 DART (Z8470) and Intel 8253 at ports $FADx/$FBDx.\n"
            "Supports backends: file, host serial, null modem, TCP, plotter.\n"
            "Plotter: use GSX driver DDHP7470.PRL on CPC to output SVG.");
      }

      ImGui::Separator();

      // Backend type selection
      const char* const backend_types[] = {
          "Null",       "File",       "Host Serial",
          "Null Modem", "TCP Socket", "HP-GL Plotter"};
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      int current_backend = static_cast<int>(cfg.backend_type);
      ImGui::SetNextItemWidth(150.0f);
      if (ImGui::Combo("Backend", &current_backend, backend_types,
                       IM_ARRAYSIZE(backend_types))) {
        cfg.backend_type = static_cast<SerialBackendType>(current_backend);
        g_serial_interface.set_config(cfg);
      }

      // Backend-specific options
      ImGui::Spacing();
      ImGui::TextDisabled("Connection Settings");

      switch (cfg.backend_type) {
        case SerialBackendType::File: {
          char input_buf[256] = {};
          char output_buf[256] = {};
          snprintf(input_buf, sizeof(input_buf), "%s", cfg.input_file.c_str());
          snprintf(output_buf, sizeof(output_buf), "%s",
                   cfg.output_file.c_str());

          ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
          if (ImGui::InputText("Input File##serial", input_buf,
                               sizeof(input_buf))) {
            cfg.input_file = input_buf;
          }
          ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
          if (ImGui::InputText("Output File##serial", output_buf,
                               sizeof(output_buf))) {
            cfg.output_file = output_buf;
          }
          break;
        }

        case SerialBackendType::HostSerial: {
          char device_buf[256] = {};
          snprintf(device_buf, sizeof(device_buf), "%s",
                   cfg.device_path.c_str());

          ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100.0f);
          if (ImGui::InputText("Device##serial", device_buf,
                               sizeof(device_buf))) {
            cfg.device_path = device_buf;
          }
          ImGui::SameLine();
          if (ImGui::Button("List Ports##serial")) {
            auto ports = HostSerialBackend::list_ports();
            if (ports.empty()) {
              ImGui::OpenPopup("No Serial Ports");
            } else {
              ImGui::OpenPopup("Select Serial Port");
            }
          }

          // Port selection popup
          if (ImGui::BeginPopup("Select Serial Port")) {
            auto ports = HostSerialBackend::list_ports();
            for (const auto& port : ports) {
              if (ImGui::Selectable(port.c_str())) {
                cfg.device_path = port;
              }
            }
            ImGui::EndPopup();
          }
          if (ImGui::BeginPopup("No Serial Ports")) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "No serial ports found");
            ImGui::EndPopup();
          }
          break;
        }

        case SerialBackendType::TcpSocket: {
          char host_buf[256] = {};
          snprintf(host_buf, sizeof(host_buf), "%s", cfg.tcp_host.c_str());

          ImGui::SetNextItemWidth(200.0f);
          if (ImGui::InputText("Host##serial", host_buf, sizeof(host_buf))) {
            cfg.tcp_host = host_buf;
          }
          ImGui::SameLine();
          int port = cfg.tcp_port;
          ImGui::SetNextItemWidth(80.0f);
          if (ImGui::InputInt("Port##serial", &port, 1, 100)) {
            port = std::max(port, 1);
            port = std::min(port, 65535);
            cfg.tcp_port = static_cast<uint16_t>(port);
          }
          break;
        }

        case SerialBackendType::NullModem:
          ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                             "Loopback mode - connect two emulated instances");
          break;

        case SerialBackendType::Null:
        default:
          ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                             "All data is dropped (null backend)");
          break;

        case SerialBackendType::Plotter:
          ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                             "HP 7470A plotter - outputs SVG");
          ImGui::Text("Enable and use GSX driver + DDHP7470.PRL on CPC.");
          break;
      }

      // Common settings
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("Serial Settings");

      const char* baud_items =
          "300\01200\02400\04800\09600\019200\038400\057600\0115200";
      int baud_idx = 4;  // default 9600
      int const baud = cfg.baud_rate;
      const int baud_rates[] = {300,   1200,  2400,  4800,  9600,
                                19200, 38400, 57600, 115200};
      for (int i = 0; i < 9; i++) {
        if (baud_rates[i] == baud) {
          baud_idx = i;
          break;
        }
      }
      ImGui::SetNextItemWidth(120.0f);
      if (ImGui::Combo("Baud Rate##serial", &baud_idx, baud_items)) {
        cfg.baud_rate = baud_rates[baud_idx];
      }

      // Apply button
      ImGui::Spacing();
      if (ImGui::Button("Apply Changes##serial")) {
        g_serial_interface.set_config(cfg);
        g_serial_interface.apply_config();
      }

      // Status
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("Status");
      if (g_serial_interface.backend) {
        ImGui::Text("Backend: %s", g_serial_interface.backend->name().c_str());
        ImGui::Text("Status: %s", g_serial_interface.backend->status().c_str());
        ImGui::Text("TX Empty: %s",
                    g_serial_interface.dart.tx_empty() ? "Yes" : "No");
        ImGui::Text("RX Available: %s",
                    g_serial_interface.dart.rx_available() ? "Yes" : "No");
      } else {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                           "Serial interface disabled");
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }
  // Deep-link consumed: the target tab (if any) has been selected this frame.
  s_pending_options_tab = OptionsTab::None;

  ImGui::Separator();
  ImGui::Spacing();

  // Bottom buttons
  if (ImGui::Button("Save", ImVec2(80, 0))) {
    std::string const cfg = getConfigurationFilename(true);
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
    cpc_resume();
    first_open = true;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Apply changes and save to config file");
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(80, 0))) {
    unsigned int const prev_style = CPC.scr_style;
    CPC = imgui_state.old_cpc_settings;
    CRTC.crtc_type = old_crtc_type;
    if (subcycle::Machine* m = subcycle_bridge_machine())
      m->set_crtc_type(static_cast<uint8_t>(old_crtc_type));
    g_m4board.enabled = old_m4_enabled;
    // Revert video plugin if it was changed live
    if (CPC.scr_style != prev_style) imgui_state.video_reinit_pending = true;
    imgui_state.show_options = false;
    cpc_resume();
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
    cpc_resume();
    first_open = true;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Apply changes for this session only\n(not saved to config file)");
  }

  if (!open) {
    // Window closed via X button — treat as Cancel
    unsigned int const prev_style = CPC.scr_style;
    CPC = imgui_state.old_cpc_settings;
    CRTC.crtc_type = old_crtc_type;
    if (subcycle::Machine* m = subcycle_bridge_machine())
      m->set_crtc_type(static_cast<uint8_t>(old_crtc_type));
    g_m4board.enabled = old_m4_enabled;
    if (CPC.scr_style != prev_style) imgui_state.video_reinit_pending = true;
    imgui_state.show_options = false;
    cpc_resume();
    first_open = true;
  }

  ImGui::End();
}
}  // namespace

// ─────────────────────────────────────────────────
// DevTools
// ─────────────────────────────────────────────────

// parse_hex, safe_read_word/dword moved to imgui_ui_testable.h

// Format memory line into stack buffer - zero heap allocations
// Buffer size: 512 bytes handles up to 64 bytes/line with all formats

// Shared poke input UI with proper validation
// Returns true if poke was executed
namespace {
bool ui_poke_input(char *addr_buf, size_t addr_size, char *val_buf,
                   // NOLINTNEXTLINE(misc-unused-parameters): parameter retained for API/callback signature stability
                   size_t val_size, const char *id_suffix) {
  ImGui::PushID(id_suffix);

  ImGui::SetNextItemWidth(50);
  ImGui::InputText("Addr", addr_buf, addr_size,
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(40);
  ImGui::InputText("Val", val_buf, val_size,
                   ImGuiInputTextFlags_CharsHexadecimal);
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
}  // namespace

namespace {
bool devtools_first_open = true;
}  // namespace

// A live numeric readout for the devtools bar whose tooltip stays put instead of
// flickering. Two causes are addressed:
//   1. The tooltip window is positioned at the cursor (BeginTooltipEx enforces
//      MousePos + offset); near a screen edge ImGui flips it so it lands
//      PARTIALLY UNDER THE POINTER. Once another window overlaps the item's
//      position, a plain IsItemHovered() returns false ("obstructed by another
//      window"), so the tooltip is skipped next frame, then reappears, then
//      hides… — the flicker. ImGuiHoveredFlags_AllowWhenOverlappedByWindow keeps
//      hover true while the cursor is inside the item's rect regardless of the
//      tooltip drawn on top of it.
//   2. The value's text width changes frame-to-frame as digits come and go; the
//      old TextUnformatted item rect jittered with it. We reserve a FIXED width
//      sized to `widest` (font-independent), draw the text into it, and
//      hover-test the constant-width box — stable like the fixed-label buttons
//      beside it (and it stops the whole bar shifting as the value changes).
// Returns true while hovered. Honors the caller's pushed ImGuiCol_Text colour.
namespace {
bool devbar_readout(const char* text, const char* widest) {
  const float box_w = ImGui::CalcTextSize(widest).x;
  const float frame_h = ImGui::GetFrameHeight();
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const ImVec2 text_size = ImGui::CalcTextSize(text);
  ImGui::GetWindowDrawList()->AddText(
      ImVec2(pos.x, pos.y + ((frame_h - text_size.y) * 0.5f)),
      ImGui::GetColorU32(ImGuiCol_Text), text);
  ImGui::Dummy(ImVec2(box_w, frame_h));  // constant-width item = stable hit-box
  return ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByWindow);
}
}  // namespace

namespace {
void imgui_render_devtools() {
  // Auto-open core windows on first DevTools open
  if (devtools_first_open) {
    if (!g_devtools_ui.any_window_open()) {
      g_devtools_ui.toggle_window("registers");
      g_devtools_ui.toggle_window("disassembly");
      g_devtools_ui.toggle_window("stack");
    }
    devtools_first_open = false;
  }

  ImGuiViewport const* vp = ImGui::GetMainViewport();
  float const bar_y =
      vp->Pos.y + s_menubar_h + static_cast<float>(s_main_topbar_h);

  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, bar_y));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, 0));  // auto-height
  ImGui::SetNextWindowViewport(vp->ID);             // keep on main viewport
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.11f, 1.0f));

  ImGuiWindowFlags const flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;

  if (ImGui::Begin("##devtools_bar", nullptr, flags)) {
    // ── Window dropdown buttons ──
    if (ImGui::Button("CPU")) ImGui::OpenPopup("##dt_cpu");
    if (ImGui::BeginPopup("##dt_cpu")) {
      ImGui::MenuItem("Registers", nullptr,
                      g_devtools_ui.window_ptr("registers"));
      ImGui::MenuItem("Disassembly", nullptr,
                      g_devtools_ui.window_ptr("disassembly"));
      ImGui::MenuItem("Stack", nullptr, g_devtools_ui.window_ptr("stack"));
      ImGui::MenuItem("Breakpoints/WP/IO", nullptr,
                      g_devtools_ui.window_ptr("breakpoints"));
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Memory")) ImGui::OpenPopup("##dt_mem");
    if (ImGui::BeginPopup("##dt_mem")) {
      ImGui::MenuItem("Memory Hex", nullptr,
                      g_devtools_ui.window_ptr("memory_hex"));
      ImGui::MenuItem("Data Areas", nullptr,
                      g_devtools_ui.window_ptr("data_areas"));
      ImGui::MenuItem("Symbols", nullptr, g_devtools_ui.window_ptr("symbols"));
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Hardware")) ImGui::OpenPopup("##dt_hw");
    if (ImGui::BeginPopup("##dt_hw")) {
      ImGui::MenuItem("Video State", nullptr,
                      g_devtools_ui.window_ptr("video_state"));
      ImGui::MenuItem("Audio State", nullptr,
                      g_devtools_ui.window_ptr("audio_state"));
      ImGui::MenuItem("Drive Sound Lab", nullptr,
                      g_devtools_ui.window_ptr("drive_sound_lab"));
      ImGui::MenuItem("ASIC Registers", nullptr,
                      g_devtools_ui.window_ptr("asic"));
      ImGui::MenuItem("Silicon Disc", nullptr,
                      g_devtools_ui.window_ptr("silicon_disc"));
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Media")) ImGui::OpenPopup("##dt_media");
    if (ImGui::BeginPopup("##dt_media")) {
      ImGui::MenuItem("Disc Tools", nullptr,
                      g_devtools_ui.window_ptr("disc_tools"));
      ImGui::MenuItem("Graphics Finder", nullptr,
                      g_devtools_ui.window_ptr("gfx_finder"));
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("ASM")) g_devtools_ui.toggle_window("assembler");

    // Layout controls reachable from where the windows are (beads-p0e/pv7):
    // a rescue for windows that drifted off-screen in Classic mode, plus the
    // docking presets that were previously only in the topbar Layout dropdown.
    ImGui::SameLine();
    if (ImGui::Button("Layout")) ImGui::OpenPopup("##dt_layout");
    if (ImGui::BeginPopup("##dt_layout")) {
      if (ImGui::MenuItem("Reset Window Positions")) {
        g_devtools_ui.reset_window_positions();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Apply Debug Layout")) {
        CPC.workspace_layout = t_CPC::WorkspaceLayoutMode::Docked;
        workspace_apply_preset(WorkspacePreset::Debug);
      }
      if (ImGui::MenuItem("Apply IDE Layout")) {
        CPC.workspace_layout = t_CPC::WorkspaceLayoutMode::Docked;
        workspace_apply_preset(WorkspacePreset::IDE);
      }
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Export")) ImGui::OpenPopup("##dt_export");
    if (ImGui::BeginPopup("##dt_export")) {
      ImGui::MenuItem("Disasm Export", nullptr,
                      g_devtools_ui.window_ptr("disasm_export"));
      ImGui::MenuItem("Session Recording", nullptr,
                      g_devtools_ui.window_ptr("session_recording"));
      ImGui::MenuItem("Recording Controls", nullptr,
                      g_devtools_ui.window_ptr("recording_controls"));
      ImGui::EndPopup();
    }

    // ── Vertical separator ──
    ImGui::SameLine(0, 12.0f);
    {
      ImVec2 const cur = ImGui::GetCursorScreenPos();
      float const h = ImGui::GetFrameHeight();
      ImGui::GetWindowDrawList()->AddLine(ImVec2(cur.x, cur.y + 2.0f),
                                          ImVec2(cur.x, cur.y + h - 2.0f),
                                          IM_COL32(128, 128, 128, 128), 1.0f);
      ImGui::Dummy(ImVec2(1.0f, h));
    }

    // ── Step/Pause controls ──
    // Capture paused state once so BeginDisabled/EndDisabled stay balanced
    // even when a button handler calls cpc_resume() mid-frame.
    ImGui::SameLine(0, 12.0f);
    // Use the atomic flag — CPC.paused is a plain bool written by the Z80
    // thread.
    bool const was_paused = g_emu_paused.load(std::memory_order_relaxed);
    if (!was_paused) ImGui::BeginDisabled();
    if (ImGui::Button("Step In")) dbg_step_in();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Execute one instruction, entering CALLs (F7)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Over")) dbg_step_over();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Execute one instruction, over CALLs/RSTs (Shift+F7)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Out")) dbg_step_out();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Run until the current subroutine returns (Shift+F11)");
    }
    if (!was_paused) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(was_paused ? "Resume" : "Pause")) {
      if (was_paused)
        cpc_resume();
      else
        cpc_pause();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run / halt the CPU (F5)");

    // Keyboard shortcuts for the debugger inner loop.  Active only when an
    // ImGui (DevTools) window holds keyboard focus and no text field is being
    // edited; the emulator skips these keys then (any_keyboard_ui_active), so
    // there is no conflict with the F-key emulator commands (beads-fa5).
    {
      ImGuiIO const& io = ImGui::GetIO();
      if (io.WantCaptureKeyboard && !io.WantTextInput) {
        if (was_paused) {
          if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F7))
            dbg_step_over();
          else if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F11))
            dbg_step_out();
          else if (ImGui::IsKeyPressed(ImGuiKey_F7, false))
            dbg_step_in();
          else if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
            cpc_resume();
        } else if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
          cpc_pause();
        }
      }
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
      if (devbar_readout(buf, "dt:9999.9ms")) {
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

    // ── Frame timing + audio diagnostics (--debug only) ──
    if (g_debug) {
      // Snapshot stats under the mutex — Z80 thread writes these once/second.
      // Take the snapshot first, then render from it (never hold the mutex
      // during ImGui calls).
      float s_frame_avg_ms, s_frame_min_ms, s_frame_max_ms;
      float s_z80_ms, s_disp_ms, s_sleep_ms;
      int s_underruns, s_near_underruns, s_pushes;
      float s_queue_avg_ms, s_queue_min_ms, s_push_interval_max_us;
      {
        std::scoped_lock const stats_lock(g_imgui_stats_mutex);
        s_frame_avg_ms = imgui_state.frame_time_avg_us / 1000.0f;
        s_frame_min_ms = imgui_state.frame_time_min_us / 1000.0f;
        s_frame_max_ms = imgui_state.frame_time_max_us / 1000.0f;
        s_z80_ms = imgui_state.z80_time_avg_us / 1000.0f;
        s_disp_ms = imgui_state.display_time_avg_us / 1000.0f;
        s_sleep_ms = imgui_state.sleep_time_avg_us / 1000.0f;
        s_underruns = imgui_state.audio_underruns;
        s_near_underruns = imgui_state.audio_near_underruns;
        s_pushes = imgui_state.audio_pushes;
        s_queue_avg_ms = imgui_state.audio_queue_avg_ms;
        s_queue_min_ms = imgui_state.audio_queue_min_ms;
        s_push_interval_max_us = imgui_state.audio_push_interval_max_us;
      }

      ImGui::SameLine(0, 8.0f);
      {
        float const budget_pct =
            s_frame_avg_ms / 20.0f * 100.0f;  // 20ms = 50fps budget
        ImVec4 const ft_color =
            budget_pct > 90.0f
                ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)   // red: >90% budget used
                : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);  // gray: healthy
        ImGui::PushStyleColor(ImGuiCol_Text, ft_color);
        char ftbuf[32];
        snprintf(ftbuf, sizeof(ftbuf), "frame:%.1fms", s_frame_avg_ms);
        if (devbar_readout(ftbuf, "frame:999.9ms")) {
          float const work_ms = s_frame_avg_ms - s_sleep_ms;
          ImGui::BeginTooltip();
          ImGui::Text("Frame time avg: %.1f ms (work: %.1f ms, sleep: %.1f ms)",
                      s_frame_avg_ms, work_ms, s_sleep_ms);
          ImGui::Text("Frame time min: %.1f ms  max: %.1f ms", s_frame_min_ms,
                      s_frame_max_ms);
          ImGui::Separator();
          ImGui::Text("Z80 emulation:  %.1f ms", s_z80_ms);
          ImGui::Text("Display/GL:     %.1f ms", s_disp_ms);
          ImGui::Text("Sleep (limiter):%.1f ms", s_sleep_ms);
          ImGui::EndTooltip();
        }
        ImGui::PopStyleColor();
      }

      // ── Audio diagnostics ──
      ImGui::SameLine(0, 8.0f);
      {
        ImVec4 snd_color;
        if (s_underruns > 0)
          snd_color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);  // red: hard underrun
        else if (s_near_underruns > 0)
          snd_color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);  // yellow: near-underrun
        else
          snd_color = ImVec4(0.4f, 0.4f, 0.4f, 1.0f);  // gray: healthy
        ImGui::PushStyleColor(ImGuiCol_Text, snd_color);
        char abuf[32];
        snprintf(abuf, sizeof(abuf), "snd:%.0fms", s_queue_avg_ms);
        if (devbar_readout(abuf, "snd:999ms")) {
          ImGui::BeginTooltip();
          ImGui::Text("Audio queue avg: %.1f ms", s_queue_avg_ms);
          ImGui::Text("Audio queue min: %.1f ms", s_queue_min_ms);
          ImGui::Text("Push interval max: %.0f us", s_push_interval_max_us);
          ImGui::Text("Pushes/sec: %d", s_pushes);
          ImGui::Text("Underruns/sec: %d", s_underruns);
          ImGui::Text("Near-underruns/sec: %d", s_near_underruns);
          ImGui::EndTooltip();
        }
        ImGui::PopStyleColor();
      }

    }  // g_debug

    // ── Sync devtools bar height ──
    {
      int const h = static_cast<int>(ImGui::GetWindowSize().y);
      if (h != s_devtools_bar_h) {
        s_devtools_bar_h = h;
        s_topbar_height_dirty = true;
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
}
}  // namespace

// ─────────────────────────────────────────────────
// Memory Tool
// ─────────────────────────────────────────────────

namespace {
void imgui_render_memory_tool() {
  ImGui::SetNextWindowSize(ImVec2(400, 340), ImGuiCond_FirstUseEver);

  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
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
  ImGui::InputText("Display##mt", imgui_state.mem_display_addr,
                   sizeof(imgui_state.mem_display_addr),
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
  const char* const bpl_items[] = {"1", "4", "8", "16", "32", "64"};
  int const bpl_values[] = {1, 4, 8, 16, 32, 64};
  int bpl_idx = 3;
  for (int i = 0; i < 6; i++) {
    if (bpl_values[i] == imgui_state.mem_bytes_per_line) bpl_idx = i;
  }
  ImGui::SetNextItemWidth(60);
  if (ImGui::Combo("Bytes/Line##mt", &bpl_idx, bpl_items, 6)) {
    imgui_state.mem_bytes_per_line = bpl_values[bpl_idx];
  }

  // Filter
  ImGui::SetNextItemWidth(40);
  ImGui::InputText("Filter Byte##mt", imgui_state.mem_filter_val,
                   sizeof(imgui_state.mem_filter_val),
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
    int const bpl = imgui_state.mem_bytes_per_line;
    for (unsigned int i = 0; i < 65536u / bpl; i++) {
      std::cout << std::uppercase << std::setfill('0') << std::setw(4)
                << std::hex << i * bpl << " : ";
      for (int j = 0; j < bpl; j++) {
        std::cout << std::setw(2)
                  << static_cast<unsigned int>(pbRAM[(i * bpl) + j]) << " ";
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
    if (ImGui::SmallButton("Clear##mtclear")) {
      imgui_state.mem_filter_value = -1;
    }
  } else if (imgui_state.mem_display_value >= 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
    ImGui::Text("[DISPLAY: %04X]", imgui_state.mem_display_value & 0xFFFF);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##mtclear")) {
      imgui_state.mem_display_value = -1;
    }
  }

  // Hex dump
  if (ImGui::BeginChild("##mtmem", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    int const bpl = imgui_state.mem_bytes_per_line;
    int const total_lines = 65536 / bpl;
    bool const filtering = imgui_state.mem_filter_value >= 0 &&
                           imgui_state.mem_filter_value <= 255;
    bool const displaying = imgui_state.mem_display_value >= 0 &&
                            imgui_state.mem_display_value <= 0xFFFF;

    if (filtering || displaying) {
      // Can't use clipper with filtering — iterate all
      for (int i = 0; i < total_lines; i++) {
        unsigned int const base = i * bpl;
        bool show = false;
        if (!filtering && !displaying) {
          show = true;
        }
        if (displaying) {
          if (base <=
                  static_cast<unsigned int>(imgui_state.mem_display_value) &&
              static_cast<unsigned int>(imgui_state.mem_display_value) <
                  base + bpl)
            show = true;
        }
        if (filtering) {
          for (int j = 0; j < bpl; j++) {
            if (pbRAM[(base + j) & 0xFFFF] == imgui_state.mem_filter_value) {
              show = true;
              break;
            }
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
          unsigned int const base = i * bpl;
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
}  // namespace

// ─────────────────────────────────────────────────
// Virtual Keyboard – CPC 6128 layout
// Main keyboard left, numeric keypad (F0-F9) right, cursor keys below numpad
// ─────────────────────────────────────────────────

namespace {
void imgui_render_vkeyboard() {
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(575, 265), ImGuiCond_FirstUseEver);

  if (!ImGui::Begin("CPC 6128 Keyboard", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) imgui_state.show_vkeyboard = false;
    return;
  }

  // Key dimensions
  const float K = 28.0f;    // standard key width
  const float H = 32.0f;    // key height (taller for two-line labels)
  const float S = 2.0f;     // spacing
  const float ROW = H + S;  // row height

  // CPC brown/tan key color
  ImVec4 const key_color(0.55f, 0.45f, 0.30f, 1.0f);
  ImVec4 const key_hover(0.65f, 0.55f, 0.40f, 1.0f);
  ImVec4 const key_active(0.45f, 0.35f, 0.20f, 1.0f);
  ImVec4 const mod_on_color(0.3f, 0.5f, 0.3f, 1.0f);

  ImGui::PushStyleColor(ImGuiCol_Button, key_color);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, key_hover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, key_active);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

  bool const caps_on = imgui_state.vkeyboard_caps_lock;
  bool shift_on =
      imgui_state.vkeyboard_shift_next || caps_on;  // either gives shift effect
  bool ctrl_on = imgui_state.vkeyboard_ctrl_next;

  // Shift mapping for non-letter keys
  static std::map<char, char> shift_map = {
      {'1', '!'}, {'2', '"'},    {'3', '#'},  {'4', '$'}, {'5', '%'},
      {'6', '&'}, {'7', '\''},   {'8', '('},  {'9', ')'}, {'0', '_'},
      {'-', '='}, {'^', '\xa3'},  // £ in Latin-1
      {';', '+'}, {':', '*'},    {'[', '{'},  {']', '}'}, {',', '<'},
      {'.', '>'}, {'/', '?'},    {'\\', '`'}, {'@', '|'}};

  // Helper to emit a key - sends directly to emulator
  auto emit_key = [&](const char* text) {
    if (!text) return;
    std::string to_send = text;

    // SHIFT toggle (one-shot)
    if (strcmp(text,
               "\x01"
               "SHIFT") == 0) {
      imgui_state.vkeyboard_shift_next = !imgui_state.vkeyboard_shift_next;
      return;
    }
    // CAPS LOCK toggle (sticky)
    if (strcmp(text,
               "\x01"
               "CAPS") == 0) {
      imgui_state.vkeyboard_caps_lock = !imgui_state.vkeyboard_caps_lock;
      return;
    }
    // CTRL toggle (one-shot)
    if (strcmp(text,
               "\x01"
               "CTRL") == 0) {
      imgui_state.vkeyboard_ctrl_next = !imgui_state.vkeyboard_ctrl_next;
      return;
    }

    // Apply CTRL modifier
    if (ctrl_on && to_send.length() == 1) {
      char const c = to_send[0];
      if (c >= 'a' && c <= 'z') {
        to_send = std::string("\a") + static_cast<char>(CPC_CTRL_a + (c - 'a'));
      } else if (c >= '0' && c <= '9') {
        to_send = std::string("\a") + static_cast<char>(CPC_CTRL_0 + (c - '0'));
      }
      imgui_state.vkeyboard_ctrl_next = false;  // one-shot
    }
    // Apply SHIFT modifier
    else if (shift_on && to_send.length() == 1) {
      char const c = to_send[0];
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
  if (caps_on) {
    ImGui::TextColored(ImVec4(0, 1, 0.5f, 1), "[CAPS]");
    ImGui::SameLine();
  }
  if (imgui_state.vkeyboard_shift_next) {
    ImGui::TextColored(ImVec4(0.5f, 1, 0, 1), "[SHIFT]");
    ImGui::SameLine();
  }
  if (ctrl_on) {
    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "[CTRL]");
    ImGui::SameLine();
  }
  ImGui::NewLine();

  float const x0 = ImGui::GetCursorPosX();
  float const y0 = ImGui::GetCursorPosY();

  // Helper: check if a CPC key is currently pressed in the keyboard matrix.
  // CPC_KEYS enum values are NOT matrix positions — must convert via scancode
  // table.
  auto cpc_key_down = [](byte cpc_key) -> bool {
    CPCScancode const sc =
        CPC.InputMapper->CPCscancodeFromCPCkey(static_cast<CPC_KEYS>(cpc_key));
    byte const row = static_cast<byte>(sc >> 4);
    byte const bit = static_cast<byte>(sc & 7);
    return row < 16 && !(keyboard_matrix[row].load(std::memory_order_relaxed) &
                         bit_values[bit]);
  };
  // Helper: draw blue overlay on last ImGui item if CPC key is pressed
  auto highlight_if_pressed = [&](byte cpc_key) {
    if (cpc_key_down(cpc_key)) {
      ImVec2 const rmin = ImGui::GetItemRectMin();
      ImVec2 const rmax = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddRectFilled(
          rmin, rmax, IM_COL32(80, 180, 255, 180), 3.0f);
    }
  };

  // Width multipliers for special keys
  const float W_TAB = 1.3f;
  const float W_CAPS = 1.4f;
  const float W_LSHIFT = 1.9f;
  const float W_CTRL = 1.5f;
  const float W_COPY = 1.6f;
  // RETURN, right SHIFT, ENTER widths calculated dynamically to align right
  // edges

  // Calculate main keyboard right edge: 15 standard keys + ESC in row 0
  // ESC 1 2 3 4 5 6 7 8 9 0 - ^/£ CLR DEL = 15 keys
  float const main_end_x = x0 + ((K + S) * 15) - S;  // right edge of DEL

  // Numpad starts after a gap from main keyboard right edge
  float const np_x = main_end_x + (S * 4);

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
  auto vk_cpc = [&](const char* label, float w, byte cpc_key,
                    bool end = false) {
    std::string const es = cpc_emit(cpc_key);
    if (ImGui::Button(label, ImVec2(w, H))) emit_key(es.c_str());
    highlight_if_pressed(cpc_key);
    if (!end) ImGui::SameLine(0, S);
  };

  // ═══════════════════ ROW 0 ═══════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0));
  vk("ESC", K, cpc_emit(CPC_ESC).c_str(), CPC_ESC);
  vk("!\n1", K, "1", CPC_1);
  vk("\"\n2", K, "2", CPC_2);
  vk("#\n3", K, "3", CPC_3);
  vk("$\n4", K, "4", CPC_4);
  vk("%\n5", K, "5", CPC_5);
  vk("&\n6", K, "6", CPC_6);
  vk("'\n7", K, "7", CPC_7);
  vk("(\n8", K, "8", CPC_8);
  vk(")\n9", K, "9", CPC_9);
  vk("_\n0", K, "0", CPC_0);
  vk("=\n-", K, "-", CPC_MINUS);
  vk("\xc2\xa3\n^", K, "^", CPC_POWER);
  vk("CLR", K, cpc_emit(CPC_CLR).c_str(), CPC_CLR);
  vk_end("DEL", K, "\b", CPC_DEL);
  // Numpad
  ImGui::SetCursorPos(ImVec2(np_x, y0));
  vk_cpc("F7", K, CPC_F7);
  vk_cpc("F8", K, CPC_F8);
  vk_cpc("F9", K, CPC_F9, true);

  // ═══════════════════ ROW 1 ═══════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + ROW));
  vk("TAB", K * W_TAB, "\t", CPC_TAB);
  vk("Q", K, "q", CPC_Q);
  vk("W", K, "w", CPC_W);
  vk("E", K, "e", CPC_E);
  vk("R", K, "r", CPC_R);
  vk("T", K, "t", CPC_T);
  vk("Y", K, "y", CPC_Y);
  vk("U", K, "u", CPC_U);
  vk("I", K, "i", CPC_I);
  vk("O", K, "o", CPC_O);
  vk("P", K, "p", CPC_P);
  vk("|\n@", K, "@", CPC_AT);
  vk("{\n[", K, "[", CPC_LBRACKET);
  // RETURN upper part — fills to main_end_x
  float const ret_x = ImGui::GetCursorPosX();
  float const ret_w = main_end_x - ret_x;
  vk_end("RETURN##1", ret_w, "\n", CPC_RETURN);
  // RETURN lower part (L-shape into row 2)
  float const ret2_x = x0 + (K * W_CAPS) + S + (12 * (K + S));
  float const ret2_w = main_end_x - ret2_x;
  ImGui::SetCursorPos(ImVec2(ret2_x, y0 + ROW + H));
  if (ImGui::Button("##ret2", ImVec2(ret2_w, ROW))) emit_key("\n");
  highlight_if_pressed(CPC_RETURN);
  // Numpad
  ImGui::SetCursorPos(ImVec2(np_x, y0 + ROW));
  vk_cpc("F4", K, CPC_F4);
  vk_cpc("F5", K, CPC_F5);
  vk_cpc("F6", K, CPC_F6, true);

  // ═══════════════════ ROW 2 ═══════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + (ROW * 2)));
  if (caps_on) ImGui::PushStyleColor(ImGuiCol_Button, mod_on_color);
  if (ImGui::Button("CAPS\nLOCK", ImVec2(K * W_CAPS, H)))
    emit_key(
        "\x01"
        "CAPS");
  if (caps_on) ImGui::PopStyleColor();
  highlight_if_pressed(CPC_CAPSLOCK);
  ImGui::SameLine(0, S);
  vk("A", K, "a", CPC_A);
  vk("S", K, "s", CPC_S);
  vk("D", K, "d", CPC_D);
  vk("F", K, "f", CPC_F);
  vk("G", K, "g", CPC_G);
  vk("H", K, "h", CPC_H);
  vk("J", K, "j", CPC_J);
  vk("K", K, "k", CPC_K);
  vk("L", K, "l", CPC_L);
  vk("+\n;", K, ";", CPC_SEMICOLON);
  vk("*\n:", K, ":", CPC_COLON);
  vk_end("}\n]", K, "]", CPC_RBRACKET);
  // Numpad
  ImGui::SetCursorPos(ImVec2(np_x, y0 + (ROW * 2)));
  vk_cpc("F1", K, CPC_F1);
  vk_cpc("F2", K, CPC_F2);
  vk_cpc("F3", K, CPC_F3, true);

  // ═══════════════════ ROW 3 ═══════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + (ROW * 3)));
  bool const shift_highlight = imgui_state.vkeyboard_shift_next;
  if (shift_highlight) ImGui::PushStyleColor(ImGuiCol_Button, mod_on_color);
  if (ImGui::Button("SHIFT##L", ImVec2(K * W_LSHIFT, H)))
    emit_key(
        "\x01"
        "SHIFT");
  if (shift_highlight) ImGui::PopStyleColor();
  highlight_if_pressed(CPC_LSHIFT);
  ImGui::SameLine(0, S);
  vk("Z", K, "z", CPC_Z);
  vk("X", K, "x", CPC_X);
  vk("C", K, "c", CPC_C);
  vk("V", K, "v", CPC_V);
  vk("B", K, "b", CPC_B);
  vk("N", K, "n", CPC_N);
  vk("M", K, "m", CPC_M);
  vk("<\n,", K, ",", CPC_COMMA);
  vk(">\n.##main", K, ".", CPC_PERIOD);
  vk("?\n/", K, "/", CPC_SLASH);
  vk("`\n\\", K, "\\", CPC_BACKSLASH);
  // Right SHIFT — fills to main_end_x
  float const rshift_x = ImGui::GetCursorPosX();
  float const rshift_w = main_end_x - rshift_x;
  if (shift_highlight) ImGui::PushStyleColor(ImGuiCol_Button, mod_on_color);
  if (ImGui::Button("SHIFT##R", ImVec2(rshift_w, H)))
    emit_key(
        "\x01"
        "SHIFT");
  if (shift_highlight) ImGui::PopStyleColor();
  highlight_if_pressed(CPC_RSHIFT);
  // Numpad
  ImGui::SetCursorPos(ImVec2(np_x, y0 + (ROW * 3)));
  vk_cpc("F0", K, CPC_F0);
  vk("\xe2\x86\x91##up", K, cpc_emit(CPC_CUR_UP).c_str(), CPC_CUR_UP);
  vk_end(".##np", K, ".", CPC_FPERIOD);

  // ═══════════════════ ROW 4 ═══════════════════
  ImGui::SetCursorPos(ImVec2(x0, y0 + (ROW * 4)));
  if (ctrl_on)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.4f, 0.2f, 1.0f));
  if (ImGui::Button("CTRL", ImVec2(K * W_CTRL, H)))
    emit_key(
        "\x01"
        "CTRL");
  if (ctrl_on) ImGui::PopStyleColor();
  highlight_if_pressed(CPC_CONTROL);
  ImGui::SameLine(0, S);
  vk("COPY", K * W_COPY, cpc_emit(CPC_COPY).c_str(), CPC_COPY);
  float const space_w = K * 8.0f;
  vk("SPACE", space_w, " ", CPC_SPACE);
  float const enter_x = ImGui::GetCursorPosX();
  float const enter_w = main_end_x - enter_x;
  vk_end("ENTER", enter_w, "\n", CPC_ENTER);
  // Numpad
  ImGui::SetCursorPos(ImVec2(np_x, y0 + (ROW * 4)));
  vk("\xe2\x86\x90##left", K, cpc_emit(CPC_CUR_LEFT).c_str(), CPC_CUR_LEFT);
  vk("\xe2\x86\x93##down", K, cpc_emit(CPC_CUR_DOWN).c_str(), CPC_CUR_DOWN);
  vk_end("\xe2\x86\x92##right", K, cpc_emit(CPC_CUR_RIGHT).c_str(),
         CPC_CUR_RIGHT);

  // Move cursor below keyboard for the rest
  ImGui::SetCursorPos(ImVec2(x0, y0 + (ROW * 5) + (S * 2)));

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
}  // namespace

// ─────────────────────────────────────────────────
// Virtual Joystick — on-screen 8-way pad + Fire1/Fire2, targets CPC joy 0/1
// ─────────────────────────────────────────────────

// Write a CPC_J{n}_* key into keyboard-matrix row 9/6.  Uses the direct matrix
// path (like the IPC `input joy` handler) so it still works while the emulator
// is paused — the F1 menu / options dialogs pause the CPC.
namespace {
void vjoy_actuate(CPC_KEYS key, bool pressed) {
  CPCScancode const sc = CPC.InputMapper->CPCscancodeFromCPCkey(key);
  applyKeypressDirect(sc, keyboard_matrix, pressed);
}
}  // namespace

// Release every bit a previously-held mask asserted for a given target.
namespace {
void vjoy_release_mask(int target, unsigned mask) {
  vjoy::VJoyKeys const k = vjoy::vjoy_active_keys(target, mask);
  for (int i = 0; i < k.count; i++) vjoy_actuate(k.keys[i], false);
}
}  // namespace

namespace {
void imgui_render_vjoystick() {
  // Always ticks (called unconditionally) so that when the window is closed we
  // can release any bits still held from the last frame.
  if (!imgui_state.show_vjoystick) {
    imgui_state.vjoystick_focused = false;
    if (imgui_state.vjoystick_held_mask) {
      vjoy_release_mask(imgui_state.vjoystick_held_target,
                        imgui_state.vjoystick_held_mask);
      imgui_state.vjoystick_held_mask = 0;
    }
    return;
  }

  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(250, 240), ImGuiCond_FirstUseEver);
  // NoNav: arrow keys are read for the joystick (below), not consumed by ImGui's
  // keyboard navigation between the d-pad buttons.
  if (!ImGui::Begin("Virtual Joystick", &open,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav)) {
    imgui_state.vjoystick_focused = false;
    ImGui::End();
    if (!open) imgui_state.show_vjoystick = false;
    return;
  }

  // Target selector (CPC joystick 0 or 1).
  int target = static_cast<int>(imgui_state.vjoystick_target);
  ImGui::TextUnformatted("Target:");
  ImGui::SameLine();
  if (ImGui::RadioButton("Joy 0", target == 0)) {
    imgui_state.vjoystick_target = ImGuiUIState::VJoyTarget::Joy0;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Joy 1", target == 1)) {
    imgui_state.vjoystick_target = ImGuiUIState::VJoyTarget::Joy1;
  }
  target = static_cast<int>(imgui_state.vjoystick_target);
  ImGui::Separator();

  using namespace vjoy;

  // While THIS is the focused window, the host arrows + space also drive the
  // target joystick — and ONLY then (imgui_any_keyboard_ui_active() reports a
  // focused vjoy window as keyboard-capturing, so these keys don't also reach the
  // CPC keyboard). Compute the keyboard-held bits UP FRONT so the on-screen
  // buttons can render as pressed too.
  const bool focused =
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  imgui_state.vjoystick_focused = focused;
  unsigned kbd = 0;
  if (focused) {
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) kbd |= VJOY_UP;
    if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) kbd |= VJOY_DOWN;
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) kbd |= VJOY_LEFT;
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) kbd |= VJOY_RIGHT;
    if (ImGui::IsKeyDown(ImGuiKey_Space)) kbd |= VJOY_FIRE1;
  }

  // Collect held buttons: mouse (IsItemActive) OR keyboard (kbd). A button whose
  // bits are all keyboard-held is drawn in the "active" colour so the press shows
  // on screen.
  unsigned mask = 0;
  auto pad_btn = [&](const char* label, unsigned bit, float w, float h) {
    const bool forced = bit != 0 && (kbd & bit) == bit;
    if (forced)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::Button(label, ImVec2(w, h));
    if (forced) ImGui::PopStyleColor();
    if (forced || ImGui::IsItemActive()) mask |= bit;
  };

  const float B = 52.0f;  // d-pad cell size
  const float S = 4.0f;   // spacing
  // 3x3 d-pad grid: diagonals fill the corners.
  float x0 = ImGui::GetCursorPosX();
  auto row = [&](const char* a, unsigned am, const char* b, unsigned bm,
                 const char* c, unsigned cm) {
    ImGui::SetCursorPosX(x0);
    pad_btn(a, am, B, B);
    ImGui::SameLine(0, S);
    pad_btn(b, bm, B, B);
    ImGui::SameLine(0, S);
    pad_btn(c, cm, B, B);
  };
  row("\xE2\x86\x96", VJOY_UP | VJOY_LEFT, "\xE2\x86\x91", VJOY_UP,
      "\xE2\x86\x97", VJOY_UP | VJOY_RIGHT);  // ↖ ↑ ↗
  row("\xE2\x86\x90", VJOY_LEFT, "\xC2\xB7", 0u, "\xE2\x86\x92",
      VJOY_RIGHT);  // ← · →
  row("\xE2\x86\x99", VJOY_DOWN | VJOY_LEFT, "\xE2\x86\x93", VJOY_DOWN,
      "\xE2\x86\x98", VJOY_DOWN | VJOY_RIGHT);  // ↙ ↓ ↘

  ImGui::Spacing();
  pad_btn("Fire 1", VJOY_FIRE1, (B * 1.5f) + (S / 2), B * 0.8f);
  ImGui::SameLine(0, S);
  pad_btn("Fire 2", VJOY_FIRE2, (B * 1.5f) + (S / 2), B * 0.8f);

  ImGui::Spacing();
  ImGui::TextDisabled("%s", focused ? "Arrows + Space drive the joystick"
                                    : "Click this window for Arrows + Space");

  // Apply the delta against what we held last frame.  Delta (not full rewrite)
  // so an external source (gamepad/keyboard) driving the same joystick isn't
  // clobbered every frame.
  if (target != imgui_state.vjoystick_held_target &&
      imgui_state.vjoystick_held_mask) {
    // Target switched — release the old target's held bits first.
    vjoy_release_mask(imgui_state.vjoystick_held_target,
                      imgui_state.vjoystick_held_mask);
    imgui_state.vjoystick_held_mask = 0;
  }
  imgui_state.vjoystick_held_target = target;
  unsigned const prev = imgui_state.vjoystick_held_mask;
  unsigned const added = mask & ~prev;
  unsigned const removed = prev & ~mask;
  if (added) {
    VJoyKeys const k = vjoy_active_keys(target, added);
    for (int i = 0; i < k.count; i++) vjoy_actuate(k.keys[i], true);
  }
  if (removed) vjoy_release_mask(target, removed);
  imgui_state.vjoystick_held_mask = mask;

  if (!open) imgui_state.show_vjoystick = false;
  ImGui::End();
}
}  // namespace

// ─────────────────────────────────────────────────
// Serial Terminal Window
// ─────────────────────────────────────────────────

namespace {
struct SerialTerminalState {
  static constexpr int BUFFER_SIZE = 4096;
  std::vector<uint8_t> tx_buffer;
  std::vector<uint8_t> rx_buffer;
  bool auto_scroll = true;
  bool show_hex = true;
  bool show_ascii = true;
  bool wrap_lines = true;
  uint8_t filter_byte = 0;
  bool use_filter = false;
  char input_buf[256] = "";
};
}  // namespace

namespace {
SerialTerminalState s_serial_term;
}  // namespace

namespace {
void render_hex_line(const uint8_t* data, int len) {
  char hex[64] = {};
  char ascii[17] = {};
  int j = 0;
  for (int i = 0; i < len && i < 16; i++) {
    snprintf(hex + j, sizeof(hex) - j, "%02X ", data[i]);
    j += 3;
    ascii[i] = (data[i] >= 32 && data[i] < 127) ? data[i] : '.';
  }
  ascii[len < 16 ? len : 16] = '\0';
  ImGui::Text("%-48s %s", hex, ascii);
}
}  // namespace

void serial_terminal_feed_byte(uint8_t byte) {
  if (s_serial_term.rx_buffer.size() < SerialTerminalState::BUFFER_SIZE) {
    s_serial_term.rx_buffer.push_back(byte);
  }
}

void imgui_render_serial_terminal() {
  ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Serial Terminal", &imgui_state.show_serial_terminal)) {
    ImGui::End();
    return;
  }

  // Status bar
  ImGui::Text("DART: TX=%s RX=%s | Backend: %s",
              g_serial_interface.dart.tx_empty() ? "Empty" : "Busy",
              g_serial_interface.dart.rx_available() ? "Data" : "Empty",
              g_serial_interface.backend
                  ? g_serial_interface.backend->name().c_str()
                  : "None");

  ImGui::Separator();

  // Controls
  ImGui::Checkbox("Auto-scroll", &s_serial_term.auto_scroll);
  ImGui::SameLine();
  ImGui::Checkbox("Hex", &s_serial_term.show_hex);
  ImGui::SameLine();
  ImGui::Checkbox("ASCII", &s_serial_term.show_ascii);
  ImGui::SameLine();
  ImGui::Checkbox("Wrap", &s_serial_term.wrap_lines);
  ImGui::SameLine();

  ImGui::PushButtonRepeat(true);
  if (ImGui::SmallButton("Clear")) {
    s_serial_term.rx_buffer.clear();
    s_serial_term.tx_buffer.clear();
  }
  ImGui::PopButtonRepeat();

  ImGui::Separator();

  // Data is fed via serial_terminal_feed_byte() from the DART TX callback
  // (see apply_config in serial_interface.cpp) — no direct backend polling
  // here.

  // Data display
  {
    ImGui::BeginChild("##serial_data",
                      ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 30),
                      true);

    // Display RX buffer
    if (s_serial_term.show_hex) {
      int line_start = 0;
      while (line_start < static_cast<int>(s_serial_term.rx_buffer.size())) {
        int const line_len = std::min(
            16, static_cast<int>(s_serial_term.rx_buffer.size()) - line_start);
        render_hex_line(&s_serial_term.rx_buffer[line_start], line_len);
        line_start += 16;
      }
    }

    if (s_serial_term.show_ascii) {
      ImGui::Spacing();
      ImGui::TextDisabled("ASCII View:");
      // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
      std::string ascii_str;
      for (uint8_t const b : s_serial_term.rx_buffer) {
        char const c = (b >= 32 && b < 127) ? b : '.';
        ascii_str += c;
      }
      ImGui::TextWrapped("%s", ascii_str.c_str());
    }

    if (s_serial_term.auto_scroll) {
      ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
  }

  // Input area
  ImGui::Separator();
  ImGui::Text("TX Buffer: %zu bytes", s_serial_term.tx_buffer.size());

  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80.0f);
  if (ImGui::InputText("##serial_input", s_serial_term.input_buf,
                       sizeof(s_serial_term.input_buf),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    // Send input
    for (const char* p = s_serial_term.input_buf; *p; p++) {
      if (s_serial_term.tx_buffer.size() < SerialTerminalState::BUFFER_SIZE) {
        s_serial_term.tx_buffer.push_back(*p);
        // Send to backend
        if (g_serial_interface.backend) {
          // This would send to the DART, which then calls backend->send()
          g_serial_interface.dart.enqueue_rx(*p);  // Echo locally for now
        }
      }
    }
    s_serial_term.input_buf[0] = '\0';
  }
  ImGui::SameLine();
  if (ImGui::Button("Send", ImVec2(60, 0))) {
    for (const char* p = s_serial_term.input_buf; *p; p++) {
      if (s_serial_term.tx_buffer.size() < SerialTerminalState::BUFFER_SIZE) {
        s_serial_term.tx_buffer.push_back(*p);
      }
    }
    s_serial_term.input_buf[0] = '\0';
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("0D")) {
    strncat(
        s_serial_term.input_buf, "\r",
        sizeof(s_serial_term.input_buf) - strlen(s_serial_term.input_buf) - 1);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("0A")) {
    strncat(
        s_serial_term.input_buf, "\n",
        sizeof(s_serial_term.input_buf) - strlen(s_serial_term.input_buf) - 1);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("1B")) {
    strncat(
        s_serial_term.input_buf, "\x1B",
        sizeof(s_serial_term.input_buf) - strlen(s_serial_term.input_buf) - 1);
  }

  ImGui::End();
}

// ─────────────────────────────────────────────────
// Plotter Preview Window
// ─────────────────────────────────────────────────

void imgui_render_plotter_preview() {
  ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Plotter Preview", &imgui_state.show_plotter_preview)) {
    ImGui::End();
    return;
  }

  const auto& segments = plotter_view_segments();
  const PlotterViewStatus pv = plotter_view_status();

  // Toolbar
  ImGui::Text("Segments: %zu | Pen: %d %s | Pos: %.0f, %.0f", segments.size(),
              pv.pen, pv.pen_down ? "DOWN" : "UP", pv.x, pv.y);

  ImGui::SameLine(ImGui::GetContentRegionAvail().x - 180.0f);
  if (ImGui::SmallButton("Export SVG...")) {
    static const SDL_DialogFileFilter filters[] = {{"SVG files", "svg"}};
    SDL_ShowSaveFileDialog(file_dialog_callback,
                           // NOLINTNEXTLINE(performance-no-int-to-ptr): intentional integer/pointer reinterpret for hardware/opaque handles
                           reinterpret_cast<void*>(static_cast<intptr_t>(
                               FileDialogAction::SavePlotterSVG)),
                           mainSDLWindow, filters, 1, "plotter_output.svg");
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear")) {
    plotter_view_clear();
  }

  ImGui::Separator();

  // Drawing canvas
  ImVec2 const canvas_pos = ImGui::GetCursorScreenPos();
  ImVec2 canvas_size = ImGui::GetContentRegionAvail();
  canvas_size.x = std::max(canvas_size.x, 50.0f);
  canvas_size.y = std::max(canvas_size.y, 50.0f);

  // Aspect ratio of HP 7470A paper (A4 landscape)
  float const paper_aspect = static_cast<float>(HPGL_MAX_X) / HPGL_MAX_Y;
  float const canvas_aspect = canvas_size.x / canvas_size.y;
  float scale, ox, oy;
  if (canvas_aspect > paper_aspect) {
    scale = canvas_size.y / HPGL_MAX_Y;
    ox = canvas_pos.x + ((canvas_size.x - (HPGL_MAX_X * scale)) * 0.5f);
    oy = canvas_pos.y;
  } else {
    scale = canvas_size.x / HPGL_MAX_X;
    ox = canvas_pos.x;
    oy = canvas_pos.y + ((canvas_size.y - (HPGL_MAX_Y * scale)) * 0.5f);
  }
  // FBO-relative offsets (same values minus canvas_pos origin).
  const float fbo_ox = ox - canvas_pos.x;
  const float fbo_oy = oy - canvas_pos.y;

  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Try to render segments into a cached FBO texture (only redraws when
  // segment count or canvas size changes — avoids per-frame iteration).
  const int canvas_w = static_cast<int>(canvas_size.x);
  const int canvas_h = static_cast<int>(canvas_size.y);
  uintptr_t const fbo_tex = video_offscreen_texture(
      "plotter", canvas_w, canvas_h, segments.size(),
      [&segments, fbo_ox, fbo_oy, scale](ImDrawList* fdl, int /*w*/,
                                         int /*h*/) {
        const ImVec2 fp0(fbo_ox, fbo_oy);
        const ImVec2 fp1(fbo_ox + (HPGL_MAX_X * scale),
                         fbo_oy + (HPGL_MAX_Y * scale));
        fdl->AddRectFilled(fp0, fp1, IM_COL32(255, 255, 255, 255));
        fdl->AddRect(fp0, fp1, IM_COL32(180, 180, 180, 255));

        auto pen_col = [](int pen) -> ImU32 {
          return (pen == 2) ? IM_COL32(200, 0, 0, 255) : IM_COL32(0, 0, 0, 255);
        };
        for (const auto& seg : segments) {
          ImU32 const col = pen_col(seg.pen);
          switch (seg.type) {
            case PlotPrimitive::Line: {
              ImVec2 const a(fbo_ox + (seg.x1 * scale),
                             fbo_oy + ((HPGL_MAX_Y - seg.y1) * scale));
              ImVec2 const b(fbo_ox + (seg.x2 * scale),
                             fbo_oy + ((HPGL_MAX_Y - seg.y2) * scale));
              fdl->AddLine(a, b, col, 1.5f);
              break;
            }
            case PlotPrimitive::Circle: {
              ImVec2 const c(fbo_ox + (seg.x1 * scale),
                             fbo_oy + ((HPGL_MAX_Y - seg.y1) * scale));
              fdl->AddCircle(c, seg.radius * scale, col, 64, 1.5f);
              break;
            }
            case PlotPrimitive::Arc: {
              ImVec2 const c(fbo_ox + (seg.x1 * scale),
                             fbo_oy + ((HPGL_MAX_Y - seg.y1) * scale));
              float sa = -seg.start_angle * (3.14159265f / 180.0f);
              float ea =
                  -(seg.start_angle + seg.sweep_angle) * (3.14159265f / 180.0f);
              if (sa > ea) std::swap(sa, ea);
              fdl->PathArcTo(c, seg.radius * scale, sa, ea, 64);
              fdl->PathStroke(col, 0, 1.5f);
              break;
            }
            case PlotPrimitive::Label: {
              ImVec2 const pos(fbo_ox + (seg.x1 * scale),
                               fbo_oy + ((HPGL_MAX_Y - seg.y1) * scale));
              fdl->AddText(pos, col, seg.text.c_str());
              break;
            }
          }
        }
      });

  if (fbo_tex) {
    // Display the cached texture (Y-flipped UV: FBO is stored bottom-up in GL).
    dl->AddImage(
        static_cast<ImTextureID>(fbo_tex), canvas_pos,
        ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
        ImVec2(0, 1), ImVec2(1, 0));
  } else {
    // SDL_Renderer fallback: draw per-frame (no FBO support).
    ImVec2 const p0(ox, oy);
    ImVec2 const p1(ox + (HPGL_MAX_X * scale), oy + (HPGL_MAX_Y * scale));
    dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255));
    dl->AddRect(p0, p1, IM_COL32(180, 180, 180, 255));

    auto pen_col = [](int pen) -> ImU32 {
      return (pen == 2) ? IM_COL32(200, 0, 0, 255) : IM_COL32(0, 0, 0, 255);
    };
    for (const auto& seg : segments) {
      ImU32 const col = pen_col(seg.pen);
      switch (seg.type) {
        case PlotPrimitive::Line: {
          ImVec2 const a(ox + (seg.x1 * scale),
                         oy + ((HPGL_MAX_Y - seg.y1) * scale));
          ImVec2 const b(ox + (seg.x2 * scale),
                         oy + ((HPGL_MAX_Y - seg.y2) * scale));
          dl->AddLine(a, b, col, 1.5f);
          break;
        }
        case PlotPrimitive::Circle: {
          ImVec2 const c(ox + (seg.x1 * scale),
                         oy + ((HPGL_MAX_Y - seg.y1) * scale));
          dl->AddCircle(c, seg.radius * scale, col, 64, 1.5f);
          break;
        }
        case PlotPrimitive::Arc: {
          ImVec2 const c(ox + (seg.x1 * scale),
                         oy + ((HPGL_MAX_Y - seg.y1) * scale));
          float sa = -seg.start_angle * (3.14159265f / 180.0f);
          float ea =
              -(seg.start_angle + seg.sweep_angle) * (3.14159265f / 180.0f);
          if (sa > ea) std::swap(sa, ea);
          dl->PathArcTo(c, seg.radius * scale, sa, ea, 64);
          dl->PathStroke(col, 0, 1.5f);
          break;
        }
        case PlotPrimitive::Label: {
          ImVec2 const pos(ox + (seg.x1 * scale),
                           oy + ((HPGL_MAX_Y - seg.y1) * scale));
          dl->AddText(pos, col, seg.text.c_str());
          break;
        }
      }
    }
  }

  // Current pen position indicator
  const PlotterViewStatus pvpen = plotter_view_status();
  if (pvpen.pen > 0) {
    ImVec2 const pp(ox + (pvpen.x * scale),
                    oy + ((HPGL_MAX_Y - pvpen.y) * scale));
    ImU32 const pc = pvpen.pen_down ? IM_COL32(0, 180, 0, 255)
                                    : IM_COL32(100, 100, 255, 255);
    dl->AddCircleFilled(pp, 3.0f, pc);
  }

  // Reserve canvas space for scrolling/interaction
  ImGui::Dummy(canvas_size);

  ImGui::End();
}
