/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file format_tree.c
 * @brief Indented-tree renderer for the memmap and archive deps.
 *
 * Mirrors the output of upstream's Rich Tree (without the colors):
 *
 *   Memory Types
 *   ├── Flash Code 64442
 *   │   └── .text 64442
 *   │       ├── libc.a 38513
 *   │       │   ├── libc_a-vfprintf.o 12845
 *   ...
 *
 * Branch glyphs are derived from a depth-tracked "is the parent the
 * last child at this level?" stack so the prefix is correct at every
 * level.
 */
#include "ice.h"

#include "format.h"
#include "memmap.h"
#include "size.h"
#include "views.h"

#define BR_LAST "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 " /* "└── " */
#define BR_TEE "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "	/* "├── " */
#define LINE_VL "\xe2\x94\x82   "			/* "│   " */
#define BLANKS "    "

static void emit_prefix(FILE *out, const int *last, int depth)
{
	for (int i = 0; i < depth - 1; i++)
		fputs(last[i] ? BLANKS : LINE_VL, out);
	if (depth > 0)
		fputs(last[depth - 1] ? BR_LAST : BR_TEE, out);
}

static int diff_inline_str(char *buf, size_t bufsz, int64_t value, int64_t diff,
			   int show)
{
	if (!show)
		return snprintf(buf, bufsz, "%lld", (long long)value);
	if (diff != 0)
		return snprintf(buf, bufsz, "%lld %+6lld", (long long)value,
				(long long)diff);
	return snprintf(buf, bufsz, "%lld %6lld", (long long)value,
			(long long)diff);
}

static void emit_symbol(FILE *out, const struct mm_symbol *sy, int *last,
			int depth, int is_last, int show_diff)
{
	char szbuf[64];

	last[depth - 1] = is_last;
	emit_prefix(out, last, depth);
	diff_inline_str(szbuf, sizeof(szbuf), sy->size, sy->size_diff,
			show_diff);
	fprintf(out, "%s %s\n", sy->name, szbuf);
}

static void emit_object(FILE *out, const struct mm_object *o, int *last,
			int depth, int is_last, const struct mm_args *args)
{
	char szbuf[64];

	last[depth - 1] = is_last;
	emit_prefix(out, last, depth);
	diff_inline_str(szbuf, sizeof(szbuf), o->size, o->size_diff,
			args->diff != NULL);
	fprintf(out, "%s %s\n", args->abbrev ? o->abbrev_name : o->name, szbuf);

	for (int i = 0; i < o->nr_syms; i++)
		emit_symbol(out, o->syms[i], last, depth + 1,
			    i + 1 == o->nr_syms, args->diff != NULL);
}

static void emit_archive(FILE *out, const struct mm_archive *a, int *last,
			 int depth, int is_last, const struct mm_args *args)
{
	char szbuf[64];

	last[depth - 1] = is_last;
	emit_prefix(out, last, depth);
	diff_inline_str(szbuf, sizeof(szbuf), a->size, a->size_diff,
			args->diff != NULL);
	fprintf(out, "%s %s\n", args->abbrev ? a->abbrev_name : a->name, szbuf);

	for (int i = 0; i < a->nr_objs; i++)
		emit_object(out, a->objs[i], last, depth + 1,
			    i + 1 == a->nr_objs, args);
}

static void emit_section(FILE *out, const struct mm_section *s, int *last,
			 int depth, int is_last, const struct mm_args *args)
{
	char szbuf[64];

	last[depth - 1] = is_last;
	emit_prefix(out, last, depth);
	diff_inline_str(szbuf, sizeof(szbuf), s->size, s->size_diff,
			args->diff != NULL);
	fprintf(out, "%s %s\n", args->abbrev ? s->abbrev_name : s->name, szbuf);

	for (int i = 0; i < s->nr_archives; i++)
		emit_archive(out, s->archives[i], last, depth + 1,
			     i + 1 == s->nr_archives, args);
}

static void emit_type(FILE *out, const struct mm_type *t, int *last, int depth,
		      int is_last, const struct mm_args *args)
{
	char usedbuf[64], totalbuf[64];

	last[depth - 1] = is_last;
	emit_prefix(out, last, depth);
	diff_inline_str(usedbuf, sizeof(usedbuf), t->used, t->used_diff,
			args->diff != NULL);
	if (t->size) {
		diff_inline_str(totalbuf, sizeof(totalbuf), t->size,
				t->size_diff, args->diff != NULL);
		fprintf(out, "%s %s / %s\n", t->name, usedbuf, totalbuf);
	} else {
		fprintf(out, "%s %s\n", t->name, usedbuf);
	}

	for (int i = 0; i < t->nr_sections; i++)
		emit_section(out, t->sections[i], last, depth + 1,
			     i + 1 == t->nr_sections, args);
}

void fmt_tree_memmap(const struct memmap *mm, const struct mm_args *args)
{
	int last[16];
	memset(last, 0, sizeof(last));

	fputs("Memory Types\n", args->out);
	for (int i = 0; i < mm->nr_types; i++)
		emit_type(args->out, mm->types[i], last, 1,
			  i + 1 == mm->nr_types, args);
}

/* ------------------------------------------------------------------ */
/* deps tree                                                          */
/* ------------------------------------------------------------------ */

void fmt_tree_deps(const struct map_file *mf, const struct memmap *mm,
		   const struct elf_symbols *syms, const struct mm_args *args)
{
	struct dep_summary d;
	const char *title = args->dep_reverse ? "Archive reverse dependencies"
					      : "Archive dependencies";
	int last[16];

	dep_build(mf, mm, syms, args, &d);
	dep_filter(&d, args);

	memset(last, 0, sizeof(last));
	fprintf(args->out, "%s\n", title);

	for (int i = 0; i < d.nr_entries; i++) {
		struct dep_entry *e = d.entries[i];
		int is_last_e = (i + 1 == d.nr_entries);

		last[0] = is_last_e;
		emit_prefix(args->out, last, 1);
		fprintf(args->out, "%s %lld\n",
			args->abbrev ? e->abbrev_name : e->name,
			(long long)e->size);

		for (int j = 0; j < e->nr_archives; j++) {
			struct dep_archive *da = e->archives[j];
			int is_last_a = (j + 1 == e->nr_archives);

			last[1] = is_last_a;
			emit_prefix(args->out, last, 2);
			fprintf(args->out, "%s %lld\n",
				args->abbrev ? da->abbrev_name : da->name,
				(long long)da->size);

			if (args->dep_symbols) {
				for (int k = 0; k < da->nr_symbols; k++) {
					int is_last_s =
					    (k + 1 == da->nr_symbols);
					last[2] = is_last_s;
					emit_prefix(args->out, last, 3);
					fputs(da->symbols[k], args->out);
					fputc('\n', args->out);
				}
			}
		}
	}

	dep_release(&d);
}
