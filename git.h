/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file git.h
 * @brief Thin wrappers for invoking the system @c git binary.
 *
 * @c git is a hard runtime dependency for ice (same as for
 * ESP-IDF's setup flow).  Rather than linking libgit or implementing
 * pack-file I/O ourselves, we shell out to the system binary and
 * consume its stdout.  The wrappers here just assemble an argv and
 * hand it to @c process_run() / @c process_start().
 *
 * Typical use:
 *
 *   const char *argv[] = { "git", "clone", url, dest, NULL };
 *   if (git_run(NULL, argv) != 0) die("clone failed");
 *
 *   struct sbuf head = SBUF_INIT;
 *   const char *rev[] = { "git", "rev-parse", "HEAD", NULL };
 *   if (git_capture(dir, rev, &head) == 0)
 *           printf("head = %s\n", head.buf);
 *
 * @c argv must be NULL-terminated and include @c "git" as @c argv[0].
 */
#ifndef GIT_H
#define GIT_H

struct sbuf;

/**
 * @brief Run a git command in @p dir.
 *
 * stdout/stderr inherit the parent's descriptors.  If @p dir is NULL
 * the command runs in the parent's working directory.
 *
 * @return the child's exit code (0 on success), or -1 on spawn failure.
 */
int git_run(const char *dir, const char **argv);

/**
 * @brief Run a git command in @p dir and capture its stdout.
 *
 * Appends stdout bytes to @p out (caller initialises via SBUF_INIT).
 * stderr is left attached to the parent's.
 *
 * @return the child's exit code (0 on success), or -1 on spawn failure.
 */
int git_capture(const char *dir, const char **argv, struct sbuf *out);

#endif /* GIT_H */
