#pragma once
#include <vector>

// ── Single-source menu bridge ──────────────────────────────────────────────
//
// Plain C++ header (compiles on every platform / build flavour).  Both the
// in-window ImGui menu bar (imgui_ui.cpp) AND the native macOS menu bar
// (macos_menu.mm) render the NON-KONCPC parts of the taxonomy from the data
// and entry points declared here, so the two bars can never drift again.
//
// The KONCPC_* command items keep coming from the action registry
// (menu_actions.h); this bridge covers everything else: Settings deep-links,
// the Window menu (specials + devtools windows), View ▸ Scale, View ▸
// Renderer, file-dialog launches, the About dialog and the command palette.
//
// All definitions live in the GUI translation unit (imgui_ui.cpp), which owns
// imgui_state, OptionsTab/s_pending_options_tab, apply_scr_scale,
// file_dialog_callback, g_command_palette, g_devtools_ui, CPC and
// video_plugin_list.  In non-GUI / headless builds imgui_ui.cpp is not linked,
// so only macos_menu.mm (mac-only, MODERN_UI-guarded) calls these.

// Machine ▸ Settings deep-links: each opens the Settings window on a tab.
// `tab` is the integer value of imgui_ui.cpp's file-scope OptionsTab enum.
struct SettingsTabItem {
  const char* label;
  int tab;
  bool separator_before;
};
const std::vector<SettingsTabItem>& koncpc_settings_tab_items();

// Window menu items: 3 specials (key "$vkbd" / "$serial" / "$plotter") plus
// the 17 devtools windows (key == window name).  `separator_before` reproduces
// the grouping used by the in-window bar.
struct WindowMenuItem {
  const char* label;
  const char* key;
  bool separator_before;
};
const std::vector<WindowMenuItem>& koncpc_window_menu_items();

// View ▸ Scale labels: {"Fit window","1x","1.5x","2x","3x"} (index == scale).
const std::vector<const char*>& koncpc_scale_labels();

extern "C" {
// konCePCja ▸ About konCePCja  → imgui_state.show_about = true
void koncpc_show_about_dialog();

// Machine ▸ <tab> / konCePCja ▸ Settings  → show Options window on `tab`.
void koncpc_open_settings_tab(int tab);

// Tools ▸ Command Palette  → g_command_palette.open()
void koncpc_open_command_palette();

// Media ▸ loaders/savers  → issue the SDL file dialog for a FileDialogAction
// (filters + default path single-sourced here).  `action` is the integer of
// imgui_state.h's FileDialogAction enum.
void koncpc_request_file_dialog(int action);

// Window menu toggles.  "$vkbd"/"$serial"/"$plotter" flip the matching
// imgui_state bool; any other key toggles the named devtools window.
void koncpc_window_toggle(const char* key);
// Mirror of the above for the live checkmark.
bool koncpc_window_is_open(const char* key);

// View ▸ Scale  → apply_scr_scale(idx) / read CPC.scr_scale.
void koncpc_set_scale(int idx);
int koncpc_current_scale();

// View ▸ Renderer  → CPC.scr_style + video_reinit_pending / read CPC.scr_style.
void koncpc_set_renderer(int plugin_idx);
int koncpc_current_renderer();
// Iteration over video_plugin_list, with the GPU/CPU group string
// single-sourced here so both bars group renderers identically.
int koncpc_renderer_count();
const char* koncpc_renderer_name(int i);
bool koncpc_renderer_hidden(int i);
const char* koncpc_renderer_group(int i);
}  // extern "C"
