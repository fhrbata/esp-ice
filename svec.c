/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file svec.c
 * @brief Dynamic NULL-terminated string vector implementation.
 */
#include "ice.h"

const char *svec_empty[] = {NULL};

void svec_init(struct svec *sv)
{
	sv->v = svec_empty;
	sv->nr = 0;
	sv->alloc = 0;
}

static void svec_grow(struct svec *sv, size_t extra)
{
	size_t need = sv->nr + extra + 1; /* +1 for NULL sentinel */

	if (need <= sv->alloc)
		return;

	size_t newalloc = sv->alloc ? sv->alloc * 2 : 8;
	while (newalloc < need)
		newalloc *= 2;

	const char **newv = malloc(newalloc * sizeof(char *));
	if (!newv)
		die_errno("malloc");

	if (sv->nr)
		memcpy(newv, sv->v, sv->nr * sizeof(char *));
	newv[sv->nr] = NULL;

	if (sv->alloc)
		free(sv->v);

	sv->v = newv;
	sv->alloc = newalloc;
}

void svec_clear(struct svec *sv)
{
	if (sv->alloc) {
		for (size_t i = 0; i < sv->nr; i++)
			free((char *)sv->v[i]);
		free(sv->v);
	}
	svec_init(sv);
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void svec_sort(struct svec *sv)
{
	if (sv->nr > 1)
		qsort(sv->v, sv->nr, sizeof(sv->v[0]), cmp_str);
}

void svec_push(struct svec *sv, const char *s)
{
	size_t len = strlen(s) + 1;
	char *copy = malloc(len);
	if (!copy)
		die_errno("malloc");
	memcpy(copy, s, len);

	svec_grow(sv, 1);
	sv->v[sv->nr++] = copy;
	sv->v[sv->nr] = NULL;
}

void svec_pushf(struct svec *sv, const char *fmt, ...)
{
	va_list ap, ap2;
	char *s;
	int n;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);

	if (n < 0)
		die("vsnprintf failed");

	s = malloc((size_t)n + 1);
	if (!s)
		die_errno("malloc");

	vsnprintf(s, (size_t)n + 1, fmt, ap);
	va_end(ap);

	svec_grow(sv, 1);
	sv->v[sv->nr++] = s;
	sv->v[sv->nr] = NULL;
}

void svec_pop(struct svec *sv)
{
	if (!sv->nr)
		return;

	free((char *)sv->v[--sv->nr]);
	sv->v[sv->nr] = NULL;
}

const char **svec_detach(struct svec *sv)
{
	const char **v;

	if (!sv->alloc) {
		v = malloc(sizeof(char *));
		if (!v)
			die_errno("malloc");
		v[0] = NULL;
	} else {
		v = sv->v;
	}

	svec_init(sv);
	return v;
}
