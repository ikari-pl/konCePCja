#pragma once
#include <string>
#include <vector>

#include "keyboard.h"

// Canonical top-level menu placement.  Every surface (in-window ImGui menu bar
// AND the native macOS menu bar) renders FROM this, so the two bars can never
// again diverge in grouping (sweep-two F2).  Taxonomy chosen in the sweep-two
// interview: App / Machine / Edit / Media / View / Tools / Window.
enum class MenuGroup {
  None = 0,  // not shown in a menu (scripting-only / the F1 trigger itself)
  App,       // application menu (named konCePCja): About, Settings, Reset, Quit
  Edit,      // Paste, ...
  Media,     // disks, tapes, cartridges, snapshots
  View,      // display actions: Fullscreen, Screenshot, Show FPS
  Input,     // input/emulation toggles: Joystick, Light Gun, Limit Speed
  Tools,     // DevTools, Command Palette, Multiface II, Diagnostics
  Window,    // tool/debug windows, virtual keyboard
};

// Single source of truth for an emulator action's UI metadata.  The shortcut
// hint is NOT stored here — it is derived from the live binding via
// koncpc_action_shortcut() so labels can never drift from the real keys.
struct MenuAction {
  KONCPC_KEYS action;
  const char* title;     // ONE canonical label, used by every surface
  const char* shortcut;  // DEPRECATED/transitional; always "" — use
                         // koncpc_action_shortcut(action) for the real hint
  bool toggle;      // true => the action flips a state shown as a checkmark
  MenuGroup group;  // canonical menu placement, shared by every surface
};

const std::vector<MenuAction>& koncpc_menu_actions();

// Look up an action's metadata by id, or nullptr if it has no entry.
const MenuAction* koncpc_find_action(KONCPC_KEYS action);

// Live toggle state for a toggle-kind action (checkmark in menus).  Defined in
// the GUI translation unit (imgui_ui.cpp) since it reads GUI/emulator globals;
// returns false for non-toggle actions or in non-GUI builds.
bool koncpc_action_is_active(KONCPC_KEYS action);
