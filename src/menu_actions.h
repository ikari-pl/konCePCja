#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "keyboard.h"

// Canonical top-level menu placement.  Every surface (in-window ImGui menu bar
// AND the native macOS menu bar) renders FROM this, so the two bars can never
// again diverge in grouping (sweep-two F2).  Taxonomy chosen in the sweep-two
// interview: App / Machine / Edit / Media / View / Tools / Window.
enum class MenuGroup : std::uint8_t {
  None = 0,  // not shown in a menu (scripting-only / the F1 trigger itself)
  App,       // application menu (named konCePCja): About, Settings, Quit
  Machine,   // the emulated machine: Reset + deep-links to the Settings tabs
  Edit,      // Paste, ...
  Media,     // disks, tapes, cartridges, snapshots
  View,      // display actions: Fullscreen, Screenshot, Show FPS
  Input,     // input devices: Joystick, Light Gun
  Tools,   // DevTools, Command Palette, Multiface II, Limit Speed, Diagnostics
  Window,  // tool/debug windows, virtual keyboard
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

// The native macOS menu's item text: "Reset  (F5)", or the bare title when
// there is no shortcut. The shortcut is TEXT, never an NSMenuItem key
// equivalent: SDL owns every key, and an AppKit accelerator on the same key
// fires the action twice (the F9 double-fire, f85a8b69).
std::string koncpc_menu_title_with_shortcut(const char* title,
                                            const std::string& shortcut);

// A registry action's native-menu text, with the shortcut its live binding has
// right now. Empty title for an unknown action. Before the InputMapper exists
// there is no binding to read, so there is no suffix — which is why the native
// menu is built after the mapper and refreshes these titles as it opens
// (beads-bqx).
std::string koncpc_action_menu_title(KONCPC_KEYS action);

// Live toggle state for a toggle-kind action (checkmark in menus).  Defined in
// the GUI translation unit (imgui_ui.cpp) since it reads GUI/emulator globals;
// returns false for non-toggle actions or in non-GUI builds.
bool koncpc_action_is_active(KONCPC_KEYS action);
