/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file views.h
 * @brief Per-archive / per-file / per-symbol summaries and archive deps.
 *
 * The summary structures pivot the memmap tree by an item key (archive,
 * object file, or symbol) and slice it into a memory-type matrix, the
 * same shape esp-idf-size emits for `--archives`, `--files` and
 * `--archive-details`.  Archive dependencies are derived from the
 * Cross Reference Table (`map_xref`) and pruned against the live
 * archive set so discarded symbols don't pollute the output.
 */
#ifndef CMD_IDF_SIZE_VIEWS_H
#define CMD_IDF_SIZE_VIEWS_H

#include <stddef.h>
#include <stdint.h>

#include "map.h"
#include "memmap.h"
#include "size.h"

/* ---- Pivoted summary tree ----------------------------------------- */

struct sv_section {
	char *name;	   /**< Owned: full section name. */
	char *abbrev_name; /**< Owned. */
	int64_t size;
	int64_t size_diff;
};

struct sv_type {
	char *name;
	int64_t size;
	int64_t size_diff;
	struct sv_section **sections;
	int nr_sections, alloc_sections;
};

struct sv_entry {
	char *name;	   /**< archive / object / symbol full key. */
	char *abbrev_name; /**< basename / leaf identifier. */
	int64_t size;
	int64_t size_diff;
	struct sv_type **types;
	int nr_types, alloc_types;
};

struct sv_summary {
	struct sv_entry **entries;
	int nr_entries, alloc_entries;
};

/**
 * Build a per-archive view.  Aggregates input section contributions
 * across all sections within a memory type and returns one entry per
 * archive (full path key by default, basename when @p args->unify is
 * set).  Filtered by @p args->filter.
 */
void sv_archives(const struct memmap *mm, const struct mm_args *args,
		 struct sv_summary *out);

/** Build a per-file view (one entry per object_file). */
void sv_files(const struct memmap *mm, const struct mm_args *args,
	      struct sv_summary *out);

/**
 * Build a per-symbol view restricted to the archive whose abbrev_name
 * matches args->archive_details.  Dies (via log_die) if no archive
 * matches.
 */
void sv_symbols(const struct memmap *mm, const struct mm_args *args,
		struct sv_summary *out);

/**
 * Sort entries by size/size_diff per @p args.  Type/section ordering
 * inside each entry is left untouched -- table-style emitters lock
 * their column layout to it.
 */
void sv_sort(struct sv_summary *s, const struct mm_args *args);

/**
 * Additional pass for JSON-style emitters: sort each entry's
 * memory_types (and the sections within them) recursively, matching
 * upstream's `get_summary_sorted`.  Call after sv_sort().
 */
void sv_sort_columns(struct sv_summary *s, const struct mm_args *args);

/** Drop entries whose name doesn't match any --filter pattern. */
void sv_filter(struct sv_summary *s, const struct mm_args *args);

void sv_release(struct sv_summary *s);

/* ---- Archive dependencies (cross-reference table) ----------------- */

struct dep_archive {
	char *name;
	char *abbrev_name;
	int64_t size;
	char **symbols; /**< Heap-owned symbol-name strings. */
	int nr_symbols, alloc_symbols;
};

struct dep_entry {
	char *name; /**< Owning archive (def or ref depending on direction). */
	char *abbrev_name;
	int64_t size;
	struct dep_archive **archives;
	int nr_archives, alloc_archives;
};

struct dep_summary {
	struct dep_entry **entries;
	int nr_entries, alloc_entries;
};

/**
 * Build archive dependencies from the linker map's CRT.
 *
 * If args->dep_reverse is set, returns def -> [refs that use it]; else
 * returns ref -> [defs it pulls in].  The ELF symbol table is required
 * (used to drop discarded symbols); dies if not provided.
 */
void dep_build(const struct map_file *mf, const struct memmap *mm,
	       const struct elf_symbols *syms, const struct mm_args *args,
	       struct dep_summary *out);

void dep_filter(struct dep_summary *s, const struct mm_args *args);
void dep_release(struct dep_summary *s);

#endif /* CMD_IDF_SIZE_VIEWS_H */
