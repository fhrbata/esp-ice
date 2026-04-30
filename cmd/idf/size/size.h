/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file size.h
 * @brief Shared CLI-flag bundle for `ice idf size`.
 *
 * Mirrors the argparse Namespace used by upstream esp-idf-size so the
 * various transformations (sort, trim, summary builders) and
 * formatters can share a single options bag rather than carry a long
 * argument list each.
 */
#ifndef CMD_IDF_SIZE_SIZE_H
#define CMD_IDF_SIZE_SIZE_H

#include <stddef.h>
#include <stdio.h>

struct mm_args {
	const char
	    *format;  /**< "table"/"text"/"tree"/"csv"/"json2"/"raw"/"dot" */
	int archives; /**< --archives */
	int archive_deps;	     /**< --archive-dependencies */
	int dep_symbols;	     /**< --dep-symbols */
	int dep_reverse;	     /**< --dep-reverse */
	const char *archive_details; /**< --archive-details NAME */
	int files;		     /**< --files */
	const char *diff;	     /**< --diff REF_MAP */
	int no_abbrev;		     /**< --no-abbrev */
	int abbrev;		     /**< derived: !no_abbrev */
	int unify;		     /**< --unify */
	int show_unused;	     /**< --show-unused */
	int show_unchanged;	     /**< --show-unchanged */
	int use_flash_size;	     /**< --use-flash-size */
	const char *target_override; /**< --target CHIP */

	/* Sorting. */
	const char *sort; /**< --sort COL (number or name) */
	int sort_diff;	  /**< --sort-diff */
	int sort_reverse; /**< --sort-reverse: ascending if set, else descending
			   */

	/* Filter. */
	const char **filter; /**< --filter PATTERN (multiple OK) */
	int nr_filter;

	/* Output sinks. */
	const char *output_file; /**< -o FILE */
	int quiet;
	int no_color;

	/* Internal: resolved output stream. */
	FILE *out;
};

#endif /* CMD_IDF_SIZE_SIZE_H */
