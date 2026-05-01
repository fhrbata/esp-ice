/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file format_csv.c
 * @brief CSV output -- delegates to the table builder, then transposes.
 */
#include "ice.h"

#include "format.h"
#include "size.h"
#include "views.h"

void fmt_csv_summary(const struct memmap *mm, const struct mm_args *args)
{
	struct tab t;
	build_summary_tab(mm, args, &t);
	tab_render_csv(&t, args->out);
	tab_release(&t);
}

void fmt_csv_archives(const struct memmap *mm, const struct mm_args *args)
{
	struct sv_summary s;
	struct tab t;
	sv_archives(mm, args, &s);
	sv_filter(&s, args);
	sv_sort(&s, args);
	build_pivoted_tab(&s, "Per-archive contributions to ELF file",
			  "Archive File", args, &t);
	tab_render_csv(&t, args->out);
	tab_release(&t);
	sv_release(&s);
}

void fmt_csv_files(const struct memmap *mm, const struct mm_args *args)
{
	struct sv_summary s;
	struct tab t;
	sv_files(mm, args, &s);
	sv_filter(&s, args);
	sv_sort(&s, args);
	build_pivoted_tab(&s, "Per-file contributions to ELF file",
			  "Object File", args, &t);
	tab_render_csv(&t, args->out);
	tab_release(&t);
	sv_release(&s);
}

void fmt_csv_symbols(const struct memmap *mm, const struct mm_args *args)
{
	struct sv_summary s;
	struct tab t;
	char title[256];

	sv_symbols(mm, args, &s);
	sv_filter(&s, args);
	sv_sort(&s, args);
	snprintf(title, sizeof(title),
		 "Symbols within archive: %s (Not all symbols may be reported)",
		 args->archive_details);
	build_pivoted_tab(&s, title, "Symbol", args, &t);
	tab_render_csv(&t, args->out);
	tab_release(&t);
	sv_release(&s);
}

void fmt_csv_deps(const struct map_file *mf, const struct memmap *mm,
		  const struct elf_symbols *syms, const struct mm_args *args)
{
	struct dep_summary d;
	struct tab t;
	dep_build(mf, mm, syms, args, &d);
	dep_filter(&d, args);
	build_deps_tab(&d, args, &t);
	tab_render_csv(&t, args->out);
	tab_release(&t);
	dep_release(&d);
}
