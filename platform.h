/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform.h
 * @brief Platform abstraction layer for POSIX / Windows differences.
 *
 * On all platforms:
 *  - Overrides fprintf, printf, vfprintf, fputs, and puts with
 *    color-aware versions that expand @x{...} tokens in format
 *    strings. On POSIX this adds color support; on Windows it also
 *    handles UTF-8 console output.
 *
 * On Windows (_WIN32):
 *  - Provides a ssize_t typedef (not part of the Windows CRT).
 *  - Overrides fopen, access, and mkdir with UTF-8-aware wrappers.
 *  - Defines EOL as "\r\n".
 *
 * On POSIX:
 *  - Defines EOL as "\n".
 *  - Includes <unistd.h> for access() / F_OK.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

/** Mark a function as never returning (for die() and friends). */
#ifdef _MSC_VER
#define NORETURN __declspec(noreturn)
#else
#define NORETURN __attribute__((noreturn))
#endif

/*
 * Color-aware I/O overrides (all platforms).
 *
 * These expand @x{...} color tokens in format strings. On POSIX they
 * emit ANSI escape codes. On Windows they handle both UTF-8 conversion
 * and ANSI/Console API color output.
 */
int fprintf_p(FILE *, const char *, ...);
int vfprintf_p(FILE *, const char *, va_list);
int fputs_p(const char *, FILE *);

#define fprintf fprintf_p
#define printf(...) fprintf_p(stdout, __VA_ARGS__)
#define vfprintf vfprintf_p
#define vprintf(fmt, args) vfprintf_p(stdout, fmt, args)
#define fputs fputs_p
#define puts(s) fputs_p(s "\n", stdout)

#ifdef _WIN32

typedef ptrdiff_t ssize_t;

/*
 * Include <io.h> early -- before our mkdir/access/fopen macros --
 * because MSYS2's <io.h> declares its own mkdir(const char *) and
 * the macro would conflict with that prototype.
 */
#include <io.h>

/* UTF-8-aware filesystem function replacements (Windows only). */
FILE *fopen_w(const char *, const char *);
int access_w(const char *, int);
int mkdir_w(const char *, int);

#define F_OK 0
#define access access_w
#define fopen fopen_w
#define mkdir mkdir_w
#define isatty _isatty
#define putenv _putenv
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/** End-of-line sequence (CRLF on Windows). */
#define EOL "\r\n"

#else /* POSIX */

/** End-of-line sequence (LF on POSIX). */
#define EOL "\n"

#include <sys/stat.h>
#include <unistd.h>

/* putenv exists in libc but is not declared in C99 <stdlib.h>. */
int putenv(char *);

#endif /* _WIN32 */

/**
 * @brief Get terminal width for a given file descriptor.
 *
 * @param fd  File descriptor (typically STDOUT_FILENO or STDERR_FILENO).
 * @return Terminal width in columns, or 80 if not a terminal or unknown.
 */
int term_width(int fd);

#endif /* PLATFORM_H */
