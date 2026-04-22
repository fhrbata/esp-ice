/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sdkconfig.h
 * @brief Lightweight sdkconfig reader + .lf conditional evaluator.
 *
 * This is NOT a full Kconfig implementation.  It parses the sdkconfig
 * file as a flat key=value store and evaluates boolean expressions
 * over those values, which covers virtually every conditional found
 * in ESP-IDF's linker fragment files.
 *
 * Supported sdkconfig line shapes:
 *
 *   CONFIG_FOO=y                 -> FOO = "y"      (bool true)
 *   CONFIG_FOO=n                 -> FOO = "n"      (bool false)
 *   CONFIG_FOO=                  -> FOO = ""       (falsy)
 *   CONFIG_FOO=0x123             -> FOO = "0x123"  (int, truthy if nonzero)
 *   CONFIG_FOO=-9                -> FOO = "-9"     (int)
 *   CONFIG_FOO="a string"        -> FOO = "a string"
 *   # CONFIG_FOO is not set      -> FOO absent     (falsy)
 *
 * Expressions in .lf `if`/`elif` clauses:
 *
 *   SYM                          truthy if SYM is set and nonzero/nonempty
 *   SYM = y                      equality check (y, n, or any string/number)
 *   SYM != y                     inequality
 *   !X                           negation
 *   X && Y                       short-circuit and
 *   X || Y                       short-circuit or
 *   ( expr )                     grouping
 *
 * Unknown symbols evaluate to falsy (same as Kconfig's "n" default).
 */
#ifndef LDGEN_SDKCONFIG_H
#define LDGEN_SDKCONFIG_H

struct sdkconfig_entry {
	char *name;  /**< Without the CONFIG_ prefix. */
	char *value; /**< "y", "n", an int, a string -- or NULL if unset. */
};

struct sdkconfig {
	struct sdkconfig_entry *v;
	int nr;
	int alloc;
};

/** @brief Load a sdkconfig file.  Dies on I/O error. */
void sdkconfig_load(struct sdkconfig *s, const char *path);

/**
 * @brief Evaluate a .lf conditional expression.
 *
 * Returns 1 if @p expr is true, 0 otherwise.  Dies on syntax errors.
 * @p expr may be NULL or empty (treated as false).
 */
int sdkconfig_eval(const struct sdkconfig *s, const char *expr);

/** @brief Release all memory owned by @p s. */
void sdkconfig_free(struct sdkconfig *s);

#endif /* LDGEN_SDKCONFIG_H */
