#include "phazer.h"
#include "koncepcja.h"
#include "crtc.h"
#include "io_dispatch.h"

extern t_CPC CPC;
extern t_CRTC CRTC;

std::string PhazerType::ToString()
{
  switch(value)
  {
    case None: return "off";
    case AmstradMagnumPhaser: return "Amstrad Magnum Phaser";
    case TrojanLightPhazer: return "Trojan Light Phazer";
    default: return "Unimplemented";
  }
}

PhazerType PhazerType::Next()
{
  auto new_value = value + 1;
  if (new_value == LastPhazerType)
  {
    return PhazerType(None);
  }
  return PhazerType(Value(new_value));
}

// ── I/O dispatch registration ──────────────────

// The phazer can be toggled at runtime via F-keys.  Rather than
// keeping a bool in sync with CPC.phazer_emulation (a PhazerType
// enum), we register with an always-true flag and let the handler
// check CPC.phazer_emulation directly.
static bool s_phazer_always_registered = true;

static bool phazer_out_handler(reg_pair port, byte /*val*/)
{
   if (port.b.l != 0xFE) return false;
   if (!CPC.phazer_emulation) return false;
   // When the phazer is not pressed, the CRTC is constantly refreshing R16 & R17
   if (!CPC.phazer_pressed) CRTC.registers[17] += 1;
   return true;
}

void phazer_register_io()
{
   io_register_out(0xFB, phazer_out_handler, &s_phazer_always_registered, "Magnum Phazer");
}
