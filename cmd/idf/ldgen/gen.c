/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/ldgen/gen.c
 * @brief Full-expansion linker-script generator.
 *
 * Pipeline:
 *   gen_compile()  -- walks parsed .lf fragments, produces a sorted
 *                     rule list (specificity descending, stable).
 *   gen_resolve()  -- for every (archive, object, section) triple in
 *                     the entity DB, finds the first matching rule and
 *                     records a placement.
 *   gen_emit()     -- sorts placements by (target, rule, archive, obj),
 *                     emits per-target explicit listings with flag
 *                     wrappers.
 *
 * See gen.h for the data shape and cmd/idf/ldgen/README.md for the
 * fragment grammar.  The design rationale -- why we don't port
 * Python's entity tree -- is recorded in the project plan.
 */
#include "ice.h"

#include "gen.h"
#include "lf.h"
#include "sinfo.h"

/* ---- Internal: parsed sections/scheme tables --------------------- */

struct gen_sections {
	char *name;
	char **patterns; /**< expanded, deduped */
	int n_patterns;
	int alloc_patterns;
};

struct gen_scheme_entry {
	char *sections_ref;
	char *target;
};

struct gen_scheme {
	char *name;
	struct gen_scheme_entry *entries;
	int n_entries;
	int alloc_entries;
};

/* ---- Small helpers ---------------------------------------------- */

/* Tiny glob: '*' matches any chars (including none); all other chars
 * are literal.  Matches the subset of fnmatch we actually use
 * (section patterns like ".text.*", object-name suffixes like
 * "obj.*.o").  No '?' or bracket expressions needed.
 */
static int glob_match(const char *pat, const char *s)
{
	while (*pat) {
		if (*pat == '*') {
			while (*pat == '*')
				pat++;
			if (!*pat)
				return 1;
			for (; *s; s++)
				if (glob_match(pat, s))
					return 1;
			return 0;
		}
		if (*pat != *s)
			return 0;
		pat++;
		s++;
	}
	return *s == '\0';
}

/* Strip trailing ".o" or ".obj" suffix.  Returns a fresh heap
 * allocation, or a copy if neither suffix is present. */
static char *strip_obj_suffix(const char *name)
{
	size_t n = strlen(name);
	if (n >= 2 && !strcmp(name + n - 2, ".o"))
		return sbuf_strndup(name, n - 2);
	if (n >= 4 && !strcmp(name + n - 4, ".obj"))
		return sbuf_strndup(name, n - 4);
	return sbuf_strdup(name);
}

/* Append a string pointer if not already present (linear scan, fine
 * for the 1-3 entries per sections fragment). */
static void patterns_add_unique(char ***v, int *n, int *alloc, const char *s)
{
	for (int i = 0; i < *n; i++)
		if (!strcmp((*v)[i], s))
			return;
	ALLOC_GROW(*v, *n + 1, *alloc);
	(*v)[(*n)++] = sbuf_strdup(s);
}

/* Expand a sections fragment entry like ".text+" into both literal and
 * glob forms (matches Python's Sections.get_section_data_from_entry). */
static void expand_section_entry(const char *entry, char ***v, int *n,
				 int *alloc)
{
	const char *plus = strchr(entry, '+');
	if (!plus) {
		patterns_add_unique(v, n, alloc, entry);
		return;
	}
	/* ".text+" -> ".text" and ".text.*" */
	size_t prefix_len = plus - entry;
	const char *suffix = plus + 1;

	struct sbuf sb = SBUF_INIT;
	sbuf_add(&sb, entry, prefix_len);
	sbuf_addstr(&sb, suffix);
	patterns_add_unique(v, n, alloc, sb.buf);

	sbuf_reset(&sb);
	sbuf_add(&sb, entry, prefix_len);
	sbuf_addstr(&sb, ".*");
	sbuf_addstr(&sb, suffix);
	patterns_add_unique(v, n, alloc, sb.buf);

	sbuf_release(&sb);
}

/* ---- Fragment -> tables ----------------------------------------- */

/* Evaluate a conditional's branch list against @p cfg and return the
 * active arm's statement list (or NULL if none).  @p cfg may be NULL,
 * in which case no branch is chosen. */
static const struct lf_stmt *pick_cond_branch(const struct lf_branch *branches,
					      int nb,
					      const struct sdkconfig *cfg,
					      int *out_n)
{
	if (!cfg) {
		*out_n = 0;
		return NULL;
	}
	for (int i = 0; i < nb; i++) {
		const struct lf_branch *b = &branches[i];
		if (!b->expr || sdkconfig_eval(cfg, b->expr)) {
			*out_n = b->n_stmts;
			return b->stmts;
		}
	}
	*out_n = 0;
	return NULL;
}

static void collect_stmts(const struct lf_stmt *stmts, int n,
			  void (*visit)(const struct lf_entry *, void *),
			  void *ud, const struct sdkconfig *cfg)
{
	for (int i = 0; i < n; i++) {
		if (!stmts[i].is_cond) {
			visit(&stmts[i].u.entry, ud);
			continue;
		}
		int nb;
		const struct lf_stmt *arm =
		    pick_cond_branch(stmts[i].u.cond.branches,
				     stmts[i].u.cond.n_branches, cfg, &nb);
		if (arm)
			collect_stmts(arm, nb, visit, ud, cfg);
	}
}

static void add_sections_entry(const struct lf_entry *e, void *ud)
{
	struct gen_sections *s = ud;
	expand_section_entry(e->name, &s->patterns, &s->n_patterns,
			     &s->alloc_patterns);
}

static void add_scheme_entry(const struct lf_entry *e, void *ud)
{
	struct gen_scheme *sc = ud;
	ALLOC_GROW(sc->entries, sc->n_entries + 1, sc->alloc_entries);
	struct gen_scheme_entry *ent = &sc->entries[sc->n_entries++];
	ent->sections_ref = sbuf_strdup(e->name);
	ent->target = sbuf_strdup(e->target);
}

static struct gen_sections *ctx_find_sections(struct gen_ctx *ctx,
					      const char *name)
{
	for (int i = 0; i < ctx->n_sections; i++)
		if (!strcmp(ctx->sections[i].name, name))
			return &ctx->sections[i];
	return NULL;
}

static struct gen_scheme *ctx_find_scheme(struct gen_ctx *ctx, const char *name)
{
	for (int i = 0; i < ctx->n_schemes; i++)
		if (!strcmp(ctx->schemes[i].name, name))
			return &ctx->schemes[i];
	return NULL;
}

/* ---- Rule emission from mappings -------------------------------- */

/* Find the flag list attached to an entry for a given (sections, target)
 * pair.  Returns NULL if none. */
static const struct lf_flag_item *find_flag_item(const struct lf_entry *e,
						 const char *sections_ref,
						 const char *target)
{
	for (int i = 0; i < e->n_flag_items; i++) {
		const struct lf_flag_item *fi = &e->flag_items[i];
		if (!strcmp(fi->sections, sections_ref) &&
		    !strcmp(fi->target, target))
			return fi;
	}
	return NULL;
}

static struct gen_flag *clone_flags(const struct lf_flag *src, int n)
{
	if (!n)
		return NULL;
	struct gen_flag *v = calloc((size_t)n, sizeof(*v));
	if (!v)
		die_errno("calloc");
	for (int i = 0; i < n; i++) {
		v[i].kind = (enum gen_flag_kind)src[i].kind;
		v[i].alignment = src[i].alignment;
		v[i].pre = src[i].pre;
		v[i].post = src[i].post;
		v[i].sort_first =
		    src[i].sort_first ? sbuf_strdup(src[i].sort_first) : NULL;
		v[i].sort_second =
		    src[i].sort_second ? sbuf_strdup(src[i].sort_second) : NULL;
		v[i].symbol = src[i].symbol ? sbuf_strdup(src[i].symbol) : NULL;
	}
	return v;
}

static int compute_specificity(const char *arch, const char *obj,
			       const char *sym)
{
	if (sym)
		return 3;
	if (obj)
		return 2;
	if (arch)
		return 1;
	return 0;
}

static void emit_rule(struct gen_ctx *ctx, const char *archive, const char *obj,
		      const char *sym, const struct gen_sections *secs,
		      const char *target, const struct lf_flag *flags,
		      int n_flags)
{
	ALLOC_GROW(ctx->rules, ctx->n_rules + 1, ctx->alloc_rules);
	struct gen_rule *r = &ctx->rules[ctx->n_rules];
	memset(r, 0, sizeof(*r));

	r->archive = archive ? sbuf_strdup(archive) : NULL;
	r->object = obj ? sbuf_strdup(obj) : NULL;
	r->symbol = sym ? sbuf_strdup(sym) : NULL;
	r->target = sbuf_strdup(target);

	r->section_patterns = calloc((size_t)secs->n_patterns, sizeof(char *));
	if (!r->section_patterns)
		die_errno("calloc");
	r->n_section_patterns = secs->n_patterns;
	for (int i = 0; i < secs->n_patterns; i++)
		r->section_patterns[i] = sbuf_strdup(secs->patterns[i]);

	r->flags = clone_flags(flags, n_flags);
	r->n_flags = n_flags;

	r->specificity = compute_specificity(r->archive, r->object, r->symbol);
	r->source_order = ctx->n_rules;

	ctx->n_rules++;
}

/* Resolve the archive statement (which may be a conditional) to a
 * single archive name. */
static const char *resolve_archive(const struct lf_stmt *stmts, int n,
				   const struct sdkconfig *cfg,
				   const char *mapname)
{
	for (int i = 0; i < n; i++) {
		if (!stmts[i].is_cond)
			return stmts[i].u.entry.name;
		int nb;
		const struct lf_stmt *arm =
		    pick_cond_branch(stmts[i].u.cond.branches,
				     stmts[i].u.cond.n_branches, cfg, &nb);
		const char *got = resolve_archive(arm, nb, cfg, mapname);
		if (got)
			return got;
	}
	return NULL;
}

static void compile_entry(struct gen_ctx *ctx, const char *archive,
			  const struct lf_entry *e, const char *mapname)
{
	const char *obj = e->name;
	const char *sym = e->target;
	const char *scheme_name = e->scheme;
	if (!strcmp(obj, "*"))
		obj = NULL;

	struct gen_scheme *sc = ctx_find_scheme(ctx, scheme_name);
	if (!sc)
		die("mapping '%s' references undefined scheme '%s'", mapname,
		    scheme_name);

	for (int j = 0; j < sc->n_entries; j++) {
		const char *sections_ref = sc->entries[j].sections_ref;
		const char *target = sc->entries[j].target;
		struct gen_sections *secs =
		    ctx_find_sections(ctx, sections_ref);
		if (!secs)
			die("scheme '%s' references undefined sections '%s'",
			    scheme_name, sections_ref);

		const struct lf_flag_item *fi =
		    find_flag_item(e, sections_ref, target);
		emit_rule(ctx, archive, obj, sym, secs, target,
			  fi ? fi->flags : NULL, fi ? fi->n_flags : 0);
	}
}

static void compile_entries(struct gen_ctx *ctx, const char *archive,
			    const struct lf_stmt *stmts, int n,
			    const struct sdkconfig *cfg, const char *mapname)
{
	for (int i = 0; i < n; i++) {
		if (!stmts[i].is_cond) {
			compile_entry(ctx, archive, &stmts[i].u.entry, mapname);
			continue;
		}
		int nb;
		const struct lf_stmt *arm =
		    pick_cond_branch(stmts[i].u.cond.branches,
				     stmts[i].u.cond.n_branches, cfg, &nb);
		if (arm)
			compile_entries(ctx, archive, arm, nb, cfg, mapname);
	}
}

static void compile_mapping(struct gen_ctx *ctx, const struct lf_frag *f,
			    const struct sdkconfig *cfg)
{
	const char *archive = resolve_archive(
	    f->u.map.archive, f->u.map.n_archive, cfg, f->u.map.name);
	if (!archive)
		die("mapping '%s' has no archive after conditional evaluation",
		    f->u.map.name);
	if (!strcmp(archive, "*"))
		archive = NULL;

	compile_entries(ctx, archive, f->u.map.entries, f->u.map.n_entries, cfg,
			f->u.map.name);
}

static int rule_cmp(const void *a, const void *b)
{
	/* Sort ASCENDING by specificity so emission order matches Python's:
	 * root-level wildcards first, then archive, object, symbol.
	 * Within a specificity level, preserve source order for stability.
	 * Resolution walks the array backwards to implement most-specific-
	 * wins without disturbing emit order. */
	const struct gen_rule *ra = a;
	const struct gen_rule *rb = b;
	if (ra->specificity != rb->specificity)
		return ra->specificity - rb->specificity;
	return ra->source_order - rb->source_order;
}

static void walk_frag(struct gen_ctx *ctx, const struct lf_frag *fr,
		      const struct sdkconfig *cfg, int pass);

static void walk_frags(struct gen_ctx *ctx, const struct lf_frag *frags, int n,
		       const struct sdkconfig *cfg, int pass)
{
	for (int i = 0; i < n; i++)
		walk_frag(ctx, &frags[i], cfg, pass);
}

static void walk_frag(struct gen_ctx *ctx, const struct lf_frag *fr,
		      const struct sdkconfig *cfg, int pass)
{
	if (fr->kind == LF_FRAG_COND) {
		if (!cfg)
			return;
		for (int i = 0; i < fr->u.cond.n; i++) {
			const struct lf_frag_branch *b =
			    &fr->u.cond.branches[i];
			if (!b->expr || sdkconfig_eval(cfg, b->expr)) {
				walk_frags(ctx, b->frags, b->n_frags, cfg,
					   pass);
				return;
			}
		}
		return;
	}
	if (pass == 0) {
		if (fr->kind == LF_SECTIONS) {
			ALLOC_GROW(ctx->sections, ctx->n_sections + 1,
				   ctx->alloc_sections);
			struct gen_sections *s =
			    &ctx->sections[ctx->n_sections++];
			memset(s, 0, sizeof(*s));
			s->name = sbuf_strdup(fr->u.sec.name);
			collect_stmts(fr->u.sec.stmts, fr->u.sec.n,
				      add_sections_entry, s, cfg);
		} else if (fr->kind == LF_SCHEME) {
			ALLOC_GROW(ctx->schemes, ctx->n_schemes + 1,
				   ctx->alloc_schemes);
			struct gen_scheme *s = &ctx->schemes[ctx->n_schemes++];
			memset(s, 0, sizeof(*s));
			s->name = sbuf_strdup(fr->u.sch.name);
			collect_stmts(fr->u.sch.stmts, fr->u.sch.n,
				      add_scheme_entry, s, cfg);
		}
	} else if (pass == 1 && fr->kind == LF_MAPPING) {
		compile_mapping(ctx, fr, cfg);
	}
}

void gen_compile(struct gen_ctx *ctx, struct lf_file **files, int n_files,
		 const struct sdkconfig *cfg)
{
	/* Pass 0: collect sections and scheme fragments.
	 * Pass 1: walk mappings, emit rules. */
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < n_files; i++)
			walk_frags(ctx, files[i]->frags, files[i]->n_frags, cfg,
				   pass);

	/* Sort: specificity asc, source_order asc. */
	qsort(ctx->rules, (size_t)ctx->n_rules, sizeof(*ctx->rules), rule_cmp);
}

/* ---- Resolution -------------------------------------------------- */

static int match_archive(const struct gen_rule *r, const char *archive)
{
	if (!r->archive)
		return 1;
	return !strcmp(r->archive, archive);
}

static int match_object(const struct gen_rule *r, const char *object)
{
	if (!r->object)
		return 1;
	/* Python matches obj.*.o, obj.o, obj.*.obj, obj.obj.  Our input
	 * has objects like "a.o" or "croutine.c.obj". */
	struct sbuf pat = SBUF_INIT;
	int hit = 0;
	static const char *const suffixes[] = {".*.o", ".o", ".*.obj", ".obj"};
	for (size_t k = 0; k < sizeof(suffixes) / sizeof(suffixes[0]) && !hit;
	     k++) {
		sbuf_reset(&pat);
		sbuf_addstr(&pat, r->object);
		sbuf_addstr(&pat, suffixes[k]);
		hit = glob_match(pat.buf, object);
	}
	sbuf_release(&pat);
	return hit;
}

static int section_matches_symbol_pattern(const char *pat, const char *symbol,
					  const char *section)
{
	/* Python's contract: for every pattern containing ".*", substitute
	 * the symbol to produce two effective patterns:
	 *   ".{sym}"      (exact)
	 *   ".{sym}.*"    (glob)
	 * Match section against either.  If the pattern has no ".*" it
	 * never claims any symbol-level sections. */
	const char *star = strstr(pat, ".*");
	if (!star)
		return 0;
	struct sbuf sb = SBUF_INIT;
	size_t prefix = star - pat;
	const char *after = star + 2;

	/* exact: prefix + "." + symbol + after */
	sbuf_add(&sb, pat, prefix);
	sbuf_addch(&sb, '.');
	sbuf_addstr(&sb, symbol);
	sbuf_addstr(&sb, after);
	int hit = !strcmp(sb.buf, section);

	/* glob: prefix + "." + symbol + ".*" + after */
	if (!hit) {
		sbuf_reset(&sb);
		sbuf_add(&sb, pat, prefix);
		sbuf_addch(&sb, '.');
		sbuf_addstr(&sb, symbol);
		sbuf_addstr(&sb, ".*");
		sbuf_addstr(&sb, after);
		hit = glob_match(sb.buf, section);
	}
	sbuf_release(&sb);
	return hit;
}

static int match_section(const struct gen_rule *r, const char *section)
{
	if (r->symbol) {
		for (int i = 0; i < r->n_section_patterns; i++)
			if (section_matches_symbol_pattern(
				r->section_patterns[i], r->symbol, section))
				return 1;
		return 0;
	}
	for (int i = 0; i < r->n_section_patterns; i++)
		if (glob_match(r->section_patterns[i], section))
			return 1;
	return 0;
}

static int find_rule(const struct gen_ctx *ctx, const char *archive,
		     const char *object_stripped, const char *object_raw,
		     const char *section)
{
	/* Rules are sorted specificity ASC; walk backwards so most-
	 * specific candidates are tried first. */
	for (int i = ctx->n_rules - 1; i >= 0; i--) {
		const struct gen_rule *r = &ctx->rules[i];
		if (!match_archive(r, archive))
			continue;
		if (!match_object(r, object_raw))
			continue;
		if (!match_section(r, section))
			continue;
		(void)object_stripped;
		return i;
	}
	return -1;
}

void gen_resolve(struct gen_ctx *ctx, const struct sinfo_db *db)
{
	for (int ai = 0; ai < db->n_archives; ai++) {
		const struct sinfo_archive *a = &db->archives[ai];
		for (int oi = 0; oi < a->n_objs; oi++) {
			const struct sinfo_object *o = &a->objs[oi];
			char *ostripped = strip_obj_suffix(o->name);
			for (int si = 0; si < o->n_sections; si++) {
				const char *s = o->sections[si];
				int ri = find_rule(ctx, a->name, ostripped,
						   o->name, s);
				if (ri < 0)
					continue;
				ALLOC_GROW(ctx->placements,
					   ctx->n_placements + 1,
					   ctx->alloc_placements);
				struct gen_placement *p =
				    &ctx->placements[ctx->n_placements++];
				p->archive = a->name;
				p->object = o->name;
				p->section = s;
				p->target = ctx->rules[ri].target;
				p->rule_idx = ri;
			}
			free(ostripped);
		}
	}
}

/* ---- Emission ---------------------------------------------------- */

static int placement_cmp_emit(const void *a, const void *b)
{
	const struct gen_placement *pa = a;
	const struct gen_placement *pb = b;
	int c;
	if ((c = strcmp(pa->target, pb->target)) != 0)
		return c;
	if (pa->rule_idx != pb->rule_idx)
		return pa->rule_idx - pb->rule_idx;
	if ((c = strcmp(pa->archive, pb->archive)) != 0)
		return c;
	if ((c = strcmp(pa->object, pb->object)) != 0)
		return c;
	return strcmp(pa->section, pb->section);
}

static int placement_cmp_canonical(const void *a, const void *b)
{
	const struct gen_placement *pa = a;
	const struct gen_placement *pb = b;
	int c;
	if ((c = strcmp(pa->archive, pb->archive)) != 0)
		return c;
	if ((c = strcmp(pa->object, pb->object)) != 0)
		return c;
	return strcmp(pa->section, pb->section);
}

static int has_flag(const struct gen_rule *r, enum gen_flag_kind k)
{
	for (int i = 0; i < r->n_flags; i++)
		if (r->flags[i].kind == k)
			return 1;
	return 0;
}

static void emit_pre_wrappers(FILE *out, const char *indent,
			      const struct gen_rule *r)
{
	for (int i = 0; i < r->n_flags; i++) {
		const struct gen_flag *f = &r->flags[i];
		if (f->kind == GF_SURROUND) {
			/* SURROUND always wraps both sides. */
			fprintf(out, "%s_%s_start = ABSOLUTE(.);\n", indent,
				f->symbol);
		} else if (f->kind == GF_ALIGN && f->pre) {
			fprintf(out, "%s. = ALIGN(%d);\n", indent,
				f->alignment);
		}
	}
}

static void emit_post_wrappers(FILE *out, const char *indent,
			       const struct gen_rule *r)
{
	for (int i = 0; i < r->n_flags; i++) {
		const struct gen_flag *f = &r->flags[i];
		if (f->kind == GF_SURROUND) {
			fprintf(out, "%s_%s_end = ABSOLUTE(.);\n", indent,
				f->symbol);
		} else if (f->kind == GF_ALIGN && f->post) {
			fprintf(out, "%s. = ALIGN(%d);\n", indent,
				f->alignment);
		}
	}
}

static const struct gen_flag *find_flag(const struct gen_rule *r,
					enum gen_flag_kind k)
{
	for (int i = 0; i < r->n_flags; i++)
		if (r->flags[i].kind == k)
			return &r->flags[i];
	return NULL;
}

/* Wrap a sections-list string in SORT_BY_<first>[(SORT_BY_<second>)] per
 * Python's output_commands.py.  Returns the number of closing ')' the
 * caller must emit after the section list. */
static int emit_sort_open(FILE *out, const struct gen_flag *sort)
{
	if (!sort)
		return 0;
	const char *first = sort->sort_first;
	const char *second = sort->sort_second;

	static const char *const tbl[] = {
	    "name",	     "SORT_BY_NAME",
	    "alignment",     "SORT_BY_ALIGNMENT",
	    "init_priority", "SORT_BY_INIT_PRIORITY",
	};
	const char *f_wrap = NULL, *s_wrap = NULL;
	if (!first && !second) {
		fputs("SORT(", out);
		return 1;
	}
	for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i += 2)
		if (first && !strcmp(first, tbl[i]))
			f_wrap = tbl[i + 1];
	for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i += 2)
		if (second && !strcmp(second, tbl[i]))
			s_wrap = tbl[i + 1];
	if (f_wrap)
		fprintf(out, "%s(", f_wrap);
	if (s_wrap)
		fprintf(out, "%s(", s_wrap);
	return (f_wrap ? 1 : 0) + (s_wrap ? 1 : 0);
}

static void emit_input_section_desc(FILE *out, const char *indent,
				    const struct gen_rule *r,
				    const char *archive, const char *object,
				    char *const *sections, int n_sections)
{
	fputs(indent, out);
	int keep = has_flag(r, GF_KEEP);
	if (keep)
		fputs("KEEP(", out);
	fprintf(out, "*%s:%s(", archive, object);
	const struct gen_flag *sort = find_flag(r, GF_SORT);
	for (int i = 0; i < n_sections; i++) {
		if (i)
			fputc(' ', out);
		int close = emit_sort_open(out, sort);
		fputs(sections[i], out);
		while (close-- > 0)
			fputc(')', out);
	}
	fputc(')', out);
	if (keep)
		fputc(')', out);
	fputc('\n', out);
}

/* Emit a root-level rule (archive wildcard) as a single catch-all
 * `*(section_patterns)` line.  This is how Python emits the default
 * mapping and also the only way to claim sections from archives that
 * weren't passed to ldgen (e.g. libc.a that the linker pulls in). */
static void emit_root_wildcard(FILE *out, const char *indent,
			       const struct gen_rule *r)
{
	fputs(indent, out);
	int keep = has_flag(r, GF_KEEP);
	if (keep)
		fputs("KEEP(", out);
	fputs("*(", out);
	const struct gen_flag *sort = find_flag(r, GF_SORT);
	for (int i = 0; i < r->n_section_patterns; i++) {
		if (i)
			fputc(' ', out);
		int close = emit_sort_open(out, sort);
		fputs(r->section_patterns[i], out);
		while (close-- > 0)
			fputc(')', out);
	}
	fputc(')', out);
	if (keep)
		fputc(')', out);
	fputc('\n', out);
}

/* Emit a single target's worth of placements starting from v[*i].
 * Consumes v[*i..j) where all have the same target; on return *i == j.
 * If @p with_header is nonzero, prefixes with a "[target]\n" line.
 */
static void emit_one_target(FILE *out, const char *indent,
			    const struct gen_ctx *ctx, struct gen_placement *v,
			    int n_total, int *i, int with_header)
{
	const char *target = v[*i].target;
	if (with_header)
		fprintf(out, "[%s]\n", target);

	int prev_rule = -1;
	while (*i < n_total && !strcmp(v[*i].target, target)) {
		const struct gen_rule *r = &ctx->rules[v[*i].rule_idx];

		if (v[*i].rule_idx != prev_rule) {
			if (prev_rule >= 0)
				emit_post_wrappers(out, indent,
						   &ctx->rules[prev_rule]);
			emit_pre_wrappers(out, indent, r);
			prev_rule = v[*i].rule_idx;
		}

		/* Root-level rule (archive == NULL && object == NULL):
		 * emit ONE catch-all wildcard and skip all its placements.
		 * This is what lets orphan sections from archives not in our
		 * entity DB still get placed -- the linker resolves the
		 * wildcard against whatever input files it actually sees. */
		if (!r->archive && !r->object && !r->symbol) {
			emit_root_wildcard(out, indent, r);
			int j = *i;
			while (j < n_total && !strcmp(v[j].target, target) &&
			       v[j].rule_idx == v[*i].rule_idx)
				j++;
			*i = j;
			continue;
		}

		/* Coalesce same (archive, object): collect all sections for
		 * one emit line.  The outer while already confirmed
		 * v[*i].target == target and rule_idx matches, and the
		 * (archive, object) pair at *i trivially matches itself --
		 * so walk starts at *i+1 and n >= 1 by construction. */
		int n = 1;
		int j = *i + 1;
		while (j < n_total && !strcmp(v[j].target, target) &&
		       v[j].rule_idx == v[*i].rule_idx &&
		       !strcmp(v[j].archive, v[*i].archive) &&
		       !strcmp(v[j].object, v[*i].object)) {
			j++;
			n++;
		}
		char **secs = malloc((size_t)n * sizeof(*secs));
		if (!secs)
			die_errno("malloc");
		for (int k = 0; k < n; k++)
			secs[k] = (char *)v[*i + k].section;
		emit_input_section_desc(out, indent, r, v[*i].archive,
					v[*i].object, secs, n);
		free(secs);
		*i = j;
	}
	if (prev_rule >= 0)
		emit_post_wrappers(out, indent, &ctx->rules[prev_rule]);
}

void gen_emit(FILE *out, const struct gen_ctx *ctx)
{
	if (!ctx->n_placements)
		return;

	/* Copy placements so we can sort without disturbing the canonical
	 * order the caller may rely on afterwards. */
	struct gen_placement *v =
	    malloc((size_t)ctx->n_placements * sizeof(*v));
	if (!v)
		die_errno("malloc");
	memcpy(v, ctx->placements, (size_t)ctx->n_placements * sizeof(*v));
	qsort(v, (size_t)ctx->n_placements, sizeof(*v), placement_cmp_emit);

	int i = 0;
	while (i < ctx->n_placements) {
		emit_one_target(out, "    ", ctx, v, ctx->n_placements, &i, 1);
		fputc('\n', out);
	}
	free(v);
}

void gen_emit_target(FILE *out, const char *indent, const struct gen_ctx *ctx,
		     const char *target)
{
	if (!ctx->n_placements)
		return;
	struct gen_placement *v =
	    malloc((size_t)ctx->n_placements * sizeof(*v));
	if (!v)
		die_errno("malloc");
	memcpy(v, ctx->placements, (size_t)ctx->n_placements * sizeof(*v));
	qsort(v, (size_t)ctx->n_placements, sizeof(*v), placement_cmp_emit);

	int i = 0;
	while (i < ctx->n_placements) {
		if (!strcmp(v[i].target, target)) {
			emit_one_target(out, indent, ctx, v, ctx->n_placements,
					&i, 0);
			break;
		}
		/* Skip to next target boundary. */
		const char *t = v[i].target;
		while (i < ctx->n_placements && !strcmp(v[i].target, t))
			i++;
	}
	free(v);
}

enum marker_kind {
	MK_MAPPING,
	MK_ARRAYS,
	MK_MUTABLE,
};

/* Parse a template line of the form "<indent><keyword>[<target>]".
 * Accepts "mapping", "arrays", or "mutable"; stores which in
 * *kind_out.  Returns heap-allocated target name or NULL on no match.
 */
static char *match_marker(const char *line, char **indent_out,
			  enum marker_kind *kind_out)
{
	const char *p = line;
	const char *ws = p;
	while (*p == ' ' || *p == '\t')
		p++;

	static const struct {
		const char *kw;
		enum marker_kind kind;
	} keywords[] = {
	    {"mapping", MK_MAPPING},
	    {"arrays", MK_ARRAYS},
	    {"mutable", MK_MUTABLE},
	};
	for (size_t k = 0; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
		size_t kl = strlen(keywords[k].kw);
		if (strncmp(p, keywords[k].kw, kl) != 0)
			continue;
		const char *q = p + kl;
		if (*q != '[')
			continue;
		q++;
		const char *name = q;
		while (*q && *q != ']')
			q++;
		if (*q != ']')
			continue;
		size_t namelen = q - name;
		if (!namelen)
			continue;
		q++;
		while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
			q++;
		if (*q != '\0')
			continue; /* trailing garbage -- not a marker */
		*indent_out = sbuf_strndup(ws, p - ws);
		*kind_out = keywords[k].kind;
		return sbuf_strndup(name, namelen);
	}
	return NULL;
}

void gen_fill_template(FILE *out, const struct gen_ctx *ctx,
		       const char *template_path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, template_path) < 0)
		die_errno("cannot read '%s'", template_path);

	fprintf(out, "/* Automatically generated file; DO NOT EDIT */\n");
	fprintf(out, "/* ice idf ldgen -- Generated from: %s */\n\n",
		template_path);

	size_t pos = 0;
	char *line;
	while ((line = sbuf_getline(sb.buf, sb.len, &pos)) != NULL) {
		char *indent = NULL;
		enum marker_kind kind;
		char *target = match_marker(line, &indent, &kind);
		if (target) {
			/* Only "mapping" markers get filled.  "arrays" and
			 * "mutable" cover tied / mutable-library placements
			 * respectively; we don't produce those yet, so those
			 * markers expand to nothing. */
			if (kind == MK_MAPPING)
				gen_emit_target(out, indent, ctx, target);
			free(indent);
			free(target);
		} else {
			fputs(line, out);
			fputc('\n', out);
		}
	}

	sbuf_release(&sb);
}

void gen_emit_canonical(FILE *out, const struct gen_ctx *ctx)
{
	if (!ctx->n_placements)
		return;
	struct gen_placement *v =
	    malloc((size_t)ctx->n_placements * sizeof(*v));
	if (!v)
		die_errno("malloc");
	memcpy(v, ctx->placements, (size_t)ctx->n_placements * sizeof(*v));
	qsort(v, (size_t)ctx->n_placements, sizeof(*v),
	      placement_cmp_canonical);
	for (int i = 0; i < ctx->n_placements; i++)
		fprintf(out, "%s|%s|%s|%s\n", v[i].archive, v[i].object,
			v[i].section, v[i].target);
	free(v);
}

/* ---- Cleanup ----------------------------------------------------- */

static void free_rule(struct gen_rule *r)
{
	free(r->archive);
	free(r->object);
	free(r->symbol);
	for (int i = 0; i < r->n_section_patterns; i++)
		free(r->section_patterns[i]);
	free(r->section_patterns);
	free(r->target);
	for (int i = 0; i < r->n_flags; i++) {
		free(r->flags[i].sort_first);
		free(r->flags[i].sort_second);
		free(r->flags[i].symbol);
	}
	free(r->flags);
}

void gen_free(struct gen_ctx *ctx)
{
	for (int i = 0; i < ctx->n_sections; i++) {
		free(ctx->sections[i].name);
		for (int j = 0; j < ctx->sections[i].n_patterns; j++)
			free(ctx->sections[i].patterns[j]);
		free(ctx->sections[i].patterns);
	}
	free(ctx->sections);

	for (int i = 0; i < ctx->n_schemes; i++) {
		free(ctx->schemes[i].name);
		for (int j = 0; j < ctx->schemes[i].n_entries; j++) {
			free(ctx->schemes[i].entries[j].sections_ref);
			free(ctx->schemes[i].entries[j].target);
		}
		free(ctx->schemes[i].entries);
	}
	free(ctx->schemes);

	for (int i = 0; i < ctx->n_rules; i++)
		free_rule(&ctx->rules[i]);
	free(ctx->rules);

	free(ctx->placements);
	memset(ctx, 0, sizeof(*ctx));
}
