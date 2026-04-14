/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmakecache.h
 * @brief Parsed reader for CMakeCache.txt files.
 *
 * Cache lines have the form "KEY:TYPE=VALUE"; blank lines and lines
 * starting with '#' or '//' are comments.  cmakecache_load() parses
 * the file once into a flat list of (key, value) pairs, after which
 * cmakecache_get() is a single linear scan over parsed entries -- no
 * re-parsing of the source buffer per lookup.
 *
 * Usage:
 *   struct cmakecache cache = CMAKECACHE_INIT;
 *   if (cmakecache_load(&cache, "build/CMakeCache.txt") == 0) {
 *       const char *target = cmakecache_get(&cache, "IDF_TARGET");
 *       ...
 *   }
 *   cmakecache_release(&cache);
 */
#ifndef CMAKECACHE_H
#define CMAKECACHE_H

struct cmakecache_entry {
	char *key;
	char *value;
};

struct cmakecache {
	struct cmakecache_entry *entries;
	int nr, alloc;
};

/** Static initializer. */
#define CMAKECACHE_INIT {.entries = NULL, .nr = 0, .alloc = 0}

/** Initialize a cache to empty (equivalent to CMAKECACHE_INIT). */
void cmakecache_init(struct cmakecache *c);

/** Free all entries and reset to empty. */
void cmakecache_release(struct cmakecache *c);

/**
 * @brief Read and parse @p path into @p c.
 *
 * Skips comment lines ("#..." or "//...") and blank lines.  Each
 * "KEY:TYPE=VALUE" line is parsed: KEY (without :TYPE) becomes the
 * entry key and VALUE (rest of line, trimmed of the trailing newline)
 * becomes the entry value.  Strings are duplicated.
 *
 * @return 0 on success, -1 on I/O error (errno is set).
 */
int cmakecache_load(struct cmakecache *c, const char *path);

/**
 * @brief Look up @p key in the parsed cache.
 *
 * @return The NUL-terminated value (owned by @p c), or NULL if @p key
 *         is not present.
 */
const char *cmakecache_get(const struct cmakecache *c, const char *key);

#endif /* CMAKECACHE_H */
