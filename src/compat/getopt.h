/*
 * getopt.h — Minimal getopt_long for MSVC
 *
 * Based on public domain implementations by Gregory Pietsch and
 * AT&T / Keith Bostic. This version provides only the subset needed
 * by konCePCja: getopt_long() with required_argument / no_argument.
 *
 * SPDX-License-Identifier: 0BSD
 */

#ifndef COMPAT_GETOPT_H
#define COMPAT_GETOPT_H

#ifdef __cplusplus
extern "C" {
#endif

extern char *optarg;
extern int   optind;
extern int   opterr;
extern int   optopt;

#define no_argument       0
#define required_argument 1
#define optional_argument 2

struct option {
    const char *name;
    int         has_arg;
    int        *flag;
    int         val;
};

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#endif /* COMPAT_GETOPT_H */
