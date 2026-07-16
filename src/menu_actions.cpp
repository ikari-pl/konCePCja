#include "menu_actions.h"

// Canonical label + toggle metadata for every emulator action.  Shortcut hints
// are intentionally NOT stored here — every surface derives them from the live
// binding via koncpc_action_shortcut() so a label can never drift from its key.
// One canonical label per action, shared by the native menu, ImGui menus, the
// F1 popup and the command palette.
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
const std::vector<MenuAction>& koncpc_menu_actions() {
  // {action, canonical label, shortcut(derived, empty), toggle, canonical
  // group}
  static const std::vector<MenuAction> actions = {
      {KONCPC_GUI, "Menu", "", false, MenuGroup::None},
      {KONCPC_VKBD, "Virtual Keyboard", "", false, MenuGroup::Window},
      {KONCPC_VJOY, "Virtual Joystick", "", false, MenuGroup::Window},
      {KONCPC_FULLSCRN, "Fullscreen", "", false, MenuGroup::View},
      {KONCPC_DEVTOOLS, "DevTools", "", true, MenuGroup::Tools},
      {KONCPC_SCRNSHOT, "Screenshot", "", false, MenuGroup::View},
      {KONCPC_SNAPSHOT, "Quick Save Snapshot", "", false, MenuGroup::Media},
      {KONCPC_TAPEPLAY, "Tape Play/Stop", "", false, MenuGroup::Media},
      {KONCPC_LD_SNAP, "Quick Load Snapshot", "", false, MenuGroup::Media},
      {KONCPC_RESET, "Reset", "", false, MenuGroup::Machine},
      {KONCPC_NEXTDISKA, "Next Disk in Archive", "", false, MenuGroup::Media},
      {KONCPC_MF2STOP, "Multiface II Stop", "", false, MenuGroup::Tools},
      {KONCPC_JOY, "Joystick Emulation", "", true, MenuGroup::Input},
      {KONCPC_PHAZER, "Light Gun (Magnum Phaser)", "", true, MenuGroup::Input},
      {KONCPC_FPS, "Show FPS", "", true, MenuGroup::View},
      {KONCPC_SPEED, "Limit Speed", "", true, MenuGroup::Input},
      {KONCPC_DEBUG, "Verbose Logging", "", true, MenuGroup::Tools},
      {KONCPC_EXIT, "Quit", "", false, MenuGroup::App},
      {KONCPC_PASTE, "Paste", "", false, MenuGroup::Edit},
      // Scripting-only autocmd primitives — not user-facing menu items (F9).
      {KONCPC_DELAY, "Delay", "", false, MenuGroup::None},
      {KONCPC_WAITBREAK, "Wait Break", "", false, MenuGroup::None},
  };
  return actions;
}

const MenuAction* koncpc_find_action(KONCPC_KEYS action) {
  for (const MenuAction& a : koncpc_menu_actions()) {
    if (a.action == action) return &a;
  }
  return nullptr;
}
