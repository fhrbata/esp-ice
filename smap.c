/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file smap.c
 * @brief String-keyed hash map implementation.
 */
#include "ice.h"

#define SMAP_INITIAL_CAP 16
#define SMAP_LOAD_NUM 7
#define SMAP_LOAD_DEN 10

void smap_init(struct smap *m)
{
	m->entries = NULL;
	m->nr = 0;
	m->alloc = 0;
}

static uint32_t fnv1a(const char *s)
{
	uint32_t h = 0x811c9dc5u;
	while (*s) {
		h ^= (uint8_t)*s++;
		h *= 0x01000193u;
	}
	return h;
}

/*
 * Return the slot for @p key given its precomputed @p hash.  If the
 * key is present, the returned slot holds it; otherwise the returned
 * slot is the first empty slot along the probe chain -- the insertion
 * point.  Callers distinguish the two by checking entries[slot].key.
 */
static size_t smap_slot(const struct smap *m, const char *key, uint32_t hash)
{
	size_t mask = m->alloc - 1;
	size_t i = hash & mask;

	while (m->entries[i].key) {
		if (m->entries[i].hash == hash &&
		    strcmp(m->entries[i].key, key) == 0)
			return i;
		i = (i + 1) & mask;
	}
	return i;
}

/*
 * Insert (key, val, hash) into @p m, which must have at least one free
 * slot and must not already contain @p key.  Takes ownership of @p
 * key (no copy).
 */
static void smap_insert_raw(struct smap *m, char *key, void *val, uint32_t hash)
{
	size_t i = smap_slot(m, key, hash);
	m->entries[i].key = key;
	m->entries[i].val = val;
	m->entries[i].hash = hash;
	m->nr++;
}

static void smap_resize(struct smap *m, size_t new_alloc)
{
	struct smap old = *m;

	m->entries = calloc(new_alloc, sizeof(*m->entries));
	if (!m->entries)
		die_errno("calloc");
	m->alloc = new_alloc;
	m->nr = 0;

	for (size_t i = 0; i < old.alloc; i++) {
		if (old.entries[i].key)
			smap_insert_raw(m, old.entries[i].key,
					old.entries[i].val,
					old.entries[i].hash);
	}
	free(old.entries);
}

void smap_release(struct smap *m)
{
	if (m->alloc) {
		for (size_t i = 0; i < m->alloc; i++)
			free(m->entries[i].key);
		free(m->entries);
	}
	smap_init(m);
}

void *smap_put(struct smap *m, const char *key, void *val)
{
	if (!m->alloc)
		smap_resize(m, SMAP_INITIAL_CAP);
	else if ((m->nr + 1) * SMAP_LOAD_DEN > m->alloc * SMAP_LOAD_NUM)
		smap_resize(m, m->alloc * 2);

	uint32_t hash = fnv1a(key);
	size_t i = smap_slot(m, key, hash);

	if (m->entries[i].key) {
		void *prev = m->entries[i].val;
		m->entries[i].val = val;
		return prev;
	}

	m->entries[i].key = sbuf_strdup(key);
	m->entries[i].val = val;
	m->entries[i].hash = hash;
	m->nr++;
	return NULL;
}

void *smap_get(const struct smap *m, const char *key)
{
	if (!m->alloc)
		return NULL;

	size_t i = smap_slot(m, key, fnv1a(key));
	return m->entries[i].key ? m->entries[i].val : NULL;
}

void *smap_remove(struct smap *m, const char *key)
{
	if (!m->alloc)
		return NULL;

	size_t mask = m->alloc - 1;
	size_t i = smap_slot(m, key, fnv1a(key));

	if (!m->entries[i].key)
		return NULL;

	void *val = m->entries[i].val;
	free(m->entries[i].key);
	m->entries[i].key = NULL;
	m->nr--;

	/*
	 * Rehash the probe chain that followed the removed slot so
	 * later lookups can still reach entries displaced past it.
	 */
	size_t j = (i + 1) & mask;
	while (m->entries[j].key) {
		char *k = m->entries[j].key;
		void *v = m->entries[j].val;
		uint32_t h = m->entries[j].hash;
		m->entries[j].key = NULL;
		m->nr--;
		smap_insert_raw(m, k, v, h);
		j = (j + 1) & mask;
	}

	return val;
}

int smap_iter(const struct smap *m, size_t *cursor, const char **key,
	      void **val)
{
	while (*cursor < m->alloc) {
		size_t i = (*cursor)++;
		if (m->entries[i].key) {
			*key = m->entries[i].key;
			*val = m->entries[i].val;
			return 1;
		}
	}
	return 0;
}
