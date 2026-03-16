/*
 * msvc_compat.h — MSVC portability shim
 *
 * Provides POSIX-ish functions that MSVC lacks but the codebase uses.
 * This header is force-included via /FI in the CMake MSVC build,
 * and explicitly included from koncepcja.h under _MSC_VER.
 *
 * It is a no-op on GCC/Clang.
 */

#ifndef MSVC_COMPAT_H
#define MSVC_COMPAT_H

#ifdef _MSC_VER

/* ── access() / F_OK / R_OK / W_OK ─────────────────────── */
#include <io.h>      /* _access */
#include <direct.h>  /* _getcwd, _mkdir */

#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif

#ifndef access
#define access _access
#endif

#ifndef getcwd
#define getcwd _getcwd
#endif

/* ── strcasecmp / strncasecmp ───────────────────────────── */
#include <string.h>  /* _stricmp, _strnicmp */

#ifndef strcasecmp
#define strcasecmp  _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

/* ── ssize_t ────────────────────────────────────────────── */
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;

/* ── suppress common MSVC warnings for legacy C patterns ── */
#pragma warning(disable : 4996)  /* _CRT_SECURE_NO_WARNINGS */

#endif /* _MSC_VER */
#endif /* MSVC_COMPAT_H */
