// konCePCja — logging state.

#include "log.h"

// Set once at startup by the -v/--verbose CLI flag; read by the LOG_VERBOSE
// and LOG_DEBUG macros.
bool log_verbose = false;
