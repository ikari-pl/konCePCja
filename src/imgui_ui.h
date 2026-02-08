#ifndef IMGUI_UI_H
#define IMGUI_UI_H

#include <string>
#include <vector>
#include "koncepcja.h"

enum class FileDialogAction {
  None,
  LoadDiskA, LoadDiskB, SaveDiskA, SaveDiskB,
  LoadSnapshot, SaveSnapshot,
  LoadTape, LoadCartridge,
  LoadROM,
  LoadDiskA_LED, LoadDiskB_LED,
  LoadTape_LED
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

  // Menu was just opened (for first-frame focus)
  bool menu_just_opened = false;

  // Top bar state
  std::string topbar_fps;
  bool drive_a_led = false;
  bool drive_b_led = false;

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

  // DevTools state
  bool devtools_regs_locked = true;
  char devtools_search[64] = "";
  char devtools_poke_addr[8] = "";
  char devtools_poke_val[8] = "";
  char devtools_display_addr[8] = "";
  int devtools_bytes_per_line = 16;
  int devtools_mem_format = 0;
  int devtools_display_value = -1;
  int devtools_filter_value = -1;
  char devtools_bp_addr[8] = "";
  char devtools_ep_addr[8] = "";

  // Virtual keyboard state
  bool vkeyboard_caps_lock = false;      // CAPS LOCK - sticky toggle
  bool vkeyboard_shift_next = false;     // SHIFT - one-shot, clears after 1 char
  bool vkeyboard_ctrl_next = false;      // CTRL - one-shot, clears after 1 char

  // Debug windows (Phase 2)
  bool show_registers = false;
  bool show_disassembly = false;
  bool show_memory_hex = false;
  bool show_stack_window = false;
  bool show_breakpoint_list = false;

  // Disassembly window state
  bool disasm_follow_pc = true;
  char disasm_goto_addr[8] = "";
  int disasm_goto_value = -1;

  // Memory hex window state
  char memhex_goto_addr[8] = "";
  int memhex_goto_value = -1;
  int memhex_bytes_per_row = 16;
};

extern ImGuiUIState imgui_state;

void imgui_init_ui();
void imgui_render_ui();
int imgui_topbar_height();

#endif // IMGUI_UI_H
