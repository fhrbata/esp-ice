/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file format_table.c
 * @brief Generic table model + Unicode box-drawing renderer.
 *
 * The renderer matches the layout produced by upstream Rich tables in
 * --no-color mode:
 *
 *   <title>
 *   ┏━━┳━━┓
 *   ┃   header  ┃
 *   ┡━━╇━━┩
 *   │ row     │
 *   └──┴──┘
 *
 * Column widths come from max(header, all-rows) for every column,
 * +2 padding (one space each side).  No overflow / wrapping; first
 * column is left-justified, every other column is right-justified
 * unless the caller overrides via cell.align_right.
 */
#include "ice.h"

#include "format.h"
#include "memmap.h"
#include "size.h"
#include "views.h"

/* Box-drawing glyphs come from term.h (TL/TM/TR/ML/MM/MR/BL/BM/BR
 * for the corner/junction set, HH/HL for heavy/light horizontals,
 * VH/VL for heavy/light verticals).  Aligning by character is safe
 * because compute_widths() counts visible code points, not bytes. */

/* ------------------------------------------------------------------ */
/* Tab API                                                            */
/* ------------------------------------------------------------------ */

void tab_init(struct tab *t, const char *title, const char *const *headers,
	      int nr_cols)
{
	memset(t, 0, sizeof(*t));
	t->title = title ? sbuf_strdup(title) : NULL;
	t->nr_cols = nr_cols;
	t->headers = calloc((size_t)nr_cols, sizeof(*t->headers));
	if (!t->headers)
		die_errno("calloc");
	for (int i = 0; i < nr_cols; i++)
		t->headers[i] = sbuf_strdup(headers[i]);
}

void tab_add_row(struct tab *t, const char *const *cells,
		 const int *align_right)
{
	struct tab_row *r;

	ALLOC_GROW(t->rows, t->nr_rows + 1, t->alloc_rows);
	r = &t->rows[t->nr_rows];
	memset(r, 0, sizeof(*r));
	r->nr_cells = t->nr_cols;
	r->cells = calloc((size_t)t->nr_cols, sizeof(*r->cells));
	if (!r->cells)
		die_errno("calloc");

	for (int i = 0; i < t->nr_cols; i++) {
		r->cells[i].text = sbuf_strdup(cells[i] ? cells[i] : "");
		r->cells[i].align_right =
		    align_right ? align_right[i] : (i == 0 ? 0 : 1);
	}
	t->nr_rows++;
}

void tab_release(struct tab *t)
{
	free(t->title);
	for (int i = 0; i < t->nr_cols; i++)
		free(t->headers[i]);
	free(t->headers);
	for (int i = 0; i < t->nr_rows; i++) {
		for (int j = 0; j < t->rows[i].nr_cells; j++)
			free(t->rows[i].cells[j].text);
		free(t->rows[i].cells);
	}
	free(t->rows);
	memset(t, 0, sizeof(*t));
}

/* ------------------------------------------------------------------ */
/* Width helpers (counts UTF-8 code points)                           */
/* ------------------------------------------------------------------ */

static int utf8_width(const char *s)
{
	int n = 0;
	while (*s) {
		unsigned char c = (unsigned char)*s++;
		if ((c & 0xC0) == 0x80)
			continue;
		n++;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Render: box-drawing                                                 */
/* ------------------------------------------------------------------ */

static void compute_widths(const struct tab *t, int *w)
{
	for (int i = 0; i < t->nr_cols; i++)
		w[i] = utf8_width(t->headers[i]);
	for (int i = 0; i < t->nr_rows; i++) {
		for (int j = 0; j < t->nr_cols; j++) {
			int cw = utf8_width(t->rows[i].cells[j].text);
			if (cw > w[j])
				w[j] = cw;
		}
	}
}

static void put_repeat(FILE *out, const char *seq, int times)
{
	for (int i = 0; i < times; i++)
		fputs(seq, out);
}

static void hline(FILE *out, const int *w, int nr_cols, const char *l,
		  const char *m, const char *r, const char *fill)
{
	fputs(l, out);
	for (int i = 0; i < nr_cols; i++) {
		put_repeat(out, fill, w[i] + 2);
		fputs(i < nr_cols - 1 ? m : r, out);
	}
	fputc('\n', out);
}

static void put_cell(FILE *out, const char *text, int width, int align_right)
{
	int tw = utf8_width(text);
	int pad = width - tw;
	if (align_right) {
		for (int i = 0; i < pad; i++)
			fputc(' ', out);
		fputs(text, out);
	} else {
		fputs(text, out);
		for (int i = 0; i < pad; i++)
			fputc(' ', out);
	}
}

void tab_render_box(const struct tab *t, FILE *out)
{
	int *w;
	int total_inner;

	if (t->nr_cols == 0)
		return;

	w = calloc((size_t)t->nr_cols, sizeof(*w));
	if (!w)
		die_errno("calloc");
	compute_widths(t, w);

	/* Visible table width = 1 (left ┏) + sum(w[i]+2) + (n-1) inner
	 * separators + 1 (right ┓) = sum(w[i]) + 3n + 1.  Match Rich's
	 * centering by padding both sides so the title line has the
	 * same visual length as the table border. */
	total_inner = 1;
	for (int i = 0; i < t->nr_cols; i++)
		total_inner += w[i] + 3;

	if (t->title && *t->title) {
		int tw = utf8_width(t->title);
		int pad = (total_inner - tw) / 2;
		int trail;
		if (pad < 0)
			pad = 0;
		trail = total_inner - pad - tw;
		if (trail < 0)
			trail = 0;
		for (int i = 0; i < pad; i++)
			fputc(' ', out);
		fputs(t->title, out);
		for (int i = 0; i < trail; i++)
			fputc(' ', out);
		fputc('\n', out);
	}

	hline(out, w, t->nr_cols, TL, TM, TR, HH);

	fputs(VH, out);
	for (int i = 0; i < t->nr_cols; i++) {
		fputc(' ', out);
		put_cell(out, t->headers[i], w[i], i == 0 ? 0 : 1);
		fputc(' ', out);
		fputs(VH, out);
	}
	fputc('\n', out);

	hline(out, w, t->nr_cols, ML, MM, MR, HH);

	for (int i = 0; i < t->nr_rows; i++) {
		fputs(VL, out);
		for (int j = 0; j < t->nr_cols; j++) {
			fputc(' ', out);
			put_cell(out, t->rows[i].cells[j].text, w[j],
				 t->rows[i].cells[j].align_right);
			fputc(' ', out);
			fputs(VL, out);
		}
		fputc('\n', out);
	}

	hline(out, w, t->nr_cols, BL, BM, BR, HL);

	free(w);
}

/* ------------------------------------------------------------------ */
/* Render: CSV                                                         */
/* ------------------------------------------------------------------ */

/*
 * Mirror upstream's CSV emitter: strip leading/trailing whitespace
 * from each cell before quoting (the indented row labels don't make
 * sense in spreadsheet output).
 */
static void csv_emit_cell(FILE *out, const char *s)
{
	const char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
		end--;

	fputc('"', out);
	for (const char *p = s; p < end; p++) {
		if (*p == '"')
			fputc('"', out);
		fputc(*p, out);
	}
	fputc('"', out);
}

void tab_render_csv(const struct tab *t, FILE *out)
{
	for (int i = 0; i < t->nr_cols; i++) {
		if (i)
			fputc(',', out);
		csv_emit_cell(out, t->headers[i]);
	}
	fputc('\n', out);

	for (int i = 0; i < t->nr_rows; i++) {
		for (int j = 0; j < t->nr_cols; j++) {
			if (j)
				fputc(',', out);
			csv_emit_cell(out, t->rows[i].cells[j].text);
		}
		fputc('\n', out);
	}
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Format an int64 as "N" (or "+N"/"-N" if showing diff inline). */
static void int_to_str(char *buf, size_t bufsz, int64_t n)
{
	snprintf(buf, bufsz, "%lld", (long long)n);
}

/* Format diff-inline cell: "value diff" with sign prefix. */
static void diff_inline(char *buf, size_t bufsz, int64_t value, int64_t diff,
			int show_diff)
{
	if (!show_diff) {
		int_to_str(buf, bufsz, value);
		return;
	}
	if (diff != 0)
		snprintf(buf, bufsz, "%lld %+6lld", (long long)value,
			 (long long)diff);
	else
		snprintf(buf, bufsz, "%lld %6lld", (long long)value,
			 (long long)diff);
}

/* Format percentage stripping trailing zeros (matches upstream
 * round(x, 2) for floats: "100.0" if integer, "15.63" otherwise).
 * Returns the written length (excluding NUL). */
static int fmt_pct(char *buf, size_t bufsz, double pct)
{
	int len = snprintf(buf, bufsz, "%.2f", pct);
	while (len > 1 && buf[len - 1] == '0')
		buf[--len] = '\0';
	if (len > 0 && buf[len - 1] == '.') {
		if ((size_t)len + 2 < bufsz) {
			buf[len++] = '0';
			buf[len] = '\0';
		}
	}
	/* Trim a stray ".0" so "100.0" stays "100.0" but "100" stays
	 * a non-decimal -- actually upstream's round(100,2) yields 100
	 * (int), printed as "100".  We keep "100.0" for consistency
	 * with the non-zero examples, since the saved fixtures show
	 * decimals always.  */
	return len;
}

/*
 * Format the diff suffix matching Python's `f'{pct_diff:+6}'` semantics
 * over a rounded-to-2-places float: build the value with %.2f, strip
 * trailing zeros (keep at least one decimal), then right-align to 6.
 * Outputs to @p out, including the leading separator space.
 */
static void emit_pct_diff_suffix(char *out, size_t outsz, double pct_diff)
{
	char tmp[32];
	int len;
	int width = 6;
	int pad;

	if (pct_diff != 0)
		len = snprintf(tmp, sizeof(tmp), "%+.2f", pct_diff);
	else
		len = snprintf(tmp, sizeof(tmp), "%.1f", 0.0);

	if (pct_diff != 0.0) {
		while (len > 1 && tmp[len - 1] == '0')
			tmp[--len] = '\0';
		if (len > 0 && tmp[len - 1] == '.') {
			tmp[len++] = '0';
			tmp[len] = '\0';
		}
	}

	pad = width - len;
	if (pad < 0)
		pad = 0;
	snprintf(out, outsz, " %*s%s", pad, "", tmp);
}

static void fmt_pct_diff(char *buf, size_t bufsz, double pct, double pct_diff,
			 int show_diff)
{
	int len;

	len = fmt_pct(buf, bufsz, pct);
	if (!show_diff)
		return;
	emit_pct_diff_suffix(buf + len, bufsz - (size_t)len, pct_diff);
}

/* ------------------------------------------------------------------ */
/* Summary table                                                       */
/* ------------------------------------------------------------------ */

/* Compare helpers used internally for the summary table sort.  Sort
 * key for memory types is `used` (or `used_diff`); for sections it's
 * `size` (or `size_diff`).  Direction follows args->sort_reverse with
 * the same descending=default semantics as everywhere else. */
static int summary_sort_diff;
static int summary_sort_reverse;

static int summary_dir(int64_t va, int64_t vb)
{
	if (va < vb)
		return summary_sort_reverse ? 1 : -1;
	if (va > vb)
		return summary_sort_reverse ? -1 : 1;
	return 0;
}

static int summary_cmp_type(const void *a, const void *b)
{
	const struct mm_type *ta = *(const struct mm_type *const *)a;
	const struct mm_type *tb = *(const struct mm_type *const *)b;
	int64_t va = summary_sort_diff ? ta->used_diff : ta->used;
	int64_t vb = summary_sort_diff ? tb->used_diff : tb->used;
	return summary_dir(va, vb);
}

static int summary_cmp_section(const void *a, const void *b)
{
	const struct mm_section *sa = *(const struct mm_section *const *)a;
	const struct mm_section *sb = *(const struct mm_section *const *)b;
	int64_t va = summary_sort_diff ? sa->size_diff : sa->size;
	int64_t vb = summary_sort_diff ? sb->size_diff : sb->size;
	return summary_dir(va, vb);
}

void build_summary_tab(const struct memmap *mm, const struct mm_args *args,
		       struct tab *out)
{
	const char *headers[] = {
	    "Memory Type/Section", "Used [bytes]",  "Used [%]",
	    "Remain [bytes]",	   "Total [bytes]",
	};
	const int aligns[] = {0, 1, 1, 1, 1};
	struct mm_type **types;

	tab_init(out, "Memory Type Usage Summary", headers, 5);

	/*
	 * Sort memory types and sections locally rather than mutating
	 * the underlying memmap.  Sibling views (--archives, raw, json2)
	 * rely on the memmap's insertion order to pin column / key
	 * ordering, so the summary table has to do its own thing here.
	 */
	summary_sort_diff = args->sort_diff;
	summary_sort_reverse = args->sort_reverse;
	types = calloc((size_t)mm->nr_types, sizeof(*types));
	if (mm->nr_types && !types)
		die_errno("calloc");
	for (int i = 0; i < mm->nr_types; i++)
		types[i] = mm->types[i];
	if (mm->nr_types > 1)
		mm_stable_sort((void **)types, mm->nr_types, summary_cmp_type);

	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = types[i];
		struct mm_section **secs = NULL;
		char used[64], pct[32], remain[64], total[64];
		const char *cells[5];
		int64_t free_b;
		double pct_v;

		diff_inline(used, sizeof(used), t->used, t->used_diff,
			    args->diff != NULL);

		if (t->size) {
			double pct_diff = 0.0;
			free_b = t->size - t->used;
			pct_v = (double)t->used * 100.0 / (double)t->size;
			/* Mirror upstream: if the ref had size == 0 (i.e.
			 * size - size_diff == 0), pct_diff degenerates to
			 * (pct - 0) == pct.  Otherwise it's the actual
			 * cur-pct minus ref-pct. */
			if (args->diff) {
				int64_t ref_size = t->size - t->size_diff;
				if (ref_size != 0) {
					double ref_pct =
					    (double)(t->used - t->used_diff) *
					    100.0 / (double)ref_size;
					pct_diff = pct_v - ref_pct;
				} else {
					pct_diff = pct_v;
				}
			}
			fmt_pct_diff(pct, sizeof(pct), pct_v, pct_diff,
				     args->diff != NULL);
			diff_inline(remain, sizeof(remain), free_b,
				    t->size_diff - t->used_diff,
				    args->diff != NULL);
			diff_inline(total, sizeof(total), t->size, t->size_diff,
				    args->diff != NULL);
		} else {
			pct[0] = '\0';
			remain[0] = '\0';
			total[0] = '\0';
		}

		cells[0] = t->name;
		cells[1] = used;
		cells[2] = pct;
		cells[3] = remain;
		cells[4] = total;
		tab_add_row(out, cells, aligns);

		secs = calloc((size_t)t->nr_sections, sizeof(*secs));
		if (t->nr_sections && !secs)
			die_errno("calloc");
		for (int j = 0; j < t->nr_sections; j++)
			secs[j] = t->sections[j];
		if (t->nr_sections > 1)
			mm_stable_sort((void **)secs, t->nr_sections,
				       summary_cmp_section);

		for (int j = 0; j < t->nr_sections; j++) {
			struct mm_section *s = secs[j];
			char pct2[32];
			char sused[64];
			char name[64];

			diff_inline(sused, sizeof(sused), s->size, s->size_diff,
				    args->diff != NULL);

			if (t->size) {
				double sp =
				    (double)s->size * 100.0 / (double)t->size;
				double sp_diff = 0.0;
				if (args->diff) {
					int64_t ref_t_size =
					    t->size - t->size_diff;
					if (ref_t_size != 0) {
						double ref_sp =
						    (double)(s->size -
							     s->size_diff) *
						    100.0 / (double)ref_t_size;
						sp_diff = sp - ref_sp;
					} else {
						sp_diff = sp;
					}
				}
				fmt_pct_diff(pct2, sizeof(pct2), sp, sp_diff,
					     args->diff != NULL);
			} else {
				pct2[0] = '\0';
			}

			snprintf(name, sizeof(name), "   %s",
				 args->abbrev ? s->abbrev_name : s->name);
			cells[0] = name;
			cells[1] = sused;
			cells[2] = pct2;
			cells[3] = "";
			cells[4] = "";
			tab_add_row(out, cells, aligns);
		}
		free(secs);
	}

	free(types);
}

void print_summary_notes(const struct memmap *mm, const struct mm_args *args)
{
	/*
	 * Mirror upstream's split: the image-size line and the
	 * informational note go to stderr (Rich's eprint), so stdout
	 * carries only the table itself -- which is what scripts pipe
	 * out and what the saved-table fixtures compare against.
	 */
	if (args->quiet)
		return;
	fprintf(stderr,
		"Total image size: %lld bytes (.bin may be padded larger)\n",
		(long long)mm->image_size);
	fprintf(stderr,
		"Note: The reported total sizes may be smaller than those "
		"in the technical reference manual due to reserved memory and "
		"application configuration. The total flash size available for "
		"the application is not included by default, as it cannot be "
		"reliably determined due to the presence of other data like "
		"the bootloader, partition table, and application partition "
		"size.\n");
}

/* ------------------------------------------------------------------ */
/* Pivoted table (archives / files / symbols)                          */
/* ------------------------------------------------------------------ */

void build_pivoted_tab(const struct sv_summary *s, const char *title,
		       const char *item_label, const struct mm_args *args,
		       struct tab *out)
{
	const char **headers;
	int nr_cols = 2;
	int *aligns;

	if (s->nr_entries == 0) {
		const char *h[] = {item_label, "Total Size"};
		const int a[] = {0, 1};
		tab_init(out, title, h, 2);
		(void)a;
		return;
	}

	/* Columns: <item>, Total Size, then per skeleton: type, sec1, sec2,
	 * type, sec1, ... */
	{
		const struct sv_entry *first = s->entries[0];
		for (int i = 0; i < first->nr_types; i++)
			nr_cols += 1 + first->types[i]->nr_sections;
	}

	headers = calloc((size_t)nr_cols, sizeof(*headers));
	aligns = calloc((size_t)nr_cols, sizeof(*aligns));
	if (!headers || !aligns)
		die_errno("calloc");

	headers[0] = item_label;
	headers[1] = "Total Size";
	aligns[0] = 0;
	aligns[1] = 1;
	{
		int c = 2;
		const struct sv_entry *first = s->entries[0];
		for (int i = 0; i < first->nr_types; i++) {
			headers[c] = first->types[i]->name;
			aligns[c++] = 1;
			for (int j = 0; j < first->types[i]->nr_sections; j++) {
				const struct sv_section *ss =
				    first->types[i]->sections[j];
				headers[c] =
				    args->abbrev ? ss->abbrev_name : ss->name;
				aligns[c++] = 1;
			}
		}
	}

	tab_init(out, title, headers, nr_cols);

	for (int e = 0; e < s->nr_entries; e++) {
		const struct sv_entry *en = s->entries[e];
		const char **cells = calloc((size_t)nr_cols, sizeof(*cells));
		char **bufs = calloc((size_t)nr_cols, sizeof(*bufs));
		int c = 0;

		if (!cells || !bufs)
			die_errno("calloc");

		bufs[c] =
		    sbuf_strdup(args->abbrev ? en->abbrev_name : en->name);
		cells[c++] = bufs[0];

		bufs[c] = malloc(64);
		if (!bufs[c])
			die_errno("malloc");
		diff_inline(bufs[c], 64, en->size, en->size_diff,
			    args->diff != NULL);
		cells[c] = bufs[c];
		c++;

		for (int i = 0; i < en->nr_types; i++) {
			const struct sv_type *st = en->types[i];

			bufs[c] = malloc(64);
			if (!bufs[c])
				die_errno("malloc");
			diff_inline(bufs[c], 64, st->size, st->size_diff,
				    args->diff != NULL);
			cells[c] = bufs[c];
			c++;

			for (int j = 0; j < st->nr_sections; j++) {
				const struct sv_section *ss = st->sections[j];
				bufs[c] = malloc(64);
				if (!bufs[c])
					die_errno("malloc");
				diff_inline(bufs[c], 64, ss->size,
					    ss->size_diff, args->diff != NULL);
				cells[c] = bufs[c];
				c++;
			}
		}

		tab_add_row(out, cells, aligns);

		for (int i = 0; i < c; i++)
			free(bufs[i]);
		free(bufs);
		free(cells);
	}

	free(headers);
	free(aligns);
}

/* ------------------------------------------------------------------ */
/* Deps table                                                          */
/* ------------------------------------------------------------------ */

void build_deps_tab(const struct dep_summary *d, const struct mm_args *args,
		    struct tab *out)
{
	const char *dep_col_name =
	    args->dep_reverse ? "Dependents" : "Dependencies";
	const char *title = args->dep_reverse
				? "Table of reverse dependencies for archives"
				: "Table of dependencies for archives";
	const int aligns_no_sym[] = {0, 0, 0, 0};
	const int aligns_with_sym[] = {0, 0, 0, 0, 0};
	char dep_size_label[64];
	const char *headers4[4];
	const char *headers5[5];

	snprintf(dep_size_label, sizeof(dep_size_label), "%s Sizes",
		 dep_col_name);

	headers4[0] = "Archive";
	headers4[1] = "Archive Size";
	headers4[2] = dep_col_name;
	headers4[3] = dep_size_label;

	headers5[0] = headers4[0];
	headers5[1] = headers4[1];
	headers5[2] = headers4[2];
	headers5[3] = headers4[3];
	headers5[4] = "Symbols";

	if (args->dep_symbols)
		tab_init(out, title, headers5, 5);
	else
		tab_init(out, title, headers4, 4);

	for (int i = 0; i < d->nr_entries; i++) {
		struct dep_entry *e = d->entries[i];
		const char *arch_name = args->abbrev ? e->abbrev_name : e->name;
		char arch_size[32];

		snprintf(arch_size, sizeof(arch_size), "%lld",
			 (long long)e->size);

		for (int j = 0; j < e->nr_archives; j++) {
			struct dep_archive *da = e->archives[j];
			const char *dep_name =
			    args->abbrev ? da->abbrev_name : da->name;
			char dep_size_str[32];
			const char *cells4[4];
			const char *cells5[5];
			struct sbuf syms = SBUF_INIT;

			snprintf(dep_size_str, sizeof(dep_size_str), "%lld",
				 (long long)da->size);

			if (j == 0) {
				cells4[0] = arch_name;
				cells4[1] = arch_size;
			} else {
				cells4[0] = "";
				cells4[1] = "";
			}
			cells4[2] = dep_name;
			cells4[3] = dep_size_str;

			cells5[0] = cells4[0];
			cells5[1] = cells4[1];
			cells5[2] = cells4[2];
			cells5[3] = cells4[3];

			if (args->dep_symbols) {
				for (int k = 0; k < da->nr_symbols; k++) {
					if (k)
						sbuf_addch(&syms, '\n');
					sbuf_addstr(&syms, da->symbols[k]);
				}
				cells5[4] = syms.buf;
				tab_add_row(out, cells5,
					    args->dep_symbols ? aligns_with_sym
							      : aligns_no_sym);
			} else {
				tab_add_row(out, cells4, aligns_no_sym);
			}
			sbuf_release(&syms);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Format dispatchers                                                  */
/* ------------------------------------------------------------------ */

void fmt_table_summary(const struct memmap *mm, const struct mm_args *args)
{
	struct tab t;
	build_summary_tab(mm, args, &t);
	tab_render_box(&t, args->out);
	tab_release(&t);
	print_summary_notes(mm, args);
}

void fmt_table_archives(const struct memmap *mm, const struct mm_args *args)
{
	struct sv_summary s;
	struct tab t;
	sv_archives(mm, args, &s);
	sv_filter(&s, args);
	sv_sort(&s, args);
	build_pivoted_tab(&s, "Per-archive contributions to ELF file",
			  "Archive File", args, &t);
	tab_render_box(&t, args->out);
	tab_release(&t);
	sv_release(&s);
}

void fmt_table_files(const struct memmap *mm, const struct mm_args *args)
{
	struct sv_summary s;
	struct tab t;
	sv_files(mm, args, &s);
	sv_filter(&s, args);
	sv_sort(&s, args);
	build_pivoted_tab(&s, "Per-file contributions to ELF file",
			  "Object File", args, &t);
	tab_render_box(&t, args->out);
	tab_release(&t);
	sv_release(&s);
}

void fmt_table_symbols(const struct memmap *mm, const struct mm_args *args)
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
	tab_render_box(&t, args->out);
	tab_release(&t);
	sv_release(&s);
}

void fmt_table_deps(const struct map_file *mf, const struct memmap *mm,
		    const struct elf_symbols *syms, const struct mm_args *args)
{
	struct dep_summary d;
	struct tab t;
	dep_build(mf, mm, syms, args, &d);
	dep_filter(&d, args);
	build_deps_tab(&d, args, &t);
	tab_render_box(&t, args->out);
	tab_release(&t);
	dep_release(&d);
}
