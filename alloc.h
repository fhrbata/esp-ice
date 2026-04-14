/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file alloc.h
 * @brief Typed array growth macros, modelled after Git's ALLOC_GROW.
 *
 * Usage -- declare a pointer, a count, and a capacity:
 *
 *   struct item *v = NULL;
 *   int nr = 0, alloc = 0;
 *
 *   ALLOC_GROW(v, nr + 1, alloc);
 *   v[nr++] = value;
 *
 * ALLOC_GROW ensures that v can hold at least nr elements, calling
 * realloc and adjusting alloc as needed.  Growth policy is 1.5x with
 * a minimum of 16 slots.  Dies on OOM.
 *
 * Do not use expressions with side-effects for any argument.
 */
#ifndef ALLOC_H
#define ALLOC_H

#include "error.h"

#define alloc_nr(x) (((x) + 16) * 3 / 2)

/**
 * @brief Reallocate a typed array to exactly @p alloc elements.
 *
 * Dies on OOM.  @p x is updated in place.
 */
#define REALLOC_ARRAY(x, alloc)                                                \
	do {                                                                   \
		void *_p = realloc((x), (alloc) * sizeof(*(x)));               \
		if (!_p)                                                       \
			die_errno("realloc");                                  \
		(x) = _p;                                                      \
	} while (0)

/**
 * @brief Ensure a typed array can hold at least @p nr elements.
 *
 * If the current capacity @p alloc is insufficient, the array @p x is
 * grown using a 1.5x policy (via alloc_nr()) and @p alloc is updated.
 * Dies on OOM.  The caller is responsible for updating the element
 * count after filling in new entries.
 */
#define ALLOC_GROW(x, nr, alloc)                                               \
	do {                                                                   \
		if ((nr) > (alloc)) {                                          \
			if (alloc_nr(alloc) < (nr))                            \
				(alloc) = (nr);                                \
			else                                                   \
				(alloc) = alloc_nr(alloc);                     \
			REALLOC_ARRAY(x, alloc);                               \
		}                                                              \
	} while (0)

#endif /* ALLOC_H */
