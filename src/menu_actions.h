#pragma once
#include <vector>
#include <string>
#include "keyboard.h"

struct MenuAction {
  KONCPC_KEYS action;
  const char* title;
  const char* shortcut; // human-readable, e.g. "F5" or "Shift+F2"
};

const std::vector<MenuAction>& koncpc_menu_actions();
