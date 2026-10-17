// konCePCja — logging state.

#include "log.h"

#include <mutex>

// Set once at startup by the -v/--verbose CLI flag; read by the LOG_VERBOSE
// and LOG_DEBUG macros.
bool log_verbose = false;

namespace {
// Constant-initialised, so a line logged during static initialisation finds
// it ready.
std::mutex g_log_mutex;
}  // namespace

void log_write_line(std::ostream& stream, const std::string& line) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  stream.write(line.data(), static_cast<std::streamsize>(line.size()));
  stream.flush();
}
