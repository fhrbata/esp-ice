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

#endif /* FS_H */
