#include "menu_actions.h"

const std::vector<MenuAction>& cap32_menu_actions() {
  static const std::vector<MenuAction> actions = {
    { CAP32_GUI,      "Menu", "F1" },
    { CAP32_VKBD,     "Virtual Keyboard", "Shift+F1" },
    { CAP32_FULLSCRN, "Fullscreen", "F2" },
    { CAP32_DEVTOOLS, "DevTools", "Shift+F2" },
    { CAP32_SCRNSHOT, "Screenshot", "F3" },
    { CAP32_SNAPSHOT, "Save Snapshot", "Shift+F3" },
    { CAP32_TAPEPLAY, "Tape Play", "F4" },
    { CAP32_LD_SNAP,  "Load Snapshot", "Shift+F4" },
    { CAP32_RESET,    "Reset", "F5" },
    { CAP32_NEXTDISKA,"Next Disk A", "Shift+F5" },
    { CAP32_MF2STOP,  "MF2 Stop", "F6" },
    { CAP32_JOY,      "Toggle Joystick Emulation", "F7" },
    { CAP32_PHAZER,   "Toggle Phazer Emulation", "Shift+F7" },
    { CAP32_FPS,      "Toggle FPS", "F8" },
    { CAP32_SPEED,    "Toggle Speed Limit", "F9" },
    { CAP32_EXIT,     "Quit", "F10" },
    { CAP32_PASTE,    "Paste", "F11" },
    { CAP32_DEBUG,    "Toggle Debug", "F12" },
    { CAP32_DELAY,    "Delay", "" },
    { CAP32_WAITBREAK,"Wait Break", "" },
  };
  return actions;
}
