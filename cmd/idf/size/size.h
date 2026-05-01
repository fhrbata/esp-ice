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

/* ------------------------------------------------------------------ */
/* Completion helpers exposed to the porcelain                        */
/*                                                                    */
/* The plumbing owns the canonical option set, so the choice list and */
/* the map-walking logic live here rather than getting reinvented in  */
/* @c cmd/size/.  The porcelain's own subcommands include this header */
/* and reuse the helpers verbatim (--format) or thinly wrap them      */
/* (component name, which is just the archive basename minus          */
/* @c "lib" / @c ".a").                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Emit the supported @c --format choices.
 *
 * Fixed list (table / text / tree / csv / json2 / raw / dot); no
 * project state needed.  Wired into both @c "ice idf size --format"
 * and @c "ice size --format" via the option tables' completion slot.
 */
void idf_size_complete_format(void);

/**
 * @brief Visit each unique archive (basename) in the active profile's
 *        map file.
 *
 * Calls @c setup_project_lenient() so the helper works even when the
 * dispatcher's lenient setup hasn't run (or didn't kick in because
 * the option's leaf was reached via a path that bypasses the
 * --ice-complete hook).  Reads @c _project.mapfile, parses it once,
 * walks every input section's @c archive field, deduplicates by
 * basename, and yields each unique archive name (e.g.
 * @c "libesp_system.a") to @p emit.
 *
 * No-op when no map is available (not in a project, or the build
 * hasn't run).  This lets completion stay silent rather than die.
 *
 * @param emit  callback invoked once per unique archive basename.
 * @param ud    opaque pointer passed through to @p emit.
 */
typedef void (*idf_size_archive_emit)(const char *archive, void *ud);
void idf_size_walk_archives(idf_size_archive_emit emit, void *ud);

/**
 * @brief Emit each archive verbatim (e.g. @c "libesp_system.a") for
 *        completion of the plumbing's @c --archive-details option.
 */
void idf_size_complete_archive(void);

#endif /* CMD_IDF_SIZE_SIZE_H */
