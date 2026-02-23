/* konCePCja - Amstrad CPC Emulator
   I/O Port Dispatch Table — implementation
*/

#include "io_dispatch.h"
#include <cassert>

IODispatch g_io_dispatch;

// ── Registration ───────────────────────────────────

void io_register_in(byte port_high, PeriphInHandler fn,
                    const bool* enabled, const char* name)
{
   PortSlot& slot = g_io_dispatch.in_slots[port_high];
   assert(slot.count < MAX_PORT_HANDLERS);
   auto& e = slot.entries[slot.count++];
   e.in_fn   = fn;
   e.enabled = enabled;
   e.name    = name;
}

void io_register_out(byte port_high, PeriphOutHandler fn,
                     const bool* enabled, const char* name)
{
   PortSlot& slot = g_io_dispatch.out_slots[port_high];
   assert(slot.count < MAX_PORT_HANDLERS);
   auto& e = slot.entries[slot.count++];
   e.out_fn  = fn;
   e.enabled = enabled;
   e.name    = name;
}

void io_register_kbd_read_hook(KeyboardReadHook fn, const bool* enabled)
{
   auto& slot = g_io_dispatch.kbd_read_hooks;
   assert(slot.count < MAX_HOOKS);
   slot.entries[slot.count++] = { fn, enabled };
}

void io_register_kbd_line_hook(NotifyHookInt fn, const bool* enabled)
{
   auto& slot = g_io_dispatch.kbd_line_hooks;
   assert(slot.count < MAX_HOOKS);
   slot.entries[slot.count++] = { fn, enabled };
}

void io_register_tape_motor_hook(NotifyHookBool fn, const bool* enabled)
{
   auto& slot = g_io_dispatch.tape_motor_hooks;
   assert(slot.count < MAX_HOOKS);
   slot.entries[slot.count++] = { fn, enabled };
}

void io_register_fdc_motor_hook(NotifyHookBool fn, const bool* enabled)
{
   auto& slot = g_io_dispatch.fdc_motor_hooks;
   assert(slot.count < MAX_HOOKS);
   slot.entries[slot.count++] = { fn, enabled };
}

// ── Lifecycle ──────────────────────────────────────

void io_dispatch_clear()
{
   g_io_dispatch = IODispatch{};
}

// Forward declarations — each peripheral provides its own registration
extern void amdrum_register_io();
extern void symbiface_register_io();
extern void m4board_register_io();
extern void phazer_register_io();
extern void amx_mouse_register_hooks();
extern void drive_sounds_register_hooks();

// MF2 registration is done by kon_cpc_ja.cpp (uses file-local globals)
extern void mf2_register_io();

void io_dispatch_init()
{
   io_dispatch_clear();

   // Standalone peripheral port handlers
   amdrum_register_io();
   symbiface_register_io();
   m4board_register_io();
   phazer_register_io();
   mf2_register_io();

   // Core hooks
   amx_mouse_register_hooks();
   drive_sounds_register_hooks();
}

// ── Dispatch ───────────────────────────────────────

byte io_dispatch_in(reg_pair port, byte current_val)
{
   const PortSlot& slot = g_io_dispatch.in_slots[port.b.h];
   if (slot.count == 0) return current_val;  // fast path

   for (int i = 0; i < slot.count; i++) {
      const auto& e = slot.entries[i];
      if (*e.enabled) {
         e.in_fn(port, current_val);
      }
   }
   return current_val;
}

void io_dispatch_out(reg_pair port, byte val)
{
   const PortSlot& slot = g_io_dispatch.out_slots[port.b.h];
   if (slot.count == 0) return;  // fast path

   for (int i = 0; i < slot.count; i++) {
      const auto& e = slot.entries[i];
      if (*e.enabled) {
         e.out_fn(port, val);
      }
   }
}
