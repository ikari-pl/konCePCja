#include "amdrum.h"

AmDrum g_amdrum;

void amdrum_reset()
{
   g_amdrum.dac_value = 128;
}
