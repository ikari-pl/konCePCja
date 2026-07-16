#pragma once

// Headless-safe data side of the modern UI.
//
// Background: the modern UI (ImGui + SDL_GPU) and the emulator core both
// touch a process-wide "UIState" struct.  The core writes telemetry into
// it (frame_time_*, audio_*, drive_*_led, …); the UI reads telemetry to
// render and writes back request flags (show_devtools, …).
//
// Today the struct lived in `imgui_ui.h` next to function declarations
// that pull ImGui types into every translation unit that includes the
// header.  For the headless build target (P1.5.2) the core TUs need
// the *struct* but must not see UI function declarations, otherwise
// linker tries to resolve them in builds where imgui_ui.cpp is excluded.
//
// This header is the headless-safe extraction: just the POD-ish state +
// the extern.  No ImGui includes.  `imgui_ui.h` now `#include`s this
// header for backward compatibility, so modern-UI translation units keep
// compiling unchanged.
//
// Definition of the singleton `imgui_state` lives in `imgui_state.cpp`,
// which is unconditionally compiled into `koncepcja_lib` (both MODERN_UI
// ON and OFF), so the symbol is always resolvable.

#include <deque>
#include <cstdint>
#include <string>
#include <vector>

#include "koncepcja.h"

enum class FileDialogAction : std::uint8_t {
  None,
  LoadDiskA,
  LoadDiskB,
  SaveDiskA,       // drive A → .dsk (the live FDC medium, not driveA)
  SaveDiskB,       // drive B → .dsk (the live FDC medium, not driveB)
  SaveDiskA_SCP,   // drive A → .scp flux (flux-backed disc only)
  SaveDiskA_HFE,   // drive A → .hfe flux (flux-backed disc only)
  NewDiskA,        // create a blank disc (Sector/Flux per new_disk_flux)
  LoadSnapshot,
  SaveSnapshot,
  LoadTape,
  LoadCartridge,
  LoadROM,
  LoadDiskA_LED,
  LoadDiskB_LED,
  LoadTape_LED,
  SelectM4SDFolder,
  SavePlotterSVG
};

struct ImGuiUIState {
  // Dialog visibility flags
  bool show_menu = false;
  bool show_options = false;
  bool show_devtools = false;
  bool show_memory_tool = false;
  bool show_vkeyboard = false;
  bool show_vjoystick = false;
  bool show_about = false;
  bool show_quit_confirm = false;
  bool show_serial_terminal = false;
  bool show_plotter_preview = false;
  bool show_new_disk = false;   // "New Disk…" backing-choice modal
  bool new_disk_flux = false;   // its Sector/Flux radio (true = Flux)

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
  int audio_underruns = 0;          // times SDL queue was empty when we pushed
  int audio_near_underruns = 0;     // times queue was below 1 buffer
  int audio_pushes = 0;             // total pushes this second
  float audio_queue_avg_ms = 0.0f;  // average queue depth in ms
  float audio_queue_min_ms = 0.0f;  // minimum queue depth in ms
  float audio_push_interval_max_us = 0.0f;  // longest gap between pushes

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
  int eject_confirm_drive = -1;  // -1=none, 0=A, 1=B
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
  byte tape_wave_buf[TAPE_WAVE_SAMPLES] = {};  // raw pulse level
  int tape_wave_head = 0;

  int tape_wave_mode = 0;  // 0=pulse, 1=decoded

  // Decoded bits ring buffer (written by Tape_ReadDataBit)
  static constexpr int TAPE_DECODED_SAMPLES = 200;
  byte tape_decoded_buf[TAPE_DECODED_SAMPLES] = {};
  int tape_decoded_head = 0;

  // Virtual keyboard state
  bool vkeyboard_caps_lock = false;   // CAPS LOCK - sticky toggle
  bool vkeyboard_shift_next = false;  // SHIFT - one-shot, clears after 1 char
  bool vkeyboard_ctrl_next = false;   // CTRL - one-shot, clears after 1 char

  // Virtual joystick state
  enum class VJoyTarget : std::uint8_t { Joy0 = 0, Joy1 = 1 };
  VJoyTarget vjoystick_target = VJoyTarget::Joy0;  // which CPC joystick to drive
  unsigned vjoystick_held_mask = 0;  // matrix bits currently asserted by the vjoy
  int vjoystick_held_target = 0;     // CPC joystick those bits belong to
  bool vjoystick_focused = false;    // vjoy window is the focused one -> host
                                     // arrows+space drive the joystick (and only
                                     // then); also captures the CPC keyboard

  // Docked mode: CPC Screen focus tracking for keyboard routing
  bool cpc_screen_focused =
      false;  // true when CPC Screen tab is the focused window
  bool request_cpc_screen_focus = false;  // set by event loop on app focus gain

  // Layout dropdown (topbar)
  bool show_layout_dropdown = false;

  // Toast notification system
  enum class ToastLevel : std::uint8_t { Info, Success, Error };
  struct Toast {
    std::string message;
    ToastLevel level = ToastLevel::Info;
    float timer = 0.0f;    // seconds remaining
    float initial = 0.0f;  // initial duration (for fade calc)
  };
  std::deque<Toast> toasts;  // rendered bottom-up, newest last
  static constexpr int MAX_TOASTS = 4;
  static constexpr float TOAST_DURATION = 3.5f;
  static constexpr float TOAST_FADE_TIME = 0.5f;
};

extern ImGuiUIState imgui_state;
