/*
 * getopt.c — Minimal getopt / getopt_long for MSVC
 *
 * Provides the standard getopt() and getopt_long() interface sufficient
 * for konCePCja's argument parsing.  No optional_argument support beyond
 * the struct definition.
 *
 * SPDX-License-Identifier: 0BSD
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef _MSC_VER

#include "getopt.h"
#include <stdio.h>
#include <string.h>

char *optarg = NULL;
int   optind = 1;
int   opterr = 1;
int   optopt = '?';

/* Internal helper: reset state between calls */
static int sp = 1;

static int
getopt_internal(int argc, char *const argv[], const char *optstring)
{
    int c;
    const char *cp;

    if (sp == 1) {
        /* Check for end of options */
        if (optind >= argc ||
            argv[optind][0] != '-' ||
            argv[optind][1] == '\0') {
            return -1;
        }
        if (strcmp(argv[optind], "--") == 0) {
            optind++;
            return -1;
        }
    }

    optopt = c = argv[optind][sp];
    if (c == ':' || (cp = strchr(optstring, c)) == NULL) {
        if (opterr)
            fprintf(stderr, "%s: unknown option '-%c'\n", argv[0], c);
        if (argv[optind][++sp] == '\0') {
            optind++;
            sp = 1;
        }
        return '?';
    }

    if (*++cp == ':') {
        /* Option takes an argument */
        if (argv[optind][sp + 1] != '\0') {
            optarg = &argv[optind++][sp + 1];
        } else if (++optind >= argc) {
            if (opterr)
                fprintf(stderr, "%s: option '-%c' requires an argument\n",
                        argv[0], c);
            sp = 1;
            return '?';
        } else {
            optarg = argv[optind++];
        }
        sp = 1;
    } else {
        /* No argument */
        if (argv[optind][++sp] == '\0') {
            sp = 1;
            optind++;
        }
        optarg = NULL;
    }

    return c;
}

int
getopt_long(int argc, char *const argv[], const char *optstring,
            const struct option *longopts, int *longindex)
{
    int i;

    /* Handle optind=0 reset (used by argparse.cpp's parseArguments) */
    if (optind == 0) {
        optind = 1;
        optarg = NULL;
        sp = 1;
    }

    if (optind >= argc)
        return -1;

    /* Check for "--" long option */
    if (argv[optind][0] == '-' && argv[optind][1] == '-') {
        const char *arg = argv[optind] + 2;
        const char *eq;
        size_t namelen;

        if (*arg == '\0') {
            /* bare "--" ends option processing */
            optind++;
            return -1;
        }

        /* Check for "=value" */
        eq = strchr(arg, '=');
        namelen = eq ? (size_t)(eq - arg) : strlen(arg);

        for (i = 0; longopts[i].name != NULL; i++) {
            if (strncmp(arg, longopts[i].name, namelen) == 0 &&
                longopts[i].name[namelen] == '\0') {
                /* Found a match */
                if (longindex)
                    *longindex = i;

                if (longopts[i].has_arg == required_argument ||
                    longopts[i].has_arg == optional_argument) {
                    if (eq) {
                        optarg = (char *)(eq + 1);
                    } else if (longopts[i].has_arg == required_argument) {
                        optind++;
                        if (optind >= argc) {
                            if (opterr)
                                fprintf(stderr,
                                        "%s: option '--%s' requires an argument\n",
                                        argv[0], longopts[i].name);
                            return '?';
                        }
                        optarg = argv[optind];
                    } else {
                        optarg = NULL;
                    }
                } else {
                    if (eq && opterr) {
                        fprintf(stderr,
                                "%s: option '--%s' doesn't allow an argument\n",
                                argv[0], longopts[i].name);
                    }
                    optarg = NULL;
                }

                optind++;

                if (longopts[i].flag != NULL) {
                    *longopts[i].flag = longopts[i].val;
                    return 0;
                }
                return longopts[i].val;
            }
        }

        if (opterr)
            fprintf(stderr, "%s: unrecognized option '--%.*s'\n",
                    argv[0], (int)namelen, arg);
        optind++;
        return '?';
    }

    /* Short option: delegate to getopt_internal */
    if (argv[optind][0] == '-' && argv[optind][1] != '\0') {
        return getopt_internal(argc, argv, optstring);
    }

    /* Not an option */
    return -1;
}

#endif /* _MSC_VER */
