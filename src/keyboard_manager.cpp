#include "keyboard_manager.h"

#include "keyboard.h"  // for applyKeypressDirect

extern t_CPC CPC;

KeyboardManager g_keyboard_manager;

KeyboardManager::KeyboardManager() {
  for (bool& i : line_scanned) {
    i = false;
  }
}

void KeyboardManager::handle_keydown(CPCScancode scancode,
                                     std::atomic<byte> keyboard_matrix[]) {
  if (static_cast<byte>(scancode) == 0xff) return;

  auto it = pending_releases.begin();
  while (it != pending_releases.end()) {
    if (it->scancode == scancode) {
      it = pending_releases.erase(it);
    } else {
      ++it;
    }
  }

  applyKeypressDirect(scancode, keyboard_matrix, true, false);

  if (CPC.keyboard_support_mode == KeyboardSupportMode::BufferedUntilRead) {
    key_needs_scan[scancode] = true;
    byte const line = static_cast<byte>(scancode) >> 4;
    line_scanned[line] = false;
  }
}

void KeyboardManager::handle_keyup(CPCScancode scancode,
                                   std::atomic<byte> keyboard_matrix[],
                                   bool release_modifiers,
                                   dword current_frame) {
  if (static_cast<byte>(scancode) == 0xff) return;

  if (CPC.keyboard_support_mode == KeyboardSupportMode::Direct) {
    key_needs_scan.erase(scancode);
    release_key(scancode, keyboard_matrix, release_modifiers);
  } else if (CPC.keyboard_support_mode ==
             KeyboardSupportMode::BufferedUntilRead) {
    auto it2 = key_needs_scan.find(scancode);
    if (it2 == key_needs_scan.end() || !it2->second) {
      key_needs_scan.erase(scancode);
      release_key(scancode, keyboard_matrix, release_modifiers);
    } else {
      pending_releases.push_back({scancode, 0, release_modifiers});
    }
  } else if (CPC.keyboard_support_mode == KeyboardSupportMode::Min2Frames) {
    pending_releases.push_back(
        {scancode, current_frame + 2, release_modifiers});
  }
}

void KeyboardManager::notify_scanned(int line) {
  line_scanned[line] = true;
  if (CPC.keyboard_support_mode == KeyboardSupportMode::BufferedUntilRead) {
    for (auto& pair : key_needs_scan) {
      if (pair.second) {
        byte const key_line = static_cast<byte>(pair.first) >> 4;
        if (key_line == line) {
          pair.second = false;
        }
      }
    }
  }
}

// engine=1: `read_value` is the matrix byte the firmware actually read for this
// line. A pending key is confirmed scanned ONLY if it was pressed there (bit
// clear) — a key set after the frame-start publish is not in this snapshot, so
// it stays held until the frame that really scans it, instead of being released
// (and dropped) on the strength of an unrelated same-row scan.
void KeyboardManager::notify_scanned(int line, byte read_value) {
  line_scanned[line] = true;
  if (CPC.keyboard_support_mode == KeyboardSupportMode::BufferedUntilRead) {
    for (auto& pair : key_needs_scan) {
      if (pair.second && (static_cast<byte>(pair.first) >> 4) == line) {
        const byte bit = static_cast<byte>(pair.first) & 0x0F;
        if (!(read_value & (1u << bit)))  // bit clear = key was pressed here
          pair.second = false;
      }
    }
  }
}

void KeyboardManager::update(std::atomic<byte> keyboard_matrix[],
                             dword current_frame) {
  if (pending_releases.empty()) return;

  auto it = pending_releases.begin();
  while (it != pending_releases.end()) {
    bool should_release = false;
    if (CPC.keyboard_support_mode == KeyboardSupportMode::BufferedUntilRead) {
      auto found = key_needs_scan.find(it->scancode);
      if (found == key_needs_scan.end() || !found->second) {
        should_release = true;
      }
    } else if (CPC.keyboard_support_mode == KeyboardSupportMode::Min2Frames) {
      if (current_frame >= it->release_frame) {
        should_release = true;
      }
    } else {
      should_release = true;
    }

    if (should_release) {
      key_needs_scan.erase(it->scancode);
      release_key(it->scancode, keyboard_matrix, it->release_modifiers);
      it = pending_releases.erase(it);
    } else {
      ++it;
    }
  }
}

void KeyboardManager::release_key(CPCScancode scancode,
                                  std::atomic<byte> keyboard_matrix[],
                                  bool release_modifiers) {
  applyKeypressDirect(scancode, keyboard_matrix, false, release_modifiers);
}
