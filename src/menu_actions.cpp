#include "menu_actions.h"

// Canonical label + toggle metadata for every emulator action.  Shortcut hints
// are intentionally NOT stored here — every surface derives them from the live
// binding via koncpc_action_shortcut() so a label can never drift from its key.
// One canonical label per action, shared by the native menu, ImGui menus, the
// F1 popup and the command palette.
const std::vector<MenuAction>& koncpc_menu_actions() {
  static const std::vector<MenuAction> actions = {
      {KONCPC_GUI, "Menu", "", false},
      {KONCPC_VKBD, "Virtual Keyboard", "", false},
      {KONCPC_FULLSCRN, "Fullscreen", "", false},
      {KONCPC_DEVTOOLS, "DevTools", "", true},
      {KONCPC_SCRNSHOT, "Screenshot", "", false},
      {KONCPC_SNAPSHOT, "Save Snapshot", "", false},
      {KONCPC_TAPEPLAY, "Tape Play/Stop", "", false},
      {KONCPC_LD_SNAP, "Load Snapshot", "", false},
      {KONCPC_RESET, "Reset", "", false},
      {KONCPC_NEXTDISKA, "Next A: Disk in ZIP", "", false},
      {KONCPC_MF2STOP, "MF2 Stop", "", false},
      {KONCPC_JOY, "Joystick Emulation", "", true},
      {KONCPC_PHAZER, "Phazer Emulation", "", true},
      {KONCPC_FPS, "Show FPS", "", true},
      {KONCPC_SPEED, "Limit Speed", "", true},
      {KONCPC_DEBUG, "Verbose Logging", "", true},
      {KONCPC_EXIT, "Quit", "", false},
      {KONCPC_PASTE, "Paste", "", false},
      {KONCPC_DELAY, "Delay", "", false},
      {KONCPC_WAITBREAK, "Wait Break", "", false},
  };
  return actions;
}

const MenuAction* koncpc_find_action(KONCPC_KEYS action) {
  for (const MenuAction& a : koncpc_menu_actions()) {
    if (a.action == action) return &a;
  }
  return nullptr;
}
