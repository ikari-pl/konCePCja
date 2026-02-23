/* konCePCja - Amstrad CPC Emulator
   I/O Port Dispatch Table

   Peripherals register their IN/OUT handlers here instead of being
   hard-coded in z80_IN_handler / z80_OUT_handler.  Core CPC devices
   (GA, CRTC, PPI, FDC) stay inline in those functions.

   The table is a flat 256-entry array indexed by port.b.h.  Each slot
   holds up to MAX_PORT_HANDLERS entries.  Handlers do their own
   port.b.l narrowing and return true if the port actually matched.

   "Core hooks" modify behavior inside core device handlers (e.g.
   AMX Mouse modifies the PPI keyboard read).  They have typed
   signatures specific to each hook point.
*/

#ifndef IO_DISPATCH_H
#define IO_DISPATCH_H

#include "types.h"
#include "z80.h"

static constexpr int MAX_PORT_HANDLERS = 4;
static constexpr int MAX_HOOKS = 4;

// ── Peripheral I/O handler signatures ──────────────

// IN handler: may modify ret_val, returns true if port matched
using PeriphInHandler  = bool (*)(reg_pair port, byte& ret_val);

// OUT handler: returns true if port matched
using PeriphOutHandler = bool (*)(reg_pair port, byte val);

// ── Port slot ──────────────────────────────────────

struct PortHandlerEntry {
   union {
      PeriphInHandler  in_fn;
      PeriphOutHandler out_fn;
   };
   const bool* enabled;   // pointer to peripheral's enabled flag
   const char* name;      // for debug / devtools display
};

struct PortSlot {
   PortHandlerEntry entries[MAX_PORT_HANDLERS];
   int count = 0;
};

// ── Core hook signatures ───────────────────────────

// Keyboard read hook: called after PPI reads keyboard matrix row.
// Returns an AND mask to apply to ret_val (0xFF = no modification).
using KeyboardReadHook = byte (*)(int keyboard_line);

// Notification hooks: fire-and-forget
using NotifyHookBool = void (*)(bool state);
using NotifyHookInt  = void (*)(int value);

// ── Hook slot templates ────────────────────────────

template<typename Fn>
struct HookEntry {
   Fn fn;
   const bool* enabled;
};

template<typename Fn>
struct HookSlot {
   HookEntry<Fn> entries[MAX_HOOKS];
   int count = 0;
};

// ── Master dispatch table ──────────────────────────

struct IODispatch {
   PortSlot in_slots[256];
   PortSlot out_slots[256];

   HookSlot<KeyboardReadHook> kbd_read_hooks;
   HookSlot<NotifyHookInt>    kbd_line_hooks;
   HookSlot<NotifyHookBool>   tape_motor_hooks;
   HookSlot<NotifyHookBool>   fdc_motor_hooks;
};

extern IODispatch g_io_dispatch;

// ── Registration API ───────────────────────────────

void io_register_in(byte port_high, PeriphInHandler fn,
                    const bool* enabled, const char* name);
void io_register_out(byte port_high, PeriphOutHandler fn,
                     const bool* enabled, const char* name);

void io_register_kbd_read_hook(KeyboardReadHook fn, const bool* enabled);
void io_register_kbd_line_hook(NotifyHookInt fn, const bool* enabled);
void io_register_tape_motor_hook(NotifyHookBool fn, const bool* enabled);
void io_register_fdc_motor_hook(NotifyHookBool fn, const bool* enabled);

// ── Lifecycle ──────────────────────────────────────

void io_dispatch_init();    // register all peripherals
void io_dispatch_clear();   // zero out all tables

// ── Dispatch (called from z80_IN/OUT after core devices) ──

byte io_dispatch_in(reg_pair port, byte current_val);
void io_dispatch_out(reg_pair port, byte val);

// ── Core hook fire functions ───────────────────────

// Returns ANDed mask from all enabled keyboard read hooks
inline byte io_fire_kbd_read_hooks(int line) {
   auto& slot = g_io_dispatch.kbd_read_hooks;
   if (slot.count == 0) return 0xFF;
   byte mask = 0xFF;
   for (int i = 0; i < slot.count; i++) {
      if (*slot.entries[i].enabled)
         mask &= slot.entries[i].fn(line);
   }
   return mask;
}

inline void io_fire_kbd_line_hooks(int line) {
   auto& slot = g_io_dispatch.kbd_line_hooks;
   for (int i = 0; i < slot.count; i++) {
      if (*slot.entries[i].enabled)
         slot.entries[i].fn(line);
   }
}

inline void io_fire_tape_motor_hooks(bool on) {
   auto& slot = g_io_dispatch.tape_motor_hooks;
   for (int i = 0; i < slot.count; i++) {
      if (*slot.entries[i].enabled)
         slot.entries[i].fn(on);
   }
}

inline void io_fire_fdc_motor_hooks(bool on) {
   auto& slot = g_io_dispatch.fdc_motor_hooks;
   for (int i = 0; i < slot.count; i++) {
      if (*slot.entries[i].enabled)
         slot.entries[i].fn(on);
   }
}

#endif
