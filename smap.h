/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file smap.h
 * @brief String-keyed hash map (string -> void *).
 *
 * Open-addressed hash map with linear probing and FNV-1a hashing.
 * Keys are copied on insert and freed on remove / release; values are
 * opaque @c void* and the caller owns whatever they point at.  All
 * allocating operations die() on failure.
 *
 * Capacity is always zero or a power of two.  The table grows (x2)
 * once the occupied slot count exceeds 70% of capacity.  Removal
 * rehashes neighbouring entries in place, so iteration never sees
 * tombstones.
 *
 * Usage:
 *   struct smap m = SMAP_INIT;
 *   smap_put(&m, "foo", foo_ptr);
 *   struct foo *f = smap_get(&m, "foo");
 *   smap_remove(&m, "foo");
 *   smap_release(&m);
 */
#ifndef SMAP_H
#define SMAP_H

#include <stddef.h>
#include <stdint.h>

struct smap_entry {
	char *key;     /**< Owned copy; NULL = empty slot. */
	void *val;     /**< Opaque value; caller-owned. */
	uint32_t hash; /**< Cached FNV-1a hash of @p key. */
};

struct smap {
	struct smap_entry *entries; /**< Slot array (NULL when alloc == 0). */
	size_t nr;		    /**< Occupied slots. */
	size_t alloc;		    /**< Capacity; 0 or a power of two. */
};

/** Static initializer -- equivalent to zero-init. */
#define SMAP_INIT {0}

/** Initialize an smap (equivalent to SMAP_INIT assignment). */
void smap_init(struct smap *m);

/** Free the table and all key copies; values are not touched. */
void smap_release(struct smap *m);

/**
 * @brief Insert or update a binding.
 *
 * If @p key already maps to a value, that value is overwritten and
 * returned; otherwise NULL is returned and a copy of @p key is stored.
 */
void *smap_put(struct smap *m, const char *key, void *val);

/** Look up @p key.  Returns the stored value, or NULL if absent. */
void *smap_get(const struct smap *m, const char *key);

/**
 * @brief Remove @p key.  Returns the removed value, or NULL if absent.
 *
 * Rehashes any neighbouring entries that were displaced by the
 * probe-chain through @p key's slot, so iteration remains dense.
 */
void *smap_remove(struct smap *m, const char *key);

/**
 * @brief Iterate over all bindings.
 *
 * Usage -- initialize the cursor to 0 and loop:
 *
 *   const char *key;
 *   void *val;
 *   size_t it = 0;
 *   while (smap_iter(&m, &it, &key, &val))
 *       use(key, val);
 *
 * Returns 1 on each yielded entry, 0 when exhausted.  Mutating @p m
 * during iteration invalidates the cursor.
 */
int smap_iter(const struct smap *m, size_t *cursor, const char **key,
	      void **val);

#endif /* SMAP_H */
