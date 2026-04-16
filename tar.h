/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tar.h
 * @brief Tar archive extraction (ustar + GNU long names + pax paths).
 *
 * Supports .tar, .tar.gz / .tgz (zlib), and .tar.xz / .txz (XZ
 * Embedded).  Compression is selected from the filename extension.
 *
 * Unsupported: sparse files, ACLs, extended attributes, hardlinks,
 * device nodes.  None of these appear in Espressif toolchain
 * archives; unknown entries are skipped with their data.
 */
#ifndef TAR_H
#define TAR_H

/**
 * @brief Progress callback for tar extraction.
 *
 * Called once for each entry (file, directory, link) extracted.
 *
 * @param name  Entry name being extracted.
 * @param ctx   Opaque context passed to tar_extract_progress.
 */
typedef void (*tar_progress_fn)(const char *name, void *ctx);

/**
 * @brief Extract @p src into @p dest_dir.
 *
 * @p dest_dir must exist.  Missing intermediate directories under it
 * are created as entries are extracted.
 *
 * Entries whose names contain ".." components or are absolute are
 * rejected with a warning (tar-slip protection).
 *
 * @return 0 on success, -1 on any I/O, decode, or format error.
 */
int tar_extract(const char *src, const char *dest_dir);

/**
 * @brief Extract with a per-entry progress callback.
 *
 * Same as tar_extract(), but calls @p progress after each entry.
 * Pass NULL for @p progress for silent extraction.
 */
int tar_extract_progress(const char *src, const char *dest_dir,
			 tar_progress_fn progress, void *ctx);

#endif /* TAR_H */
