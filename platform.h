/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform.h
 * @brief Per-OS primitives for POSIX / Windows.
 *
 * This header and the `platform/` directory exist for APIs that
 * genuinely need different code on POSIX and Windows: UTF-8 path
 * wrappers, console output, child-process spawning, and similar.
 * Portable helpers built on top of these primitives (mkdirp, rmtree,
 * CSV readers, ...) live at the project root instead.
 *
 * Rule of thumb: if an implementation requires `#ifdef _WIN32`, it
 * belongs here; if it's ordinary C that only calls into the primitives
 * below, it doesn't.
 *
 * On all platforms:
 *  - Overrides fprintf, printf, vfprintf, fputs, and puts with
 *    color-aware versions that expand @x{...} tokens in format
 *    strings. On POSIX this adds color support; on Windows it also
 *    handles UTF-8 console output.
 *  - Provides the child-process API (struct process, process_start,
 *    process_finish, process_run); implementation lives in
 *    platform/<os>/posix_process.c or platform/win/process.c.
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

#include <sys/types.h>

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

#include <sys/stat.h>

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

/**
 * @brief Return the absolute path of the running executable.
 *
 * Resolved on first call via the platform's native mechanism:
 * /proc/self/exe on Linux, _NSGetExecutablePath on macOS,
 * GetModuleFileName on Windows.  Cached for the lifetime of the
 * process.
 *
 * @return Pointer to a static buffer holding the path, or NULL if
 *   the native query failed.  Callers that need a usable name on
 *   failure should fall back to "ice" and rely on PATH lookup.
 */
const char *process_exe(void);

/* ------------------------------------------------------------------ */
/*  Child process API                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Child process descriptor.
 *
 * Set argv and optional flags before calling process_start() or
 * process_run(). After process_start(), the pipe file descriptors
 * (in/out/err) are populated if the corresponding pipe_* flag was set.
 *
 * Basic usage (inherit terminal, no pipes):
 *   const char *argv[] = {"ninja", "-C", "build", NULL};
 *   struct process proc = PROCESS_INIT;
 *   proc.argv = argv;
 *   int rc = process_run(&proc);
 *
 * With pipes:
 *   struct process proc = PROCESS_INIT;
 *   proc.argv = argv;
 *   proc.pipe_out = 1;
 *   process_start(&proc);
 *   while ((n = read(proc.out, buf, sizeof(buf))) > 0)
 *       ...;
 *   int rc = process_finish(&proc);
 */
struct process {
	const char **argv; /**< NULL-terminated argument vector. */
	const char *dir;   /**< Working directory (NULL = inherit). */
	const char **env; /**< Extra env vars ("KEY=VALUE"), NULL-terminated. */
	pid_t pid;	  /**< Child PID (set by process_start). */
	int in;		  /**< fd to child's stdin  (caller writes). */
	int out;	  /**< fd to child's stdout (caller reads). */
	int err;	  /**< fd to child's stderr (caller reads). */
	unsigned pipe_in : 1;	/**< Create pipe for stdin. */
	unsigned pipe_out : 1;	/**< Create pipe for stdout. */
	unsigned pipe_err : 1;	/**< Create pipe for stderr. */
	unsigned merge_err : 1; /**< Redirect stderr to stdout. */
};

/** Static initializer for struct process. */
#define PROCESS_INIT {0}

/**
 * @brief Start a child process.
 *
 * Spawns the command in argv[0] with the given arguments. If pipe_in,
 * pipe_out, or pipe_err are set, the corresponding file descriptors
 * are created and connected to the child's stdin/stdout/stderr.
 * Otherwise the child inherits the parent's terminal.
 *
 * @param proc  Process descriptor (argv must be set).
 * @return 0 on success, -1 on error.
 */
int process_start(struct process *proc);

/**
 * @brief Wait for a child process to finish and clean up.
 *
 * Closes any open pipe file descriptors and waits for the child to
 * terminate.
 *
 * @param proc  Process descriptor (must have been started).
 * @return Exit code of the child, or -1 on error.
 */
int process_finish(struct process *proc);

/**
 * @brief Run a child process and wait for it to finish.
 *
 * Convenience wrapper: process_start() + process_finish().
 * Best for simple cases where the child inherits the terminal.
 *
 * @param proc  Process descriptor (argv must be set).
 * @return Exit code of the child, or -1 on error.
 */
static inline int process_run(struct process *proc)
{
	if (process_start(proc))
		return -1;
	return process_finish(proc);
}

#endif /* PLATFORM_H */
