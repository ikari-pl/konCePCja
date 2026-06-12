#pragma once

#include <map>
#include <vector>

#include "keyboard.h"
#include "koncepcja.h"
#include "types.h"

struct PendingKeyRelease {
  CPCScancode scancode;
  dword release_frame;
  bool release_modifiers;
};

class KeyboardManager {
 public:
  KeyboardManager();

  void handle_keydown(CPCScancode scancode,
                      std::atomic<byte> keyboard_matrix[]);
  void handle_keyup(CPCScancode scancode, std::atomic<byte> keyboard_matrix[],
                    bool release_modifiers, dword current_frame);
  void update(std::atomic<byte> keyboard_matrix[], dword current_frame);
  void notify_scanned(int line);

 private:
  std::vector<PendingKeyRelease> pending_releases;

  // For Buffered mode
  std::map<CPCScancode, bool> key_needs_scan;
  bool line_scanned[16];

  void release_key(CPCScancode scancode, std::atomic<byte> keyboard_matrix[],
                   bool release_modifiers);
};

extern KeyboardManager g_keyboard_manager;
