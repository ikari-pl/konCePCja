#include "menu_actions.h"

const std::vector<MenuAction>& koncpc_menu_actions() {
  static const std::vector<MenuAction> actions = {
    { KONCPC_GUI,      "Menu", "F1" },
    { KONCPC_VKBD,     "Virtual Keyboard", "Shift+F1" },
    { KONCPC_FULLSCRN, "Fullscreen", "F2" },
    { KONCPC_DEVTOOLS, "DevTools", "Shift+F2" },
    { KONCPC_SCRNSHOT, "Screenshot", "F3" },
    { KONCPC_SNAPSHOT, "Save Snapshot", "Shift+F3" },
    { KONCPC_TAPEPLAY, "Tape Play", "F4" },
    { KONCPC_LD_SNAP,  "Load Snapshot", "Shift+F4" },
    { KONCPC_RESET,    "Reset", "F5" },
    { KONCPC_NEXTDISKA,"Next A: Disk in ZIP", "Shift+F5" },
    { KONCPC_MF2STOP,  "MF2 Stop", "F6" },
    { KONCPC_JOY,      "Toggle Joystick Emulation", "F7" },
    { KONCPC_PHAZER,   "Toggle Phazer Emulation", "Shift+F7" },
    { KONCPC_FPS,      "Toggle FPS", "F8" },
    { KONCPC_SPEED,    "Toggle Speed Limit", "F9" },
    { KONCPC_EXIT,     "Quit", "F10" },
    { KONCPC_PASTE,    "Paste", "F11" },
    { KONCPC_DEBUG,    "Toggle Debug", "F12" },
    { KONCPC_DELAY,    "Delay", "" },
    { KONCPC_WAITBREAK,"Wait Break", "" },
  };
  return actions;
}
