#pragma once

// konCePCja — stream-style logging macros.
//
// Each LOG_* macro is a self-terminating statement (no trailing ';' needed
// at the call site) whose message argument may chain stream inserters:
//   LOG_ERROR("bad sector " << sector_id << " on track " << track)
//
// The whole line is formatted into one buffer and written with a single
// stream insertion, so concurrent threads cannot interleave partial lines.

#include <iostream>
#include <sstream>

extern bool log_verbose;

// NOLINTNEXTLINE(misc-macro-parentheses): `message` is deliberately
// unparenthesised so call sites can chain `<<` inserters.
#define LOG_TO(stream, level, message)                                         \
  {                                                                            \
    std::ostringstream koncpc_log_line_;                                       \
    koncpc_log_line_ << (level) << " " << __FILE__ << ":" << __LINE__ << " - " \
                     << message << "\n";                                       \
    (stream) << koncpc_log_line_.str() << std::flush;                          \
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
