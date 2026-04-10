#ifndef IMGUI_UI_H
#define IMGUI_UI_H

#include <string>
#include <vector>
#include <deque>
#include "koncepcja.h"

enum class FileDialogAction {
  None,
  LoadDiskA, LoadDiskB, SaveDiskA, SaveDiskB,
  LoadSnapshot, SaveSnapshot,
  LoadTape, LoadCartridge,
  LoadROM,
  LoadDiskA_LED, LoadDiskB_LED,
  LoadTape_LED,
  SelectM4SDFolder
};

struct ImGuiUIState {
  // Dialog visibility flags
  bool show_menu = false;
  bool show_options = false;
  bool show_devtools = false;
  bool show_memory_tool = false;
  bool show_vkeyboard = false;
  bool show_about = false;
  bool show_quit_confirm = false;
  bool show_serial_terminal = false;
  bool show_plotter_preview = false;

  // Deferred video reinit (set in Options, handled after ImGui frame)
  bool video_reinit_pending = false;

  // Menu was just opened (for first-frame focus)
  bool menu_just_opened = false;

  // Top bar state
  std::string topbar_fps;
  bool drive_a_led = false;
  bool drive_b_led = false;

  // Frame timing measurement (updated each second, values in microseconds)
  float frame_time_avg_us = 0.0f;
  float frame_time_min_us = 0.0f;
  float frame_time_max_us = 0.0f;
  float display_time_avg_us = 0.0f;
  float sleep_time_avg_us = 0.0f;
  float z80_time_avg_us = 0.0f;

  // Audio diagnostics (updated each second)
  int audio_underruns = 0;           // times SDL queue was empty when we pushed
  int audio_near_underruns = 0;      // times queue was below 1 buffer
  int audio_pushes = 0;              // total pushes this second
  float audio_queue_avg_ms = 0.0f;   // average queue depth in ms
  float audio_queue_min_ms = 0.0f;   // minimum queue depth in ms
  float audio_push_interval_max_us = 0.0f; // longest gap between pushes

  // Options dialog state
  t_CPC old_cpc_settings;

  // Memory tool input buffers
  char mem_poke_addr[8] = "";
  char mem_poke_val[8] = "";
  char mem_display_addr[8] = "";
  char mem_filter_val[8] = "";
  int mem_bytes_per_line = 16;
  int mem_display_value = -1;
  int mem_filter_value = -1;

  // Eject confirmation
  int eject_confirm_drive = -1; // -1=none, 0=A, 1=B
  bool eject_confirm_tape = false;

  // Tape block index (built on tape load)
  std::vector<byte*> tape_block_offsets;
  int tape_current_block = 0;

  // File dialog async state
  FileDialogAction pending_dialog = FileDialogAction::None;
  std::string pending_dialog_result;
  int pending_rom_slot = -1;

  // Tape waveform oscilloscope
  static constexpr int TAPE_WAVE_SAMPLES = 128;
  byte tape_wave_buf[TAPE_WAVE_SAMPLES] = {};   // raw pulse level
  int  tape_wave_head = 0;

  int  tape_wave_mode = 0; // 0=pulse, 1=decoded

  // Decoded bits ring buffer (written by Tape_ReadDataBit)
  static constexpr int TAPE_DECODED_SAMPLES = 200;
  byte tape_decoded_buf[TAPE_DECODED_SAMPLES] = {};
  int  tape_decoded_head = 0;

  // Virtual keyboard state
  bool vkeyboard_caps_lock = false;      // CAPS LOCK - sticky toggle
  bool vkeyboard_shift_next = false;     // SHIFT - one-shot, clears after 1 char
  bool vkeyboard_ctrl_next = false;      // CTRL - one-shot, clears after 1 char

  // Docked mode: CPC Screen focus tracking for keyboard routing
  bool cpc_screen_focused = false;       // true when CPC Screen tab is the focused window
  bool request_cpc_screen_focus = false;  // set by event loop on app focus gain

  // Layout dropdown (topbar)
  bool show_layout_dropdown = false;

  // Toast notification system
  enum class ToastLevel { Info, Success, Error };
  struct Toast {
    std::string message;
    ToastLevel level = ToastLevel::Info;
    float timer = 0.0f;       // seconds remaining
    float initial = 0.0f;     // initial duration (for fade calc)
  };
  std::deque<Toast> toasts;   // rendered bottom-up, newest last
  static constexpr int MAX_TOASTS = 4;
  static constexpr float TOAST_DURATION = 3.5f;
  static constexpr float TOAST_FADE_TIME = 0.5f;

};

extern ImGuiUIState imgui_state;

void imgui_init_ui();
void imgui_render_ui();
int imgui_topbar_height();
void imgui_close_menu();

// Returns true when any keyboard-consuming UI is active (menus, dialogs,
// text fields, popups, devtools). Single source of truth — used by both
// the keyboard capture policy in imgui_ui.cpp and the event filter in
// kon_cpc_ja.cpp. Does NOT include the virtual keyboard (mouse-only).
bool imgui_any_keyboard_ui_active();

// Toast notifications
void imgui_toast(const std::string& message, ImGuiUIState::ToastLevel level = ImGuiUIState::ToastLevel::Info);

// MRU (recent files) helper — pushes path to front, deduplicates, caps at MRU_MAX
void imgui_mru_push(std::vector<std::string>& list, const std::string& path);

// Tape block scanning (called after tape load to populate block offsets)
void tape_scan_blocks();
void imgui_toast_info(const std::string& message);
void imgui_toast_success(const std::string& message);
void imgui_toast_error(const std::string& message);

// Serial Terminal window
void imgui_render_serial_terminal();
void serial_terminal_feed_byte(uint8_t byte);

// Plotter Preview window
void imgui_render_plotter_preview();

#endif // IMGUI_UI_H
