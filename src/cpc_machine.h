// Core CPC machine aggregate — non-owning view of global emulator state.
// Phase 1: wrapper only, no behavior change. Globals remain the source of truth.

#ifndef CPC_MACHINE_H
#define CPC_MACHINE_H

#include "koncepcja.h"

class t_z80regs;

struct CpcMachine {
  t_CPC*       cpc        = nullptr;
  t_CRTC*      crtc       = nullptr;
  t_GateArray* gate_array = nullptr;
  t_FDC*       fdc        = nullptr;
  t_PPI*       ppi        = nullptr;
  t_PSG*       psg        = nullptr;
  t_VDU*       vdu        = nullptr;
  t_drive*     driveA     = nullptr;
  t_drive*     driveB     = nullptr;
  t_z80regs*   z80        = nullptr;
};

extern CpcMachine g_machine;

#endif // CPC_MACHINE_H


