#pragma once

// konCePCja — stream-style logging macros.
//
// Each LOG_* macro is a self-terminating statement (no trailing ';' needed
// at the call site) whose message argument may chain stream inserters:
//   LOG_ERROR("bad sector " << sector_id << " on track " << track)
//
// The whole line is formatted into one buffer first, then written by
// log_write_line() under the one lock every LOG_* macro shares: the macros run
// on the Z80, render, IPC, telnet, M4 HTTP and KCPC threads at once, and
// std::cerr/std::cout are not safe to write from two threads together
// (beads-2qcw). The lock is only taken for a line that is really written.

#include <iostream>
#include <sstream>
#include <string>

extern bool log_verbose;

// Writes one finished line to `stream` and flushes it, under the log lock.
void log_write_line(std::ostream& stream, const std::string& line);

// NOLINTNEXTLINE(misc-macro-parentheses): `message` is deliberately
// unparenthesised so call sites can chain `<<` inserters.
#define LOG_TO(stream, level, message)                                         \
  {                                                                            \
    std::ostringstream koncpc_log_line_;                                       \
    koncpc_log_line_ << (level) << " " << __FILE__ << ":" << __LINE__ << " - " \
                     << message << "\n";                                       \
    log_write_line((stream), koncpc_log_line_.str());                          \
  }

#define LOG_ERROR(message) LOG_TO(std::cerr, "ERROR  ", message)
#define LOG_WARNING(message) LOG_TO(std::cerr, "WARNING", message)
#define LOG_INFO(message) LOG_TO(std::cerr, "INFO   ", message)
#define LOG_VERBOSE(message)              \
  if (log_verbose) {                      \
    LOG_TO(std::cout, "VERBOSE", message) \
  }

#ifdef DEBUG
#define LOG_DEBUG(message)                \
  if (log_verbose) {                      \
    LOG_TO(std::cout, "DEBUG  ", message) \
  }
#else
#define LOG_DEBUG(message)
#endif
