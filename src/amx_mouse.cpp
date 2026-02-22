#include "amx_mouse.h"

AMXMouse g_amx_mouse;

void amx_mouse_reset()
{
   g_amx_mouse.accum_x = 0;
   g_amx_mouse.accum_y = 0;
   g_amx_mouse.buttons = 0;
   g_amx_mouse.row9_selected = false;
   g_amx_mouse.row9_was_deselected = false;
}

void amx_mouse_update(float dx, float dy, uint32_t sdl_buttons)
{
   // Accumulate motion — SDL gives sub-pixel floats in SDL3
   g_amx_mouse.accum_x += static_cast<int>(dx);
   g_amx_mouse.accum_y += static_cast<int>(dy);

   // Store button state (SDL: bit 0=left, bit 1=middle, bit 2=right)
   g_amx_mouse.buttons = static_cast<uint8_t>(sdl_buttons);
}

void amx_mouse_row_select(int line)
{
   bool now_row9 = (line == 9);
   if (!now_row9 && g_amx_mouse.row9_selected) {
      // Row 9 was deselected — mark for mickey consumption on next select
      g_amx_mouse.row9_was_deselected = true;
   }
   if (now_row9 && g_amx_mouse.row9_was_deselected) {
      // Row 9 re-selected after deselect — consume one mickey in each axis
      if (g_amx_mouse.accum_x > 0) g_amx_mouse.accum_x--;
      else if (g_amx_mouse.accum_x < 0) g_amx_mouse.accum_x++;
      if (g_amx_mouse.accum_y > 0) g_amx_mouse.accum_y--;
      else if (g_amx_mouse.accum_y < 0) g_amx_mouse.accum_y++;
      g_amx_mouse.row9_was_deselected = false;
   }
   g_amx_mouse.row9_selected = now_row9;
}

byte amx_mouse_get_row9()
{
   byte val = 0xFF;  // all bits high = nothing pressed (active-low)

   // Direction bits (active-low: clear bit when there's motion)
   if (g_amx_mouse.accum_y < 0) val &= ~0x01;  // up
   if (g_amx_mouse.accum_y > 0) val &= ~0x02;  // down
   if (g_amx_mouse.accum_x < 0) val &= ~0x04;  // left
   if (g_amx_mouse.accum_x > 0) val &= ~0x08;  // right

   // Button bits (active-low)
   // SDL3: SDL_BUTTON_LMASK=1, SDL_BUTTON_MMASK=2, SDL_BUTTON_RMASK=4
   if (g_amx_mouse.buttons & 1) val &= ~0x10;  // left button -> fire2 (bit 4)
   if (g_amx_mouse.buttons & 4) val &= ~0x20;  // right button -> fire1 (bit 5)
   if (g_amx_mouse.buttons & 2) val &= ~0x40;  // middle button -> fire3 (bit 6)

   return val;
}
