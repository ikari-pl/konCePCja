#include "amdrum.h"

AmDrum g_amdrum;

void amdrum_reset()
{
   g_amdrum.dac_value = 128;
}

// ── I/O dispatch registration ──────────────────

#include "io_dispatch.h"

static bool amdrum_out_handler(reg_pair /*port*/, byte val)
{
   g_amdrum.dac_value = val;
   return true;
}

void amdrum_register_io()
{
   io_register_out(0xFF, amdrum_out_handler, &g_amdrum.enabled, "AmDrum");
}
