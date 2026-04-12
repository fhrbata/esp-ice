/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ar.h
 * @brief AR (static library) archive reader.
 *
 * Sequential reader for Unix AR archives as produced by GNU binutils
 * ar.  Supports both GNU and BSD long-name extensions.  Special members
 * (symbol table, string table) are consumed internally and never
 * exposed to the caller.
 *
 * Usage:
 *   struct sbuf sb = SBUF_INIT;
 *   sbuf_read_file(&sb, "libfoo.a");
 *
 *   struct ar_reader r;
 *   struct ar_member m;
 *   ar_reader_init(&r, sb.buf, sb.len);
 *   while (ar_reader_next(&r, &m)) {
 *       printf("%s (%zu bytes)\n", m.name, m.size);
 *       free(m.name);
 *   }
 *   sbuf_release(&sb);
 */
#ifndef AR_H
#define AR_H

#include <stddef.h>

/**
 * A single member (file) within an AR archive.
 *
 * @p name is heap-allocated; the caller must free() it after use.
 * @p data points into the original archive buffer (not a copy);
 * the caller must keep the buffer alive while using @p data.
 */
struct ar_member {
	char *name;		/**< Member name (heap-allocated, caller frees). */
	const void *data;	/**< Member data (points into archive buffer). */
	size_t size;		/**< Member data size in bytes. */
};

/**
 * Sequential reader state for an AR archive.
 *
 * Initialized with ar_reader_init(); iterated with ar_reader_next().
 * No cleanup is needed -- all pointers reference the original buffer.
 */
struct ar_reader {
	const unsigned char *buf;
	size_t len;
	size_t pos;
	const char *strtab;	/**< GNU long-name string table (or NULL). */
	size_t strtab_len;
};

/**
 * @brief Initialize an AR reader over a memory buffer.
 *
 * Validates the archive global header ("!<arch>\n").
 * Dies if the buffer does not start with a valid AR magic.
 *
 * @param r    reader to initialize
 * @param buf  archive data (caller retains ownership)
 * @param len  archive data length
 */
void ar_reader_init(struct ar_reader *r, const void *buf, size_t len);

/**
 * @brief Read the next archive member.
 *
 * Skips special members (symbol table, string table) automatically.
 * On success, fills in @p m and returns 1.  The caller must free()
 * m->name when done.
 *
 * @param r  reader state
 * @param m  output member (filled on success)
 * @return   1 if a member was read, 0 at end of archive
 */
int ar_reader_next(struct ar_reader *r, struct ar_member *m);

#endif /* AR_H */
