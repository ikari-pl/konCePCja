/* rig_args.h — argument parsing shared by the sim rigs (cpct_tap_rig,
 * psg_oracle_rig). One rule for every numeric option: absence of a valid
 * number is a hard error, never a silent zero. */
#ifndef KONCPC_SIM_RIG_ARGS_H
#define KONCPC_SIM_RIG_ARGS_H

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace rig {

// Strict unsigned parse: the whole string must be a number in [lo, hi]
// (0x.. accepted). atol/strtoul would turn "abc" into 0 and run nothing.
inline bool parse_u64(const char* s, unsigned long long lo,
                      unsigned long long hi, unsigned long long& out) {
  if (!s || !*s || *s == '-') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 0);
  if (errno != 0 || *end != '\0' || v < lo || v > hi) return false;
  out = v;
  return true;
}

// Diagnose a rejected option value; returns the rigs' bad-argument status.
inline int bad_arg(const char* tool, const char* opt, const char* val) {
  std::fprintf(stderr, "%s: bad %s value '%s'\n", tool, opt, val ? val : "");
  return 2;
}

}  // namespace rig

#endif /* KONCPC_SIM_RIG_ARGS_H */
