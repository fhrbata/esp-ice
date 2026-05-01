/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file format_tree.c
 * @brief Indented-tree renderer for the memmap and archive deps.
 *
 * Mirrors the output of upstream's Rich Tree:
 *
 *   Memory Types
 *   ├── Flash Code 64442                         <- yellow
 *   │   └── .text 64442                          <- cyan
 *   │       ├── libc.a 38513                     <- green
 *   │       │   ├── libc_a-vfprintf.o 12845      <- default
 *   ...
 *
 * Branch glyphs are derived from a depth-tracked "is the parent the
 * last child at this level?" stack so the prefix is correct at every
 * level.  A parallel @p style stack carries the per-column color
 * letter for the connector glyph at that column (@x{...} token), so
 * the guide lines pick up the parent's color the way Rich's
 * guide_style does.
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

/* Per-depth connector colors.  style[k] is the color of the connector
 * glyph emitted at column k -- matches the parent's guide_style in
 * upstream's Rich tree.  Indexed by 0-based column, so:
 *   col 0 (mem-type rows)         -> default
 *   col 1 (section rows)          -> 'y' (parent is mem-type)
 *   col 2 (archive rows)          -> 'c' (parent is section)
 *   col 3 (object rows)           -> 'g' (parent is archive)
 *   col 4 (symbol rows)           -> default (parent is object)
 */
static const char tree_style[8] = {0, 'y', 'c', 'g', 0, 0, 0, 0};

static void emit_branch(FILE *out, char letter, const char *glyph)
{
	if (letter)
		fprintf(out, "@%c{%s}", letter, glyph);
	else
		fputs(glyph, out);
}

static void emit_prefix(FILE *out, const int *last, int depth)
{
	for (int i = 0; i < depth - 1; i++)
		emit_branch(out, tree_style[i], last[i] ? BLANKS : LINE_VL);
	if (depth > 0)
		emit_branch(out, tree_style[depth - 1],
			    last[depth - 1] ? BR_LAST : BR_TEE);
}

/* Format "value[ diff]" into a buffer.  The diff suffix is wrapped in
 * @r{...}/@g{...} so it picks up its own color inside the row's color.
 * Caller must emit the result inside a single fprintf call so
 * expand_colors sees the open and close markers in the same pass --
 * depth tracking does not persist across separate stdio calls. */
static void diff_to_str(char *buf, size_t bufsz, int64_t value, int64_t diff,
			int show_diff, int growth_is_bad)
{
	if (!show_diff) {
		snprintf(buf, bufsz, "%lld", (long long)value);
		return;
	}
	if (diff == 0) {
		snprintf(buf, bufsz, "%lld %6lld", (long long)value,
			 (long long)diff);
		return;
	}
	{
		char letter;
		if (growth_is_bad)
			letter = diff > 0 ? 'r' : 'g';
		else
			letter = diff > 0 ? 'g' : 'r';
		snprintf(buf, bufsz, "%lld @%c{%+6lld}", (long long)value,
			 letter, (long long)diff);
	}
}

/* Emit one tree row as a single fprintf so the @<row>{...} block opens
 * and closes in the same expand_colors pass. */
static void emit_row(FILE *out, char row_color, const char *name, int64_t value,
		     int64_t diff, int show_diff)
{
	char body[96];

	diff_to_str(body, sizeof(body), value, diff, show_diff, 1);
	if (row_color)
		fprintf(out, "@%c{%s %s}\n", row_color, name, body);
	else
		fprintf(out, "%s %s\n", name, body);
}

static void emit_symbol(FILE *out, const struct mm_symbol *sy, int *last,
			int depth, int is_last, int show_diff)
{
	last[depth - 1] = is_last;
	emit_prefix(out, last, depth);
	emit_row(out, 0, sy->name, sy->size, sy->size_diff, show_diff);
}

static void emit_object(FILE *out, const struct mm_object *o, int *last,
			int depth, int is_last, const struct mm_args *args)
{
	last[depth - 1] = is_last;
	emit_prefix(out, last, depth);
	emit_row(out, 0, args->abbrev ? o->abbrev_name : o->name, o->size,
		 o->size_diff, args->diff != NULL);

	for (int i = 0; i < o->nr_syms; i++)
		emit_symbol(out, o->syms[i], last, depth + 1,
			    i + 1 == o->nr_syms, args->diff != NULL);
}

static void emit_archive(FILE *out, const struct mm_archive *a, int *last,
			 int depth, int is_last, const struct mm_args *args)
{
	last[depth - 1] = is_last;
	emit_prefix(out, last, depth);
	emit_row(out, 'g', args->abbrev ? a->abbrev_name : a->name, a->size,
		 a->size_diff, args->diff != NULL);

	for (int i = 0; i < a->nr_objs; i++)
		emit_object(out, a->objs[i], last, depth + 1,
			    i + 1 == a->nr_objs, args);
}

static void emit_section(FILE *out, const struct mm_section *s, int *last,
			 int depth, int is_last, const struct mm_args *args)
{
	last[depth - 1] = is_last;
	emit_prefix(out, last, depth);
	emit_row(out, 'c', args->abbrev ? s->abbrev_name : s->name, s->size,
		 s->size_diff, args->diff != NULL);

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
	diff_to_str(usedbuf, sizeof(usedbuf), t->used, t->used_diff,
		    args->diff != NULL, 1);
	if (t->size) {
		/* "total" cell: bigger means more headroom, so growth is
		 * good (green). */
		diff_to_str(totalbuf, sizeof(totalbuf), t->size, t->size_diff,
			    args->diff != NULL, 0);
		fprintf(out, "@y{%s %s / %s}\n", t->name, usedbuf, totalbuf);
	} else {
		fprintf(out, "@y{%s %s}\n", t->name, usedbuf);
	}

	for (int i = 0; i < t->nr_sections; i++)
		emit_section(out, t->sections[i], last, depth + 1,
			     i + 1 == t->nr_sections, args);
}

void fmt_tree_memmap(const struct memmap *mm, const struct mm_args *args)
{
	int last[16];
	memset(last, 0, sizeof(last));

	fputs("@b{Memory Types}\n", args->out);
	for (int i = 0; i < mm->nr_types; i++)
		emit_type(args->out, mm->types[i], last, 1,
			  i + 1 == mm->nr_types, args);
}

/* ------------------------------------------------------------------ */
/* deps tree                                                          */
/* ------------------------------------------------------------------ */

/* Style array for the deps tree: archives at col 0 (yellow header),
 * dep names at col 1 (cyan from archive's guide), symbols at col 2
 * (default from dep's guide). */
static const char dep_tree_style[4] = {0, 'y', 'c', 0};

static void emit_dep_branch(FILE *out, const int *last, int depth)
{
	for (int i = 0; i < depth - 1; i++)
		emit_branch(out, dep_tree_style[i], last[i] ? BLANKS : LINE_VL);
	if (depth > 0)
		emit_branch(out, dep_tree_style[depth - 1],
			    last[depth - 1] ? BR_LAST : BR_TEE);
}

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
	fprintf(args->out, "@b{%s}\n", title);

	for (int i = 0; i < d.nr_entries; i++) {
		struct dep_entry *e = d.entries[i];
		int is_last_e = (i + 1 == d.nr_entries);

		last[0] = is_last_e;
		emit_dep_branch(args->out, last, 1);
		fprintf(args->out, "@y{%s %lld}\n",
			args->abbrev ? e->abbrev_name : e->name,
			(long long)e->size);

		for (int j = 0; j < e->nr_archives; j++) {
			struct dep_archive *da = e->archives[j];
			int is_last_a = (j + 1 == e->nr_archives);

			last[1] = is_last_a;
			emit_dep_branch(args->out, last, 2);
			fprintf(args->out, "@c{%s %lld}\n",
				args->abbrev ? da->abbrev_name : da->name,
				(long long)da->size);

			if (args->dep_symbols) {
				for (int k = 0; k < da->nr_symbols; k++) {
					int is_last_s =
					    (k + 1 == da->nr_symbols);
					last[2] = is_last_s;
					emit_dep_branch(args->out, last, 3);
					fputs(da->symbols[k], args->out);
					fputc('\n', args->out);
				}
			}
		}
	}

	dep_release(&d);
}
