#ifndef KEYBOARD_MANAGER_H
#define KEYBOARD_MANAGER_H

#include "types.h"
#include "keyboard.h"
#include "koncepcja.h"
#include <map>
#include <vector>

struct PendingKeyRelease {
    CPCScancode scancode;
    dword release_frame;
    bool release_modifiers;
};

class KeyboardManager {
public:
    KeyboardManager();
    
    void handle_keydown(CPCScancode scancode, byte keyboard_matrix[]);
    void handle_keyup(CPCScancode scancode, byte keyboard_matrix[], bool release_modifiers, dword current_frame);
    void update(byte keyboard_matrix[], dword current_frame);
    void notify_scanned(int line);
    
private:
    std::vector<PendingKeyRelease> pending_releases;
    
    // For Buffered mode
    std::map<CPCScancode, bool> key_needs_scan;
    bool line_scanned[16];
    
    void release_key(CPCScancode scancode, byte keyboard_matrix[], bool release_modifiers);
};

extern KeyboardManager g_keyboard_manager;

#endif
