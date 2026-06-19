#include "menu_actions.h"

const std::vector<MenuAction>& koncpc_menu_actions() {
  static const std::vector<MenuAction> actions = {
      {KONCPC_GUI, "Menu", ""},
      {KONCPC_VKBD, "Virtual Keyboard", ""},
      {KONCPC_FULLSCRN, "Fullscreen", ""},
      {KONCPC_DEVTOOLS, "DevTools", ""},
      {KONCPC_SCRNSHOT, "Screenshot", ""},
      {KONCPC_SNAPSHOT, "Save Snapshot", ""},
      {KONCPC_TAPEPLAY, "Tape Play", ""},
      {KONCPC_LD_SNAP, "Load Snapshot", ""},
      {KONCPC_RESET, "Reset", ""},
      {KONCPC_NEXTDISKA, "Next A: Disk in ZIP", ""},
      {KONCPC_MF2STOP, "MF2 Stop", ""},
      {KONCPC_JOY, "Toggle Joystick Emulation", ""},
      {KONCPC_PHAZER, "Toggle Phazer Emulation", ""},
      {KONCPC_FPS, "Toggle FPS", ""},
      {KONCPC_SPEED, "Toggle Speed Limit", ""},
      {KONCPC_EXIT, "Quit", ""},
      {KONCPC_PASTE, "Paste", ""},
      {KONCPC_DELAY, "Delay", ""},
      {KONCPC_WAITBREAK, "Wait Break", ""},
  };
  return actions;
}
