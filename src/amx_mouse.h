/* konCePCja - Amstrad CPC Emulator
   AMX Mouse - Joystick port mouse emulation

   The AMX Mouse connects to the joystick port and appears as keyboard
   matrix row 9 (joystick 0). Direction bits pulse LOW for one mickey
   per read cycle. Software must deselect/reselect row 9 between reads
   to consume motion pulses.

   Row 9 bit mapping:
     Bit 0: Up     (LOW = mouse moved up)
     Bit 1: Down   (LOW = mouse moved down)
     Bit 2: Left   (LOW = mouse moved left)
     Bit 3: Right  (LOW = mouse moved right)
     Bit 4: Fire2  (LOW = left button pressed)
     Bit 5: Fire1  (LOW = right button pressed)
     Bit 6: Fire3  (LOW = middle button pressed)
*/

#ifndef AMX_MOUSE_H
#define AMX_MOUSE_H

#include "types.h"
#include <cstdint>

struct AMXMouse {
   bool enabled = false;
   int accum_x = 0;          // accumulated X mickeys since last read
   int accum_y = 0;          // accumulated Y mickeys since last read
   uint8_t buttons = 0;      // SDL button state (not CPC-inverted)
   bool row9_selected = false;   // true when CPC is selecting row 9
   bool row9_was_deselected = false; // set when row deselected, cleared on reselect
};

extern AMXMouse g_amx_mouse;

void amx_mouse_reset();
void amx_mouse_update(float dx, float dy, uint32_t sdl_buttons);
void amx_mouse_row_select(int line);
byte amx_mouse_get_row9();

#endif
