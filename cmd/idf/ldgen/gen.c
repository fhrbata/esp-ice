/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/ldgen/gen.c
 * @brief Rule-driven linker-script generator.
 *
 * Pipeline:
 *
 *   gen_compile() -- walks parsed .lf fragments, produces a rule list
 *                    sorted by (specificity ASC, source_order ASC).
 *   gen_resolve() -- four sub-passes:
 *                      (a) drop symbol rules whose archive or object
 *                          isn't in the DB;
 *                      (b) walk the DB and attach every (archive,
 *                          object, section) triple to its most-
 *                          specific surviving rule's @c matches;
 *                      (c) drop symbol rules left with empty matches;
 *                      (d) populate every surviving rule's per-rule
 *                          @c exclude_list with the file patterns of
 *                          more-specific rules that route to a
 *                          different target -- the basis's glob-
 *                          emitting line carries this list as
 *                          @c{EXCLUDE_FILE(...)} so cross-target
 *                          placement is order-independent.
 *   gen_emit()    -- iterates rules per target.  Each non-dropped rule
 *                    emits its frame (SURROUND/ALIGN/KEEP/SORT
 *                    wrappers) plus a body of literal-section
 *                    listings per (archive, object) group.  Root
 *                    rules also emit a @c{*(...)} catch-all carrying
 *                    @c{EXCLUDE_FILE(<R->exclude_list>)} when set.
 *                    Archive rules without DB matches emit a fallback
 *                    selector with the same EXCLUDE_FILE treatment.
 *
 * See gen.h and cmd/idf/ldgen/README.md for the design rationale.
 */
#include "ice.h"

#include "gen.h"
#include "lf.h"
#include "sinfo.h"

#include "cmd/idf/kconfgen/kc_ast.h"
#include "cmd/idf/kconfgen/kc_eval.h"

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

/*
 * Evaluate a @c .lf conditional expression using the kconfgen engine.
 *
 * @p cfg is declared const to match the rest of gen.c's signatures,
 * but kc_expr_parse_string may intern new unknown identifiers into
 * the symtab -- harmless here since ldgen does not write sdkconfig
 * back.
 */
static int eval_lf_cond(const struct kc_ctx *cfg, const char *expr)
{
	struct kexpr *e =
	    kc_expr_parse_string((struct kc_ctx *)cfg, expr, "<lf>");
	int result = kc_expr_bool(e);
	kc_expr_free(e);
	return result;
}

/* Evaluate a conditional's branch list and return the active arm's
 * statement list (or NULL if none).  @p cfg may be NULL, in which
 * case no branch is chosen. */
static const struct lf_stmt *pick_cond_branch(const struct lf_branch *branches,
					      int nb, const struct kc_ctx *cfg,
					      int *out_n)
{
	if (!cfg) {
		*out_n = 0;
		return NULL;
	}
	for (int i = 0; i < nb; i++) {
		const struct lf_branch *b = &branches[i];
		if (!b->expr || eval_lf_cond(cfg, b->expr)) {
			*out_n = b->n_stmts;
			return b->stmts;
		}
	}
	*out_n = 0;
	return NULL;
}

static void collect_stmts(const struct lf_stmt *stmts, int n,
			  void (*visit)(const struct lf_entry *, void *),
			  void *ud, const struct kc_ctx *cfg)
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

/* ---- Rule construction from mappings ---------------------------- */

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

static void add_rule(struct gen_ctx *ctx, const char *archive, const char *obj,
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
				   const struct kc_ctx *cfg,
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
		add_rule(ctx, archive, obj, sym, secs, target,
			 fi ? fi->flags : NULL, fi ? fi->n_flags : 0);
	}
}

static void compile_entries(struct gen_ctx *ctx, const char *archive,
			    const struct lf_stmt *stmts, int n,
			    const struct kc_ctx *cfg, const char *mapname)
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
			    const struct kc_ctx *cfg)
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
	/* Sort ASCENDING by specificity so the resolution loop (in
	 * find_rule) can walk the array backwards to implement
	 * most-specific-wins.  Within a specificity level, preserve source
	 * order for stability. */
	const struct gen_rule *ra = a;
	const struct gen_rule *rb = b;
	if (ra->specificity != rb->specificity)
		return ra->specificity - rb->specificity;
	return ra->source_order - rb->source_order;
}

static void walk_frag(struct gen_ctx *ctx, const struct lf_frag *fr,
		      const struct kc_ctx *cfg, int pass);

static void walk_frags(struct gen_ctx *ctx, const struct lf_frag *frags, int n,
		       const struct kc_ctx *cfg, int pass)
{
	for (int i = 0; i < n; i++)
		walk_frag(ctx, &frags[i], cfg, pass);
}

static void walk_frag(struct gen_ctx *ctx, const struct lf_frag *fr,
		      const struct kc_ctx *cfg, int pass)
{
	if (fr->kind == LF_FRAG_COND) {
		if (!cfg)
			return;
		for (int i = 0; i < fr->u.cond.n; i++) {
			const struct lf_frag_branch *b =
			    &fr->u.cond.branches[i];
			if (!b->expr || eval_lf_cond(cfg, b->expr)) {
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
		 const struct kc_ctx *cfg)
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
		     const char *object, const char *section)
{
	/* Rules are sorted specificity ASC; walk backwards so most-
	 * specific candidates are tried first. */
	for (int i = ctx->n_rules - 1; i >= 0; i--) {
		const struct gen_rule *r = &ctx->rules[i];
		if (!match_archive(r, archive))
			continue;
		if (!match_object(r, object))
			continue;
		if (!match_section(r, section))
			continue;
		return i;
	}
	return -1;
}

/* Append a (archive, object, section) triple to a rule's matches array.
 * All three pointers are borrowed from sinfo_db. */
static void rule_add_match(struct gen_rule *r, const char *archive,
			   const char *object, const char *section)
{
	ALLOC_GROW(r->matches, r->n_matches + 1, r->alloc_matches);
	struct gen_rule_match *m = &r->matches[r->n_matches++];
	m->archive = archive;
	m->object = object;
	m->section = section;
}

/* Append @p entry (space-separated token, e.g. "*libfoo.a" or
 * "*libfoo.a:bar.*") to @p sb if not already present.  Whole-token
 * comparison so "*libfoo.a" and "*libfoo.a:bar.*" don't collide. */
static void exclude_token_append(struct sbuf *sb, const char *entry)
{
	size_t elen = strlen(entry);
	const char *p = sb->buf;
	while (p && *p) {
		const char *space = strchr(p, ' ');
		size_t tlen = space ? (size_t)(space - p) : strlen(p);
		if (tlen == elen && !memcmp(p, entry, elen))
			return;
		p = space ? space + 1 : NULL;
	}
	if (sb->len)
		sbuf_addch(sb, ' ');
	sbuf_addstr(sb, entry);
}

/* ---- DB lookup helpers ------------------------------------------ */

static const struct sinfo_archive *db_find_archive(const struct sinfo_db *db,
						   const char *archive)
{
	for (int i = 0; i < db->n_archives; i++)
		if (!strcmp(db->archives[i].name, archive))
			return &db->archives[i];
	return NULL;
}

/* True iff the rule's (archive, object) pair refers to anything in
 * @p db.  Object-less rules need only their archive; object-bearing
 * rules need at least one DB object whose name matches the rule's
 * object pattern under the suffix-glob semantics of @ref match_object. */
static int rule_entity_in_db(const struct sinfo_db *db,
			     const struct gen_rule *r)
{
	const struct sinfo_archive *a = db_find_archive(db, r->archive);
	if (!a)
		return 0;
	if (!r->object)
		return 1;
	for (int i = 0; i < a->n_objs; i++)
		if (match_object(r, a->objs[i].name))
			return 1;
	return 0;
}

/* ---- Rule-overlap helpers --------------------------------------- */

/* Whether @p container's entity scope is a less-specific superset of
 * @p inner's: a root rule contains everything; an archive-only rule
 * contains every object of that archive; an object rule contains only
 * its own object.  Symbol scope isn't a structural container -- two
 * symbol rules on the same object don't contain each other. */
static int entity_contains_scope(const struct gen_rule *container,
				 const struct gen_rule *inner)
{
	if (!container->archive)
		return 1;
	if (!inner->archive)
		return 0;
	if (strcmp(container->archive, inner->archive) != 0)
		return 0;
	if (!container->object)
		return 1;
	if (!inner->object)
		return 0;
	return strcmp(container->object, inner->object) == 0;
}

/* Two rules' section-pattern sets overlap iff they share at least one
 * exact pattern string.  IDF .lf files name their @c [sections]
 * fragments by purpose (text, data, rodata, ...), so two rules
 * covering the same family expand to identical pattern arrays --
 * an exact-string set intersection suffices. */
static int patterns_intersect(const struct gen_rule *a,
			      const struct gen_rule *b)
{
	for (int i = 0; i < a->n_section_patterns; i++)
		for (int j = 0; j < b->n_section_patterns; j++)
			if (!strcmp(a->section_patterns[i],
				    b->section_patterns[j]))
				return 1;
	return 0;
}

/* Find the closest less-specific surviving rule whose entity scope
 * contains @p R's and whose section patterns intersect R's.  This is
 * R's "basis" -- the rule whose wildcard R narrows.  ctx->rules is
 * sorted by (specificity ASC, source_order ASC), so scanning backward
 * walks down through progressively less-specific tiers. */
static const struct gen_rule *walk_back_for_overlap(const struct gen_ctx *ctx,
						    const struct gen_rule *R)
{
	int idx = (int)(R - ctx->rules);
	for (int i = idx - 1; i >= 0; i--) {
		const struct gen_rule *Rp = &ctx->rules[i];
		if (Rp->dropped)
			continue;
		if (Rp->specificity >= R->specificity)
			continue;
		if (!entity_contains_scope(Rp, R))
			continue;
		if (!patterns_intersect(Rp, R))
			continue;
		return Rp;
	}
	return NULL;
}

/* Compute the EXCLUDE_FILE entry @p R contributes to @p basis.
 *
 * The shape depends on what kind of glob @p basis will emit:
 *
 *   - **basis is archive-scoped** (its fallback glob is
 *     @c{*<basis->archive>(...)}): always narrow to the object.  An
 *     archive-level entry would shadow the entire archive selector
 *     so the basis would emit nothing at link time.
 *
 *   - **basis is the root rule** (its catch-all is @c{*(...)}):
 *     archive-level when @p R's archive is in the DB (every section
 *     of that archive is enumerated in some rule's literal listings,
 *     so excluding the whole archive from the catch-all is exact);
 *     per-object when the archive is unknown (other objects of the
 *     same archive must keep falling through so the catch-all still
 *     routes them to the basis's target).
 *
 * Writes into @p sb (caller resets); returns sb->buf for chaining. */
static const char *rule_exclude_pattern(const struct sinfo_db *db,
					const struct gen_rule *R,
					const struct gen_rule *basis,
					struct sbuf *sb)
{
	sbuf_reset(sb);
	sbuf_addch(sb, '*');
	sbuf_addstr(sb, R->archive);
	int per_object;
	if (basis->archive)
		per_object = (R->object != NULL);
	else
		per_object = R->object && !db_find_archive(db, R->archive);
	if (per_object) {
		sbuf_addch(sb, ':');
		sbuf_addstr(sb, R->object);
		sbuf_addstr(sb, ".*");
	}
	return sb->buf;
}

/* ---- Resolution -------------------------------------------------- */

/* Walk the DB and attach every (archive, object, section) triple to
 * its most-specific matching surviving rule. */
static void attach_db_matches(struct gen_ctx *ctx, const struct sinfo_db *db)
{
	for (int ai = 0; ai < db->n_archives; ai++) {
		const struct sinfo_archive *a = &db->archives[ai];
		for (int oi = 0; oi < a->n_objs; oi++) {
			const struct sinfo_object *o = &a->objs[oi];
			for (int si = 0; si < o->n_sections; si++) {
				const char *s = o->sections[si];
				int ri = find_rule(ctx, a->name, o->name, s);
				if (ri < 0)
					continue;
				rule_add_match(&ctx->rules[ri], a->name,
					       o->name, s);
			}
		}
	}
}

/* Drop symbol-specific rules whose archive or object doesn't exist in
 * @p db.  Match-time would already skip them (no DB section can carry
 * a non-DB archive name), but we want the warning *before* the DB
 * walk so users see one diagnostic per dropped rule rather than a
 * silent disappearance plus a downstream link error. */
static void drop_symbol_rules_not_in_db(struct gen_ctx *ctx,
					const struct sinfo_db *db)
{
	for (int i = 0; i < ctx->n_rules; i++) {
		struct gen_rule *R = &ctx->rules[i];
		if (!R->symbol || R->dropped)
			continue;
		if (rule_entity_in_db(db, R))
			continue;
		warn("rule references symbol %s:%s:%s, but %s is not in the "
		     "entity DB; rule dropped",
		     R->archive, R->object, R->symbol, R->archive);
		R->dropped = 1;
	}
}

/* Drop symbol-specific rules whose section patterns matched no real
 * section.  At symbol granularity an empty match means the function
 * or data the rule named was inlined or DCE'd: the surrounded area
 * is genuinely a single section that doesn't exist, so the rule's
 * frame (SURROUND symbols, etc.) has nothing meaningful to bracket. */
static void drop_symbol_rules_with_no_matches(struct gen_ctx *ctx)
{
	for (int i = 0; i < ctx->n_rules; i++) {
		struct gen_rule *R = &ctx->rules[i];
		if (!R->symbol || R->dropped)
			continue;
		if (R->n_matches > 0)
			continue;
		warn("rule references symbol %s:%s:%s, but no matching "
		     "section was found in the object; rule dropped "
		     "(symbol may have been inlined or eliminated)",
		     R->archive, R->object, R->symbol);
		R->dropped = 1;
	}
}

/* For every surviving rule R that narrows a less-specific basis
 * routing to a *different* target, append R's file pattern to the
 * basis's per-rule @c exclude_list.  At emit time the basis's glob-
 * emitting line (root catch-all, or archive-fallback for an unknown
 * archive) carries this list as @c{EXCLUDE_FILE(...)} so cross-target
 * placement holds regardless of output-section order in the template. */
static void build_cross_target_excludes(struct gen_ctx *ctx,
					const struct sinfo_db *db)
{
	struct sbuf *staged = calloc((size_t)ctx->n_rules, sizeof(*staged));
	if (!staged)
		die_errno("calloc");

	struct sbuf entry = SBUF_INIT;
	for (int i = 0; i < ctx->n_rules; i++) {
		struct gen_rule *R = &ctx->rules[i];
		if (R->dropped)
			continue;
		if (R->specificity == 0)
			continue; /* root rules have no basis */
		const struct gen_rule *basis = walk_back_for_overlap(ctx, R);
		if (!basis)
			continue;
		if (!strcmp(basis->target, R->target))
			continue;
		rule_exclude_pattern(db, R, basis, &entry);
		int bidx = (int)(basis - ctx->rules);
		exclude_token_append(&staged[bidx], entry.buf);
	}
	sbuf_release(&entry);

	for (int i = 0; i < ctx->n_rules; i++) {
		free(ctx->rules[i].exclude_list);
		if (staged[i].len) {
			ctx->rules[i].exclude_list = staged[i].buf;
			/* Ownership transferred; do not sbuf_release(). */
		} else {
			ctx->rules[i].exclude_list = NULL;
			sbuf_release(&staged[i]);
		}
	}
	free(staged);
}

void gen_resolve(struct gen_ctx *ctx, const struct sinfo_db *db)
{
	drop_symbol_rules_not_in_db(ctx, db);
	attach_db_matches(ctx, db);
	drop_symbol_rules_with_no_matches(ctx);
	build_cross_target_excludes(ctx, db);
}

/* ---- Emission ---------------------------------------------------- */

static int has_flag(const struct gen_rule *r, enum gen_flag_kind k)
{
	for (int i = 0; i < r->n_flags; i++)
		if (r->flags[i].kind == k)
			return 1;
	return 0;
}

static const struct gen_flag *find_flag(const struct gen_rule *r,
					enum gen_flag_kind k)
{
	for (int i = 0; i < r->n_flags; i++)
		if (r->flags[i].kind == k)
			return &r->flags[i];
	return NULL;
}

static void emit_pre_wrappers(FILE *out, const char *indent,
			      const struct gen_rule *r)
{
	for (int i = 0; i < r->n_flags; i++) {
		const struct gen_flag *f = &r->flags[i];
		if (f->kind == GF_SURROUND) {
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

/* Wrap a sections-list string in SORT_BY_<first>[(SORT_BY_<second>)] per
 * Python's output_commands.py.  Returns the number of closing ')' the
 * caller must emit after each section pattern. */
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

/* Emit one input section description.
 *
 * @p file_pattern is the file-side pattern, e.g. "*libfoo.a:bar.o" or
 * "*libfoo.a" or "*".  Used for explicit literal listings; the root
 * catch-all has its own emitter (@ref emit_root_catchall) that knows
 * how to nest EXCLUDE_FILE inside SORT properly. */
static void emit_isd(FILE *out, const char *indent, const struct gen_rule *r,
		     const char *file_pattern, char *const *sections,
		     int n_sections)
{
	fputs(indent, out);
	int keep = has_flag(r, GF_KEEP);
	if (keep)
		fputs("KEEP(", out);
	fprintf(out, "%s(", file_pattern);
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

/* Emit body lines for the rule's matches, grouped by (archive, object).
 * Matches are appended in DB walk order (archive[0]:obj[0]:s0, s1, ...
 * archive[0]:obj[1]:..., ...) so they are naturally contiguous per
 * (archive, object) -- we just walk and detect group boundaries. */
static void emit_rule_matches(FILE *out, const char *indent,
			      const struct gen_rule *r)
{
	int i = 0;
	while (i < r->n_matches) {
		int j = i + 1;
		while (j < r->n_matches &&
		       r->matches[j].archive == r->matches[i].archive &&
		       r->matches[j].object == r->matches[i].object)
			j++;

		struct sbuf fp = SBUF_INIT;
		sbuf_addch(&fp, '*');
		sbuf_addstr(&fp, r->matches[i].archive);
		sbuf_addch(&fp, ':');
		sbuf_addstr(&fp, r->matches[i].object);

		int n = j - i;
		char **secs = malloc((size_t)n * sizeof(*secs));
		if (!secs)
			die_errno("malloc");
		for (int k = 0; k < n; k++)
			secs[k] = (char *)r->matches[i + k].section;
		emit_isd(out, indent, r, fp.buf, secs, n);
		free(secs);

		sbuf_release(&fp);
		i = j;
	}
}

/* For a symbol-specific rule, build the effective pattern set as it
 * would expand against the named symbol (".text.<sym>", ".text.<sym>.*",
 * etc).  Non-symbol rules return their @c section_patterns array
 * unchanged.  Returns a NULL-terminated array of borrowed/freshly-
 * allocated strings via @p heap_owns_out: if non-NULL, caller frees
 * each entry plus the array itself. */
static char **expand_symbol_patterns(const struct gen_rule *r, int *n_out,
				     int *heap_owns_out)
{
	if (!r->symbol) {
		*n_out = r->n_section_patterns;
		*heap_owns_out = 0;
		return r->section_patterns;
	}
	/* Each ".*"-bearing pattern expands to two entries: ".<sym>"
	 * (exact) and ".<sym>.*" (glob).  Patterns without ".*" produce
	 * no symbol-level placement and are skipped. */
	int cap = 2 * r->n_section_patterns;
	char **out = calloc((size_t)cap, sizeof(*out));
	if (!out)
		die_errno("calloc");
	int n = 0;
	struct sbuf sb = SBUF_INIT;
	for (int i = 0; i < r->n_section_patterns; i++) {
		const char *pat = r->section_patterns[i];
		const char *star = strstr(pat, ".*");
		if (!star)
			continue;
		size_t prefix = (size_t)(star - pat);
		const char *after = star + 2;

		sbuf_reset(&sb);
		sbuf_add(&sb, pat, prefix);
		sbuf_addch(&sb, '.');
		sbuf_addstr(&sb, r->symbol);
		sbuf_addstr(&sb, after);
		out[n++] = sbuf_strdup(sb.buf);

		sbuf_reset(&sb);
		sbuf_add(&sb, pat, prefix);
		sbuf_addch(&sb, '.');
		sbuf_addstr(&sb, r->symbol);
		sbuf_addstr(&sb, ".*");
		sbuf_addstr(&sb, after);
		out[n++] = sbuf_strdup(sb.buf);
	}
	sbuf_release(&sb);
	*n_out = n;
	*heap_owns_out = 1;
	return out;
}

/* Emit a glob-style line ("*(...)" or "*libfoo.a(...)" /
 * "*libfoo.a:bar.*(...)") for a rule with zero DB matches or for a
 * root-rule catch-all.  Per GNU ld grammar, EXCLUDE_FILE can appear
 * either as a wildcard_spec by itself ("EXCLUDE_FILE(...) <pattern>")
 * or wrapped inside SORT_BY_NAME ("SORT_BY_NAME(EXCLUDE_FILE(...)
 * <pattern>)") -- but NOT the other way around.  We always emit
 * EXCLUDE_FILE per pattern (rather than relying on the propagating
 * form documented in the manual but inconsistently implemented by
 * binutils versions) so the same code path handles SORT and non-SORT
 * cases. */
static void emit_glob_line(FILE *out, const char *indent,
			   const struct gen_rule *r, const char *file_pattern)
{
	int n_pats, owns;
	char **pats = expand_symbol_patterns(r, &n_pats, &owns);
	if (n_pats == 0) {
		if (owns)
			free(pats);
		return;
	}
	fputs(indent, out);
	int keep = has_flag(r, GF_KEEP);
	if (keep)
		fputs("KEEP(", out);
	fprintf(out, "%s(", file_pattern);
	const struct gen_flag *sort = find_flag(r, GF_SORT);
	for (int i = 0; i < n_pats; i++) {
		if (i)
			fputc(' ', out);
		int close = emit_sort_open(out, sort);
		if (r->exclude_list && *r->exclude_list)
			fprintf(out, "EXCLUDE_FILE(%s) ", r->exclude_list);
		fputs(pats[i], out);
		while (close-- > 0)
			fputc(')', out);
	}
	fputc(')', out);
	if (keep)
		fputc(')', out);
	fputc('\n', out);
	if (owns) {
		for (int i = 0; i < n_pats; i++)
			free(pats[i]);
		free(pats);
	}
}

/* Fallback for archive-specific rules with zero DB matches: emit the
 * rule's own selector with its (symbol-expanded if applicable) section
 * patterns.  Without this, a rule like `archive: libwifi_phy.a
 * (in_iram)` for an archive ldgen does not see would silently
 * disappear.  Carries the rule's @c exclude_list when more-specific
 * narrowing rules route to a different target. */
static void emit_rule_archive_fallback(FILE *out, const char *indent,
				       const struct gen_rule *r)
{
	struct sbuf fp = SBUF_INIT;
	sbuf_addch(&fp, '*');
	sbuf_addstr(&fp, r->archive);
	if (r->object) {
		sbuf_addch(&fp, ':');
		sbuf_addstr(&fp, r->object);
		/* The linker accepts a plain glob in the file pattern, so
		 * `obj.*.o` covers both legacy `bar.o` and IDF's `bar.c.obj`
		 * forms in one selector. */
		sbuf_addstr(&fp, ".*");
	}
	emit_glob_line(out, indent, r, fp.buf);
	sbuf_release(&fp);
}

/* Emit the root rule's catch-all line.  Sections from archives the
 * libraries-file doesn't enumerate land here.  The rule's
 * @c exclude_list (built by @ref build_cross_target_excludes) lists
 * file patterns of more-specific rules routing to other targets, so
 * the catch-all doesn't pull their content into the wrong target. */
static void emit_root_catchall(FILE *out, const char *indent,
			       const struct gen_rule *r)
{
	emit_glob_line(out, indent, r, "*");
}

static int rule_is_root(const struct gen_rule *r)
{
	return !r->archive && !r->object && !r->symbol;
}

/* Emit one rule's contribution to its target's body. */
static void emit_rule(FILE *out, const char *indent, const struct gen_rule *r)
{
	if (r->dropped)
		return;

	emit_pre_wrappers(out, indent, r);

	if (r->n_matches > 0)
		emit_rule_matches(out, indent, r);
	else if (r->archive)
		emit_rule_archive_fallback(out, indent, r);

	if (rule_is_root(r))
		emit_root_catchall(out, indent, r);

	emit_post_wrappers(out, indent, r);
}

/* Iterate ctx->rules in source order, return the index of the first
 * rule with the given target after @p start, or ctx->n_rules if none. */
static int find_next_rule_for_target(const struct gen_ctx *ctx, int start,
				     const char *target)
{
	for (int i = start; i < ctx->n_rules; i++) {
		const struct gen_rule *r = &ctx->rules[i];
		if (!strcmp(r->target, target))
			return i;
	}
	return ctx->n_rules;
}

/* Emit all rules with the given target, in source-order (which is the
 * stable order from rule_cmp's secondary key).  ctx->rules is sorted
 * primarily by specificity, but rules with the same target may appear
 * at any position -- so we scan and select. */
static void emit_target_rules(FILE *out, const char *indent,
			      const struct gen_ctx *ctx, const char *target)
{
	/* ctx->rules is currently sorted by (specificity ASC, source_order
	 * ASC) for resolution.  Re-collect rules for this target in
	 * source-order so multiple SURROUND wrappers within the same
	 * target appear in the order the user wrote them. */
	int *idx = malloc((size_t)ctx->n_rules * sizeof(*idx));
	if (!idx)
		die_errno("malloc");
	int n = 0;
	for (int i = 0; i < ctx->n_rules; i++)
		if (!strcmp(ctx->rules[i].target, target))
			idx[n++] = i;
	/* Sort by source_order (stable -- already partially sorted). */
	for (int i = 1; i < n; i++) {
		int v = idx[i];
		int j = i - 1;
		while (j >= 0 && ctx->rules[idx[j]].source_order >
				     ctx->rules[v].source_order) {
			idx[j + 1] = idx[j];
			j--;
		}
		idx[j + 1] = v;
	}

	for (int i = 0; i < n; i++)
		emit_rule(out, indent, &ctx->rules[idx[i]]);

	free(idx);
}

void gen_emit_target(FILE *out, const char *indent, const struct gen_ctx *ctx,
		     const char *target)
{
	emit_target_rules(out, indent, ctx, target);
}

void gen_emit(FILE *out, const struct gen_ctx *ctx)
{
	/* Walk rules in source order, emitting each unique target's block
	 * the first time we encounter it.  Duplicate-target detection is
	 * O(n_targets^2) but n_targets is small (~10). */
	for (int i = 0; i < ctx->n_rules; i++) {
		const char *target = ctx->rules[i].target;
		int j = find_next_rule_for_target(ctx, 0, target);
		if (j != i)
			continue; /* not the first rule for this target */
		fprintf(out, "[%s]\n", target);
		emit_target_rules(out, "    ", ctx, target);
		fputc('\n', out);
	}
}

/* ---- Template fill ---------------------------------------------- */

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

/* ---- Canonical dump --------------------------------------------- */

struct canon_row {
	const char *archive;
	const char *object;
	const char *section;
	const char *target;
};

static int canon_cmp(const void *a, const void *b)
{
	const struct canon_row *ra = a;
	const struct canon_row *rb = b;
	int c;
	if ((c = strcmp(ra->archive, rb->archive)) != 0)
		return c;
	if ((c = strcmp(ra->object, rb->object)) != 0)
		return c;
	return strcmp(ra->section, rb->section);
}

void gen_emit_canonical(FILE *out, const struct gen_ctx *ctx)
{
	int total = 0;
	for (int i = 0; i < ctx->n_rules; i++)
		total += ctx->rules[i].n_matches;
	if (!total)
		return;

	struct canon_row *rows = malloc((size_t)total * sizeof(*rows));
	if (!rows)
		die_errno("malloc");
	int k = 0;
	for (int i = 0; i < ctx->n_rules; i++) {
		const struct gen_rule *r = &ctx->rules[i];
		for (int j = 0; j < r->n_matches; j++) {
			rows[k].archive = r->matches[j].archive;
			rows[k].object = r->matches[j].object;
			rows[k].section = r->matches[j].section;
			rows[k].target = r->target;
			k++;
		}
	}
	qsort(rows, (size_t)total, sizeof(*rows), canon_cmp);
	for (int i = 0; i < total; i++)
		fprintf(out, "%s|%s|%s|%s\n", rows[i].archive, rows[i].object,
			rows[i].section, rows[i].target);
	free(rows);
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
	free(r->matches);
	free(r->exclude_list);
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

	memset(ctx, 0, sizeof(*ctx));
}
