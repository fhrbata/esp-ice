/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file format.h
 * @brief Output formatters for `ice idf size`.
 *
 * One small Table struct backs both the table and CSV emitters; tree
 * / dot / json walk the memmap directly.  Every formatter writes to
 * the FILE * stored on @p args (set up by the size dispatcher).
 */
#ifndef CMD_IDF_SIZE_FORMAT_H
#define CMD_IDF_SIZE_FORMAT_H

#include <stdint.h>
#include <stdio.h>

#include "map.h"
#include "memmap.h"
#include "size.h"
#include "views.h"

/* ---- Generic table model ----------------------------------------- */

struct tab_cell {
	char *text;	 /**< Owned. */
	int align_right; /**< 0 = left, 1 = right. */
};

struct tab_row {
	struct tab_cell *cells;
	int nr_cells;
};

struct tab {
	char *title;
	char **headers;
	int nr_cols;
	struct tab_row *rows;
	int nr_rows;
	int alloc_rows;
};

void tab_init(struct tab *t, const char *title, const char *const *headers,
	      int nr_cols);
void tab_add_row(struct tab *t, const char *const *cells,
		 const int *align_right);
void tab_release(struct tab *t);

void tab_render_box(const struct tab *t, FILE *out);
void tab_render_csv(const struct tab *t, FILE *out);

/* ---- Format dispatchers ------------------------------------------ */

void fmt_table_summary(const struct memmap *mm, const struct mm_args *args);
void fmt_table_archives(const struct memmap *mm, const struct mm_args *args);
void fmt_table_files(const struct memmap *mm, const struct mm_args *args);
void fmt_table_symbols(const struct memmap *mm, const struct mm_args *args);
void fmt_table_deps(const struct map_file *mf, const struct memmap *mm,
		    const struct elf_symbols *syms, const struct mm_args *args);

void fmt_csv_summary(const struct memmap *mm, const struct mm_args *args);
void fmt_csv_archives(const struct memmap *mm, const struct mm_args *args);
void fmt_csv_files(const struct memmap *mm, const struct mm_args *args);
void fmt_csv_symbols(const struct memmap *mm, const struct mm_args *args);
void fmt_csv_deps(const struct map_file *mf, const struct memmap *mm,
		  const struct elf_symbols *syms, const struct mm_args *args);

void fmt_json_raw(const struct memmap *mm, const struct mm_args *args);
void fmt_json2_summary(const struct memmap *mm, const struct mm_args *args);
void fmt_json2_archives(const struct memmap *mm, const struct mm_args *args);
void fmt_json2_files(const struct memmap *mm, const struct mm_args *args);
void fmt_json2_symbols(const struct memmap *mm, const struct mm_args *args);
void fmt_json_deps(const struct map_file *mf, const struct memmap *mm,
		   const struct elf_symbols *syms, const struct mm_args *args);

void fmt_tree_memmap(const struct memmap *mm, const struct mm_args *args);
void fmt_tree_deps(const struct map_file *mf, const struct memmap *mm,
		   const struct elf_symbols *syms, const struct mm_args *args);

void fmt_dot_deps(const struct map_file *mf, const struct memmap *mm,
		  const struct elf_symbols *syms, const struct mm_args *args);

/* ---- Helper used by table / CSV summary tables ------------------- */

/**
 * Build the rectangular table for the summary view (memory_type rows
 * with section sub-rows).  Returns a populated tab; caller calls
 * tab_release() when done.  Non-static so format_csv can reuse it.
 */
void build_summary_tab(const struct memmap *mm, const struct mm_args *args,
		       struct tab *out);

/**
 * Build a per-archive / per-file / per-symbol pivoted table from a
 * pre-built sv_summary and a title.  @p item_label is the name of the
 * first column (e.g. "Archive File", "Object File", "Symbol").
 */
void build_pivoted_tab(const struct sv_summary *s, const char *title,
		       const char *item_label, const struct mm_args *args,
		       struct tab *out);

/**
 * Build the archive-deps table from a pre-built dep_summary.
 */
void build_deps_tab(const struct dep_summary *d, const struct mm_args *args,
		    struct tab *out);

/**
 * Print the trailing notes that follow the summary table (image size
 * line + the "reported total sizes may be smaller..." reminder).
 */
void print_summary_notes(const struct memmap *mm, const struct mm_args *args);

#endif /* CMD_IDF_SIZE_FORMAT_H */
