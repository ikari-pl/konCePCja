/*
 * msvc_compat.h — MSVC portability shim
 *
 * Provides POSIX-ish functions that MSVC lacks but the codebase uses.
 * Included from koncepcja.h under _MSC_VER.
 *
 * It is a no-op on GCC/Clang.
 */

#ifndef MSVC_COMPAT_H
#define MSVC_COMPAT_H

#ifdef _MSC_VER

#include <io.h>      /* _access */
#include <direct.h>  /* _getcwd, _mkdir */

/* F_OK / R_OK / W_OK constants (MSVC doesn't define these) */
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif

/* POSIX-to-MSVC function name mappings.
 * We do NOT #define access/getcwd as macros — that poisons Windows SDK
 * headers (stdlib.h, intrin.h) which use these names internally.
 * Instead, call sites must use _access/_getcwd on MSVC. */

/* S_ISDIR / S_ISREG — MSVC's <sys/stat.h> doesn't define the POSIX macros */
#include <sys/stat.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

/* strcasecmp / strncasecmp — safe to macro since no SDK header uses these */
#include <string.h>
#ifndef strcasecmp
#define strcasecmp  _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

/* ssize_t */
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;

/* suppress common MSVC deprecation warnings for POSIX names */
#pragma warning(disable : 4996)

#endif /* _MSC_VER */
#endif /* MSVC_COMPAT_H */
