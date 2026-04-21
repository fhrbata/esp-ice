/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fs.h
 * @brief Portable filesystem helpers built on the platform primitives.
 *
 * These sit at the project root because the recursion / path walking
 * is ordinary C on top of mkdir(), is_directory(), dir_foreach(),
 * unlink(), rmdir() — all of which platform.h already abstracts.
 */
#ifndef FS_H
#define FS_H

#include <stddef.h>

/**
 * @brief Create a directory and any missing intermediate parents.
 *
 * Equivalent to `mkdir -p`.  Treats EEXIST on an existing component
 * as success.  Accepts both '/' and '\\' as separators.  On Windows,
 * a leading "X:" drive prefix is skipped so mkdir() is not called on
 * a drive root.  Trailing separators are tolerated.
 *
 * @param dir  Directory path to create (UTF-8 on Windows).
 * @return 0 on success, -1 on the first non-EEXIST mkdir failure
 *         (errno is that of the failing mkdir).
 */
int mkdirp(const char *dir);

/**
 * @brief Create the parent directory tree of a file path.
 *
 * Strips the final path component (the filename) and calls mkdirp()
 * on the remainder.  A no-op if @p path has no directory separator.
 *
 * @param path  File path whose parents to create (UTF-8 on Windows).
 * @return 0 on success, -1 on mkdir failure.
 */
int mkdirp_for_file(const char *path);

/**
 * @brief Write @p len bytes of @p data to @p path atomically.
 *
 * The content goes to `@p path.tmp` first and is renamed onto @p path
 * on success.  `rename()` is atomic on POSIX and atomic-replace on
 * Windows (via rename_w / MoveFileExW), so a crash mid-write leaves
 * the original @p path untouched -- no truncate-then-partial-write
 * window.
 *
 * On failure the tmp file is removed; errno reflects the failing call.
 *
 * @return 0 on success, -1 on any I/O failure.
 */
int write_file_atomic(const char *path, const void *data, size_t len);

/**
 * @brief Recursively delete the contents of @p path.
 *
 * The directory @p path itself is not removed — the caller decides
 * whether to rmdir() it afterwards.  Errors on individual entries are
 * reported via warn_errno() and cause the overall return to be -1,
 * but iteration continues so a partial cleanup still makes progress.
 *
 * @param path     Directory whose contents to remove.
 * @param verbose  If non-zero, print every removed entry to stdout.
 * @return 0 on full success, -1 if any entry could not be removed or
 *         @p path could not be opened.
 */
int rmtree(const char *path, int verbose);

/**
 * @brief Check whether @p name is an executable on PATH.
 * @return 1 if found and executable, 0 otherwise.
 */
int find_in_path(const char *name);

/**
 * @brief Acquire an exclusive advisory lock by creating @p path.
 *
 * Uses open(O_CREAT | O_EXCL) to atomically create @p path (git's
 * "<filename>.lock" pattern).  If the file already exists, retries on
 * a 100 ms cadence until @p timeout_ms has elapsed, then fails with
 * errno == EEXIST so the caller can report that another ice process
 * holds the lock.  @p timeout_ms == 0 fails immediately on EEXIST
 * with no retries.
 *
 * The parent directory of @p path is created on demand (mkdir -p),
 * so callers do not have to precreate it.  On success @p path is
 * registered with atexit() so the lock is removed even if die() is
 * called before lock_release().
 *
 * Works cross-filesystem and cross-platform -- the atomicity is
 * guaranteed by the kernel, not by filesystem semantics.  Signals
 * that bypass atexit (SIGKILL, default SIGINT) will leave the file
 * behind; callers should document how to remove stale locks.
 *
 * @return 0 on success, -1 on failure (errno set).
 */
int lock_acquire(const char *path, unsigned timeout_ms);

/**
 * @brief Release a lock previously acquired with lock_acquire().
 *
 * Unlinks @p path and deregisters it from the atexit cleanup list.
 * Safe to call after a successful lock_acquire only -- releasing a
 * lock we don't own would unlink another process's file.
 */
void lock_release(const char *path);

/**
 * @brief Deregister a lock path from the atexit cleanup list without
 *        unlinking it.
 *
 * Used by atomic-write patterns that rename(lockfile, target): the
 * lockfile name has been consumed by the rename, so the exit handler
 * must not unlink it -- otherwise a future re-acquisition of the same
 * lockfile path by another process could be torn down by our atexit.
 * Silent no-op if @p path is not in the registry.
 */
void lock_forget(const char *path);

#endif /* FS_H */
