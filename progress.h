/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file progress.h
 * @brief Run a child process with a single-line progress indicator.
 *
 * Orchestrator commands (build, flash, init, ...) spawn tools whose
 * output is noisy but only interesting on failure.  process_run_progress
 * captures the full stdout/stderr to a log file under
 * ~/.ice/logs -- always -- and either draws a spinner line (quiet
 * mode) or streams the output live (verbose).  The log path is fixed
 * by the helper so every call-site produces the same shape of
 * artefact, and the path is printed on failure so the user (or the
 * future `ice hint` command) can scan it for known patterns.
 */
#ifndef PROGRESS_H
#define PROGRESS_H

struct process;

/**
 * @brief Run a child process, teeing its merged output to a log file.
 *
 * Forces @c pipe_out and @c merge_err on @p proc, starts it, and
 * pumps the combined stdout+stderr into a per-invocation log file at
 * @c ~/.ice/logs/YYYYMMDD-HHMMSS-<slug>.log -- the directory tree is
 * created on demand.
 *
 * In quiet mode (@c global_verbose == 0) a single status line
 * "<spinner> @p msg (<elapsed>)" is drawn on stdout; on completion
 * the line is replaced with "@c{✓} @p msg done. (<elapsed>)" on
 * success or "@c{✗} @p msg failed. (<elapsed>)" on failure.  In
 * verbose mode the captured bytes are also written to stdout in real
 * time and no spinner is drawn, but the final status line is still
 * emitted and the log file is still produced identically.
 *
 * On non-zero exit codes, a @ref hint() line is emitted pointing at
 * the log file so the user can inspect it.
 *
 * The caller must set @c proc->argv (and may set @c dir, @c env);
 * the pipe_* / merge_err flags are set internally and will be
 * overwritten.
 *
 * @param proc  Process descriptor (argv must be set).
 * @param msg   Progress line text, e.g. "Building".
 * @param slug  Short identifier embedded in the log filename, e.g.
 *              "build", "flash", "init-cmake".  Must be safe as a
 *              path component.
 * @return The child's exit code, or -1 on spawn or I/O failure.
 */
int process_run_progress(struct process *proc, const char *msg,
			 const char *slug);

#endif /* PROGRESS_H */
