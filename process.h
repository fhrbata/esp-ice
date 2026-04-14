/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file process.h
 * @brief Cross-platform child process API.
 *
 * Provides a struct-based interface for spawning child processes with
 * optional pipe redirection for stdin/stdout/stderr.
 *
 * On POSIX this uses fork()/execvp()/waitpid().
 * On Windows this uses CreateProcessW()/WaitForSingleObject().
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
#ifndef PROCESS_H
#define PROCESS_H

#include <sys/types.h>

#include "platform.h"

/**
 * @brief Child process descriptor.
 *
 * Set argv and optional flags before calling process_start() or
 * process_run(). After process_start(), the pipe file descriptors
 * (in/out/err) are populated if the corresponding pipe_* flag was set.
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

#endif /* PROCESS_H */
