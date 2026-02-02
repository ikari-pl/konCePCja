#ifndef IMGUI_UI_H
#define IMGUI_UI_H

#include <string>
#include "cap32.h"

struct ImGuiUIState {
  // Dialog visibility flags
  bool show_menu = false;
  bool show_options = false;
  bool show_devtools = false;
  bool show_memory_tool = false;
  bool show_about = false;
  bool show_quit_confirm = false;

  // Menu was just opened (for first-frame focus)
  bool menu_just_opened = false;

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
};

extern ImGuiUIState imgui_state;

void imgui_init_ui();
void imgui_render_ui();

#endif // IMGUI_UI_H
