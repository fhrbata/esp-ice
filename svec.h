/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file svec.h
 * @brief Dynamic NULL-terminated string vector for building argv.
 *
 * svec manages a heap-allocated array of strings, always
 * NULL-terminated so it can be passed directly to exec. The vector
 * pointer is never NULL -- when empty it points to a static {NULL}
 * sentinel. Strings are copied on push. All allocating operations
 * die() on failure.
 *
 * Usage:
 *   struct svec args = SVEC_INIT;
 *   svec_push(&args, "cmake");
 *   svec_push(&args, "-G");
 *   svec_push(&args, "Ninja");
 *   svec_pushf(&args, "-DCMAKE_BUILD_TYPE=%s", type);
 *   proc.argv = args.v;
 *   process_run(&proc);
 *   svec_clear(&args);
 */
#ifndef SVEC_H
#define SVEC_H

#include <stddef.h>

/** Sentinel empty vector -- sv->v points here when no allocation. */
extern const char *svec_empty[];

struct svec {
	const char **v; /**< NULL-terminated string array (never NULL). */
	size_t nr;	/**< Number of strings (excluding NULL terminator). */
	size_t alloc;	/**< Allocated slots (0 = not owned). */
};

/** Static initializer. */
#define SVEC_INIT {.v = svec_empty, .nr = 0, .alloc = 0}

/** Initialize an svec (equivalent to SVEC_INIT assignment). */
void svec_init(struct svec *sv);

/** Free all strings and the vector; reset to empty state. */
void svec_clear(struct svec *sv);

/** Append a copy of string @p s. */
void svec_push(struct svec *sv, const char *s);

/** Append a printf-formatted string. */
void svec_pushf(struct svec *sv, const char *fmt, ...);

/** Sort strings alphabetically. */
void svec_sort(struct svec *sv);

/** Remove and free the last string. */
void svec_pop(struct svec *sv);

/**
 * @brief Detach and return the string array; caller must free each
 * string and the array. Resets sv to empty.
 */
const char **svec_detach(struct svec *sv);

#endif /* SVEC_H */
