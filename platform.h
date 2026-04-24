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
 *    platform/posix/process.c or platform/win/process.c.
 *
 * On Windows (_WIN32):
 *  - Provides a ssize_t typedef (not part of the Windows CRT).
 *  - Overrides fopen, access, mkdir, open with UTF-8-aware wrappers.
 *  - Defines EOL as "\r\n".
 *
 * On POSIX:
 *  - Defines EOL as "\n".
 *  - Includes <unistd.h> for access() / F_OK.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
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
 * Include <io.h> and <fcntl.h> early -- before our mkdir/access/fopen/
 * open macros -- because MSYS2's headers declare their own versions
 * and the macros would conflict with those prototypes.
 */
#include <direct.h>
#include <fcntl.h>
#include <io.h>

/* UTF-8-aware filesystem function replacements (Windows only). */
FILE *fopen_w(const char *, const char *);
int access_w(const char *, int);
int mkdir_w(const char *, int);
int unlink_w(const char *);
int rmdir_w(const char *);
int rename_w(const char *oldp, const char *newp);
int open_w(const char *path, int flags, int mode);
int chdir_w(const char *path);
int chmod_w(const char *path, int mode);
char *getcwd_w(char *buf, size_t size);

/*
 * POSIX symlink(2) / link(2) shims.  symlink_w is a no-op that
 * returns 0: NTFS symlinks require elevated privileges (or Developer
 * Mode), and Espressif Windows toolchain archives don't use them
 * anyway (Unix archives' gcc → cc symlink is a separate .exe on
 * Windows).  link_w uses CreateHardLinkW; both paths must live on
 * the same volume and the filesystem must be NTFS.
 */
int symlink_w(const char *target, const char *linkpath);
int link_w(const char *target, const char *linkpath);

/*
 * UTF-8-aware setenv() replacement.  The narrow _putenv_s interprets
 * its arguments through the current ANSI code page, which mangles
 * non-ASCII bytes in UTF-8 paths / values.  setenv_w converts to wide
 * char first and calls _wputenv_s so the environment block (stored as
 * UTF-16 by Windows) keeps the original content intact.  Copies both
 * inputs -- caller is free to release source buffers.
 */
int setenv_w(const char *name, const char *value, int overwrite);

/*
 * X_OK is not a meaningful mode for the Win32 _access() CRT call, but
 * it is the semantic POSIX callers want ("can I spawn this?").  We
 * define the POSIX bit here and let access_w() interpret it by trying
 * the bare path plus a small set of PATHEXT extensions.
 */
#define F_OK 0
#define X_OK 1
#define access access_w
#define fopen fopen_w
#define mkdir mkdir_w
#define unlink unlink_w
#define rmdir rmdir_w
#define rename rename_w
#define symlink symlink_w
#define link link_w
#define open open_w
#define chdir chdir_w
#define chmod chmod_w
#define isatty _isatty
#define setenv setenv_w
#define fileno _fileno
#define dup _dup
#define dup2 _dup2
#define getcwd getcwd_w
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/** End-of-line sequence (CRLF on Windows). */
#define EOL "\r\n"

/** Environment variable that holds the user's home directory. */
#define HOME_ENV "USERPROFILE"

/** PATH entry separator (';' on Windows, ':' on POSIX). */
#define PATH_SEP_CHAR ';'

/**
 * Default state of @c use_vt (ANSI escape-sequence rendering).
 * Windows consoles may or may not have VT processing enabled at
 * startup; leave the flag off and let the console bootstrap code
 * turn it on after it succeeds in enabling the mode.  POSIX
 * terminals always render ANSI, so default on there.
 */
#define PLATFORM_ANSI_VT_DEFAULT 0

/**
 * Path to the interpreter binary inside a @c{python -m venv} tree,
 * relative to the venv root.  Windows venvs place the interpreter
 * under @c{Scripts\\python.exe}; POSIX venvs use @c{bin/python3}.
 */
#define PLATFORM_VENV_PYTHON_REL "Scripts/python.exe"

#else /* POSIX */

/** End-of-line sequence (LF on POSIX). */
#define EOL "\n"

/** Environment variable that holds the user's home directory. */
#define HOME_ENV "HOME"

/** PATH entry separator (';' on Windows, ':' on POSIX). */
#define PATH_SEP_CHAR ':'

/** POSIX terminals always render ANSI escape sequences. */
#define PLATFORM_ANSI_VT_DEFAULT 1

/**
 * Path to the interpreter binary inside a @c{python -m venv} tree,
 * relative to the venv root.  See the Windows branch above.
 */
#define PLATFORM_VENV_PYTHON_REL "bin/python3"

#include <unistd.h>

/* setenv exists in libc but is hidden from C99 <stdlib.h> without
 * _POSIX_C_SOURCE; the main build defines it but test drivers compile
 * individual files with plain -std=c99, so the prototype lives here. */
int setenv(const char *name, const char *value, int overwrite);

/* fileno exists in libc but is hidden from C99 <stdio.h> without
 * _POSIX_C_SOURCE; platform.h maps it to _fileno on Windows. */
int fileno(FILE *stream);

#endif /* _WIN32 */

/**
 * @brief Sleep for @p ms milliseconds.
 *
 * Uses nanosleep() on POSIX and Sleep() on Windows.
 * Implementation lives next to mono_ms() in platform/{posix,win}/process.c.
 */
void delay_ms(uint32_t ms);

/**
 * @brief Get terminal width for a given file descriptor.
 *
 * @param fd  File descriptor (typically STDOUT_FILENO or STDERR_FILENO).
 * @return Terminal width in columns, or 80 if not a terminal or unknown.
 */
int term_width(int fd);

/**
 * @brief Test whether @p path refers to a directory.
 *
 * @return 1 if the path exists and is a directory, 0 otherwise
 *         (non-existent, not a directory, or stat failure).
 */
int is_directory(const char *path);

/**
 * @brief Iterate over the entries of a directory.
 *
 * Reads every entry in @p path (skipping "." and "..") into an
 * internal buffer, closes the directory, and then calls @p cb once per
 * entry.  The two-pass design lets the callback safely mutate @p path
 * (unlink, rmdir, rename entries) -- single-pass readdir would be
 * unspecified after modification.
 *
 * Iteration stops when the callback returns non-zero; that value is
 * returned to the caller.
 *
 * @param path  Directory to enumerate (UTF-8 on Windows).
 * @param cb    Invoked with each entry's bare name (no path prefix).
 * @param ud    Opaque pointer passed through to @p cb.
 * @return 0 on success, -1 if @p path cannot be opened, or the first
 *         non-zero value returned by @p cb.
 */
int dir_foreach(const char *path, int (*cb)(const char *name, void *ud),
		void *ud);

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
	/**
	 * If set, argv[0] is a shell command line executed via the
	 * platform shell (@c /bin/sh @c -c on POSIX, @c cmd.exe @c /c on
	 * Windows); other argv entries are ignored.  Use this for
	 * spawning user-configurable commands (pagers, aliases, editors)
	 * without hand-splitting the command string.
	 */
	unsigned use_shell : 1;
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

/**
 * @brief Read from a pipe file descriptor with a timeout.
 *
 * Waits up to @p timeout_ms milliseconds for data, then reads whatever
 * is buffered into @p buf (up to @p n bytes).  Designed for reading
 * the output side of a pipe created by process_start() (proc->out or
 * proc->err) while keeping the parent responsive -- e.g. to animate a
 * progress indicator between bursts of child output.
 *
 * POSIX uses select() + read(); Windows uses PeekNamedPipe() in a
 * short poll loop + ReadFile().  Anonymous pipes on Windows are not
 * signalable objects, so WaitForSingleObject() is not an option there.
 *
 * @param fd          Read end of the pipe (e.g. proc->out).
 * @param buf         Destination buffer.
 * @param n           Maximum bytes to read.
 * @param timeout_ms  Maximum time to wait for data (0 = poll once).
 * @return bytes read (> 0), 0 on timeout, -1 on EOF or error.
 */
ssize_t pipe_read_timed(int fd, void *buf, size_t n, unsigned timeout_ms);

/**
 * @brief Return a monotonic timestamp in milliseconds.
 *
 * Suitable for measuring elapsed intervals inside a single process
 * run.  The value is monotonic (never decreases across wall-clock
 * adjustments) but its epoch is unspecified -- only differences are
 * meaningful.  Resolution is ~1 ms on both platforms.
 *
 * POSIX uses clock_gettime(CLOCK_MONOTONIC); Windows uses
 * GetTickCount64().
 */
unsigned long long mono_ms(void);

#endif /* PLATFORM_H */
