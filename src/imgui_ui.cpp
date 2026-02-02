#include "imgui_ui.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <iostream>

#include "cap32.h"
#include "z80.h"
#include "z80_disassembly.h"
#include "slotshandler.h"
#include "disk.h"
#include "portable-file-dialogs.h"

extern t_CPC CPC;
extern t_z80regs z80;
extern t_CRTC CRTC;
extern t_GateArray GateArray;
extern t_PSG PSG;
extern t_drive driveA;
extern t_drive driveB;
extern byte *pbRAM;

ImGuiUIState imgui_state;

// Forward declarations for per-dialog renderers
static void imgui_render_menu();
static void imgui_render_options();
static void imgui_render_devtools();
static void imgui_render_memory_tool();

// ─────────────────────────────────────────────────
// Theme setup
// ─────────────────────────────────────────────────

void imgui_init_ui()
{
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
}

// ─────────────────────────────────────────────────
// Main dispatcher
// ─────────────────────────────────────────────────

void imgui_render_ui()
{
  if (imgui_state.show_menu)        imgui_render_menu();
  if (imgui_state.show_options)     imgui_render_options();
  if (imgui_state.show_devtools)    imgui_render_devtools();
  if (imgui_state.show_memory_tool) imgui_render_memory_tool();
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
// Menu
// ─────────────────────────────────────────────────

static void imgui_render_menu()
{
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  ImGui::SetNextWindowSize(ImVec2(260, 0));

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_AlwaysAutoResize;

  if (!ImGui::Begin("konCePCja", nullptr, flags)) {
    ImGui::End();
    return;
  }

  // Keyboard shortcuts within menu
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { close_menu(); ImGui::End(); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_O)) { imgui_state.show_options = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_M)) { imgui_state.show_memory_tool = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_D)) { imgui_state.show_devtools = true; close_menu(); ImGui::End(); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) { emulator_reset(); close_menu(); ImGui::End(); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Q)) { imgui_state.show_quit_confirm = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_A)) { imgui_state.show_about = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) { emulator_reset(); close_menu(); ImGui::End(); return; }
  }

  float bw = ImGui::GetContentRegionAvail().x;

  if (ImGui::Button("Options (O)", ImVec2(bw, 0))) {
    imgui_state.show_options = true;
  }

  ImGui::Separator();

  // --- Disk operations ---
  if (ImGui::Button("Load Disk A...", ImVec2(bw, 0))) {
    auto f = pfd::open_file("Load Disk A", CPC.current_dsk_path,
      { "Disk Images", "*.dsk *.ipf *.raw *.zip" });
    auto result = f.result();
    if (!result.empty()) {
      CPC.driveA.file = result[0];
      file_load(CPC.driveA);
      CPC.current_dsk_path = result[0].substr(0, result[0].find_last_of("/\\"));
      close_menu();
    }
  }
  if (ImGui::Button("Load Disk B...", ImVec2(bw, 0))) {
    auto f = pfd::open_file("Load Disk B", CPC.current_dsk_path,
      { "Disk Images", "*.dsk *.ipf *.raw *.zip" });
    auto result = f.result();
    if (!result.empty()) {
      CPC.driveB.file = result[0];
      file_load(CPC.driveB);
      CPC.current_dsk_path = result[0].substr(0, result[0].find_last_of("/\\"));
      close_menu();
    }
  }
  if (ImGui::Button("Save Disk A...", ImVec2(bw, 0))) {
    if (driveA.tracks) {
      auto f = pfd::save_file("Save Disk A", CPC.current_dsk_path,
        { "Disk Images", "*.dsk" });
      auto result = f.result();
      if (!result.empty()) {
        dsk_save(result, &driveA);
        CPC.current_dsk_path = result.substr(0, result.find_last_of("/\\"));
      }
    }
  }
  if (ImGui::Button("Save Disk B...", ImVec2(bw, 0))) {
    if (driveB.tracks) {
      auto f = pfd::save_file("Save Disk B", CPC.current_dsk_path,
        { "Disk Images", "*.dsk" });
      auto result = f.result();
      if (!result.empty()) {
        dsk_save(result, &driveB);
        CPC.current_dsk_path = result.substr(0, result.find_last_of("/\\"));
      }
    }
  }

  ImGui::Separator();

  // --- Snapshot operations ---
  if (ImGui::Button("Load Snapshot...", ImVec2(bw, 0))) {
    auto f = pfd::open_file("Load Snapshot", CPC.current_snap_path,
      { "Snapshots", "*.sna *.zip" });
    auto result = f.result();
    if (!result.empty()) {
      CPC.snapshot.file = result[0];
      file_load(CPC.snapshot);
      CPC.current_snap_path = result[0].substr(0, result[0].find_last_of("/\\"));
      close_menu();
    }
  }
  if (ImGui::Button("Save Snapshot...", ImVec2(bw, 0))) {
    auto f = pfd::save_file("Save Snapshot", CPC.current_snap_path,
      { "Snapshots", "*.sna" });
    auto result = f.result();
    if (!result.empty()) {
      snapshot_save(result);
      CPC.current_snap_path = result.substr(0, result.find_last_of("/\\"));
    }
  }

  ImGui::Separator();

  // --- Tape & Cartridge ---
  if (ImGui::Button("Load Tape...", ImVec2(bw, 0))) {
    auto f = pfd::open_file("Load Tape", CPC.current_tape_path,
      { "Tape Images", "*.cdt *.voc *.zip" });
    auto result = f.result();
    if (!result.empty()) {
      CPC.tape.file = result[0];
      file_load(CPC.tape);
      CPC.current_tape_path = result[0].substr(0, result[0].find_last_of("/\\"));
      close_menu();
    }
  }
  if (ImGui::Button("Load Cartridge...", ImVec2(bw, 0))) {
    auto f = pfd::open_file("Load Cartridge", CPC.current_cart_path,
      { "Cartridges", "*.cpr *.zip" });
    auto result = f.result();
    if (!result.empty()) {
      CPC.cartridge.file = result[0];
      file_load(CPC.cartridge);
      CPC.current_cart_path = result[0].substr(0, result[0].find_last_of("/\\"));
      emulator_reset();
      close_menu();
    }
  }

  ImGui::Separator();

  // --- Tools ---
  if (ImGui::Button("Memory Tool (M)", ImVec2(bw, 0))) {
    imgui_state.show_memory_tool = true;
  }
  if (ImGui::Button("DevTools (D)", ImVec2(bw, 0))) {
    imgui_state.show_devtools = true;
    close_menu();
  }

  ImGui::Separator();

  if (ImGui::Button("Reset (F5/R)", ImVec2(bw, 0))) {
    emulator_reset();
    close_menu();
  }
  if (ImGui::Button("About (A)", ImVec2(bw, 0))) {
    imgui_state.show_about = true;
  }
  if (ImGui::Button("Resume (Esc)", ImVec2(bw, 0))) {
    close_menu();
  }
  if (ImGui::Button("Quit (Q)", ImVec2(bw, 0))) {
    imgui_state.show_quit_confirm = true;
  }

  ImGui::End();

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

static int find_ram_index(unsigned int ram) {
  for (int i = 0; i < 6; i++) {
    if (ram_size_values[i] == static_cast<int>(ram)) return i;
  }
  return 2; // default 192
}

static int find_sample_rate_index(unsigned int rate) {
  for (int i = 0; i < 5; i++) {
    if (sample_rate_values[i] == static_cast<int>(rate)) return i;
  }
  return 2; // default 44100
}

static void imgui_render_options()
{
  static bool first_open = true;
  if (first_open) {
    imgui_state.old_cpc_settings = CPC;
    first_open = false;
  }

  ImGuiIO& io = ImGui::GetIO();
  ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
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
          auto f = pfd::open_file("Select ROM", CPC.rom_path,
            { "ROM files", "*.rom *.bin" });
          auto result = f.result();
          if (!result.empty()) {
            CPC.rom_file[i] = result[0];
          }
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
  auto breakpoints = z80_list_breakpoints();

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
    if (imgui_state.devtools_bp_addr[0]) {
      word addr = static_cast<word>(strtol(imgui_state.devtools_bp_addr, nullptr, 16));
      z80_add_breakpoint(addr);
      imgui_state.devtools_bp_addr[0] = '\0';
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear BPs")) {
    z80_clear_breakpoints();
  }
}

static void devtools_format_mem_line(std::ostringstream& out, unsigned int base_addr,
                                     int bytes_per_line, int format)
{
  out << std::uppercase << std::setfill('0') << std::setw(4) << std::hex << base_addr << " : ";
  for (int j = 0; j < bytes_per_line; j++) {
    unsigned int addr = (base_addr + j) & 0xFFFF;
    out << std::setw(2) << static_cast<unsigned int>(pbRAM[addr]) << " ";
  }

  // Extended formats
  if (format == 1) { // Hex & char
    out << " | ";
    for (int j = 0; j < bytes_per_line; j++) {
      byte b = pbRAM[(base_addr + j) & 0xFFFF];
      out << (char)((b >= 32 && b < 127) ? b : '.');
    }
  } else if (format == 2) { // Hex & u8
    out << " | ";
    for (int j = 0; j < bytes_per_line; j++) {
      out << std::dec << std::setw(3) << static_cast<unsigned int>(pbRAM[(base_addr + j) & 0xFFFF]) << " ";
    }
  }
  out << "\n";
}

static void devtools_tab_memory()
{
  // Poke
  ImGui::SetNextItemWidth(50);
  ImGui::InputText("Addr##dtpoke", imgui_state.devtools_poke_addr, sizeof(imgui_state.devtools_poke_addr),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(40);
  ImGui::InputText("Val##dtpoke", imgui_state.devtools_poke_val, sizeof(imgui_state.devtools_poke_val),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (ImGui::Button("Poke##dt")) {
    if (imgui_state.devtools_poke_addr[0] && imgui_state.devtools_poke_val[0]) {
      unsigned int addr = strtol(imgui_state.devtools_poke_addr, nullptr, 16);
      int val = strtol(imgui_state.devtools_poke_val, nullptr, 16);
      if (addr < 65536 && val >= 0 && val <= 255) {
        pbRAM[addr] = static_cast<byte>(val);
      }
    }
  }

  // Display address
  ImGui::SetNextItemWidth(50);
  ImGui::InputText("Display##dt", imgui_state.devtools_display_addr, sizeof(imgui_state.devtools_display_addr),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (ImGui::Button("Go##dt")) {
    if (imgui_state.devtools_display_addr[0])
      imgui_state.devtools_display_value = strtol(imgui_state.devtools_display_addr, nullptr, 16);
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
        std::ostringstream line;
        devtools_format_mem_line(line, i * bpl, bpl, imgui_state.devtools_mem_format);
        ImGui::TextUnformatted(line.str().c_str());
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

static void devtools_tab_char()
{
  ImGui::Text("Character Set (from CPC font memory)");
  ImGui::Separator();
  ImGui::TextWrapped("Character grid rendering requires access to the CPC font "
                     "memory region. This will be implemented when the font "
                     "address is exposed from the gate array.");
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
          z80_add_breakpoint(next_it->address_);
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
    if (ImGui::BeginTabItem("Char"))   { devtools_tab_char();   ImGui::EndTabItem(); }
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
  ImGui::SetNextItemWidth(50);
  ImGui::InputText("Addr##mt", imgui_state.mem_poke_addr, sizeof(imgui_state.mem_poke_addr),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(40);
  ImGui::InputText("Val##mt", imgui_state.mem_poke_val, sizeof(imgui_state.mem_poke_val),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (ImGui::Button("Poke##mt")) {
    if (imgui_state.mem_poke_addr[0] && imgui_state.mem_poke_val[0]) {
      unsigned int addr = strtol(imgui_state.mem_poke_addr, nullptr, 16);
      int val = strtol(imgui_state.mem_poke_val, nullptr, 16);
      if (addr < 65536 && val >= 0 && val <= 255) {
        pbRAM[addr] = static_cast<byte>(val);
      }
    }
  }

  // Display address
  ImGui::SetNextItemWidth(50);
  ImGui::InputText("Display##mt", imgui_state.mem_display_addr, sizeof(imgui_state.mem_display_addr),
                   ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (ImGui::Button("Go##mt")) {
    if (imgui_state.mem_display_addr[0])
      imgui_state.mem_display_value = strtol(imgui_state.mem_display_addr, nullptr, 16);
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
    if (imgui_state.mem_filter_val[0]) {
      imgui_state.mem_filter_value = strtol(imgui_state.mem_filter_val, nullptr, 16);
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

        std::ostringstream line;
        line << std::uppercase << std::setfill('0') << std::setw(4) << std::hex << base << " : ";
        for (int j = 0; j < bpl; j++) {
          line << std::setw(2) << static_cast<unsigned int>(pbRAM[(base + j) & 0xFFFF]) << " ";
        }
        ImGui::TextUnformatted(line.str().c_str());
      }
    } else {
      // Fast path with clipper
      ImGuiListClipper clipper;
      clipper.Begin(total_lines);
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
          unsigned int base = i * bpl;
          std::ostringstream line;
          line << std::uppercase << std::setfill('0') << std::setw(4) << std::hex << base << " : ";
          for (int j = 0; j < bpl; j++) {
            line << std::setw(2) << static_cast<unsigned int>(pbRAM[(base + j) & 0xFFFF]) << " ";
          }
          ImGui::TextUnformatted(line.str().c_str());
        }
      }
    }
  }
  ImGui::EndChild();

  if (!open) imgui_state.show_memory_tool = false;

  ImGui::End();
}
