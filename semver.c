/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal SemVer 2.0.0 parser and constraint matcher -- see semver.h
 * for the public contract.
 *
 * Internally, a constraint is stored in disjunctive normal form: a
 * list of clauses, each a conjunction of single-operator predicates.
 * "1.0 || >=2.0,<3.0" is two clauses; "^1.2" expands to a single
 * clause with the two predicates ">=1.2.0" and "<2.0.0".  Matching is
 * linear: any clause whose predicates all satisfy the candidate
 * wins.  No range-algebra is performed; pubgrub operates on concrete
 * per-package version lists and filters via
 * semver_constraint_matches(), sidestepping the Range/Union machinery
 * a general SemVer library would need.
 *
 * Pre-release handling is the conservative PRERELEASE_NATURAL policy
 * from python-semanticversion: a candidate with a prerelease
 * identifier only matches a predicate whose version also specifies a
 * prerelease and shares the same M.m.p.rev base.  Without a
 * prerelease in the predicate, prerelease candidates never match.
 * That is stricter than mixology's tri-policy model but covers every
 * case the canonical PubGrub test corpus exercises; widen if a real
 * project demands.
 */

#include "semver.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "error.h"
#include "sbuf.h"

/* ==================================================================== */
/* Small lexical helpers                                                 */
/* ==================================================================== */

static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alpha(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_ident(int c) { return is_digit(c) || is_alpha(c) || c == '-'; }

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/*
 * Parse a non-negative decimal integer with no leading zeros (except
 * a bare "0").  Advances *p past the digits on success.  Returns 0 on
 * success, -1 on error.
 */
static int parse_uint(const char **p, unsigned *out)
{
	const char *s = *p;
	unsigned v = 0;

	if (!is_digit(*s))
		return -1;

	if (*s == '0') {
		if (is_digit(s[1]))
			return -1;
		*out = 0;
		*p = s + 1;
		return 0;
	}

	while (is_digit(*s)) {
		unsigned digit = (unsigned)(*s - '0');
		if (v > (UINT_MAX - digit) / 10)
			return -1;
		v = v * 10 + digit;
		s++;
	}

	*out = v;
	*p = s;
	return 0;
}

/*
 * Parse a '.'-separated list of SemVer identifiers into a heap array.
 * Each identifier is [0-9A-Za-z-]+; numeric-only identifiers must
 * not have leading zeros.  On success, *out receives the array and
 * *nr_out its length.  On failure, any partial allocations are freed
 * and -1 is returned.
 */
static int parse_ident_list(const char **p, char ***out, size_t *nr_out)
{
	char **list = NULL;
	size_t nr = 0, alloc = 0;
	const char *s = *p;

	for (;;) {
		const char *start = s;
		int all_digits = 1;

		if (!is_ident(*s))
			goto fail;

		while (is_ident(*s)) {
			if (!is_digit(*s))
				all_digits = 0;
			s++;
		}

		if (s == start)
			goto fail;

		if (all_digits && (s - start) > 1 && *start == '0')
			goto fail;

		ALLOC_GROW(list, nr + 1, alloc);
		list[nr++] = sbuf_strndup(start, (size_t)(s - start));

		if (*s != '.')
			break;
		s++;
	}

	*out = list;
	*nr_out = nr;
	*p = s;
	return 0;

fail:
	for (size_t i = 0; i < nr; i++)
		free(list[i]);
	free(list);
	return -1;
}

/* ==================================================================== */
/* Version                                                               */
/* ==================================================================== */

void semver_release(struct semver_version *v)
{
	if (!v)
		return;

	for (size_t i = 0; i < v->prerelease_nr; i++)
		free(v->prerelease[i]);
	free(v->prerelease);

	for (size_t i = 0; i < v->build_nr; i++)
		free(v->build[i]);
	free(v->build);

	v->prerelease = v->build = NULL;
	v->prerelease_nr = v->build_nr = 0;
	v->major = v->minor = v->patch = v->revision = 0;
}

int semver_parse(struct semver_version *out, const char *s)
{
	struct semver_version tmp = SEMVER_VERSION_INIT;
	const char *p = s;

	if (!s)
		return -1;

	if (parse_uint(&p, &tmp.major) < 0)
		goto fail;
	if (*p++ != '.')
		goto fail;
	if (parse_uint(&p, &tmp.minor) < 0)
		goto fail;
	if (*p++ != '.')
		goto fail;
	if (parse_uint(&p, &tmp.patch) < 0)
		goto fail;

	if (*p == '~') {
		p++;
		if (parse_uint(&p, &tmp.revision) < 0)
			goto fail;
	}

	if (*p == '-') {
		p++;
		if (parse_ident_list(&p, &tmp.prerelease, &tmp.prerelease_nr) <
		    0)
			goto fail;
	}

	if (*p == '+') {
		p++;
		if (parse_ident_list(&p, &tmp.build, &tmp.build_nr) < 0)
			goto fail;
	}

	if (*p != '\0')
		goto fail;

	*out = tmp;
	return 0;

fail:
	semver_release(&tmp);
	return -1;
}

/*
 * Compare two prerelease identifiers per SemVer 2.0.0 rules:
 *   - All-digit identifiers compare numerically
 *   - Otherwise, compare lexicographically as ASCII bytes
 *   - A numeric identifier sorts below an alphanumeric one
 */
static int ident_cmp(const char *a, const char *b)
{
	int a_num = 1, b_num = 1;

	for (const char *p = a; *p; p++)
		if (!is_digit((unsigned char)*p)) {
			a_num = 0;
			break;
		}
	for (const char *p = b; *p; p++)
		if (!is_digit((unsigned char)*p)) {
			b_num = 0;
			break;
		}

	if (a_num && b_num) {
		size_t la = strlen(a), lb = strlen(b);
		if (la != lb)
			return la < lb ? -1 : 1;
		return strcmp(a, b);
	}
	if (a_num)
		return -1;
	if (b_num)
		return 1;
	return strcmp(a, b);
}

int semver_cmp(const struct semver_version *a, const struct semver_version *b)
{
	if (a->major != b->major)
		return a->major < b->major ? -1 : 1;
	if (a->minor != b->minor)
		return a->minor < b->minor ? -1 : 1;
	if (a->patch != b->patch)
		return a->patch < b->patch ? -1 : 1;
	if (a->revision != b->revision)
		return a->revision < b->revision ? -1 : 1;

	/* Absent prerelease sorts ABOVE any prerelease (per SemVer 2.0.0). */
	if (a->prerelease_nr == 0 && b->prerelease_nr == 0)
		return 0;
	if (a->prerelease_nr == 0)
		return 1;
	if (b->prerelease_nr == 0)
		return -1;

	{
		size_t n = a->prerelease_nr < b->prerelease_nr
			       ? a->prerelease_nr
			       : b->prerelease_nr;
		for (size_t i = 0; i < n; i++) {
			int c = ident_cmp(a->prerelease[i], b->prerelease[i]);
			if (c)
				return c;
		}
	}

	/* All shared identifiers equal: the shorter list sorts lower. */
	if (a->prerelease_nr != b->prerelease_nr)
		return a->prerelease_nr < b->prerelease_nr ? -1 : 1;
	return 0;
}

void semver_format(struct sbuf *out, const struct semver_version *v)
{
	sbuf_addf(out, "%u.%u.%u", v->major, v->minor, v->patch);
	if (v->revision)
		sbuf_addf(out, "~%u", v->revision);
	if (v->prerelease_nr) {
		sbuf_addch(out, '-');
		for (size_t i = 0; i < v->prerelease_nr; i++) {
			if (i)
				sbuf_addch(out, '.');
			sbuf_addstr(out, v->prerelease[i]);
		}
	}
	if (v->build_nr) {
		sbuf_addch(out, '+');
		for (size_t i = 0; i < v->build_nr; i++) {
			if (i)
				sbuf_addch(out, '.');
			sbuf_addstr(out, v->build[i]);
		}
	}
}

/* ==================================================================== */
/* Constraint                                                            */
/* ==================================================================== */

enum semver_op {
	SEMVER_OP_ANY, /* '*' */
	SEMVER_OP_EQ,  /* == v */
	SEMVER_OP_NEQ, /* != v */
	SEMVER_OP_LT,
	SEMVER_OP_LTE,
	SEMVER_OP_GT,
	SEMVER_OP_GTE,
};

struct semver_predicate {
	enum semver_op op;
	struct semver_version v;
	int has_pre; /* 1 if v specifies a prerelease identifier */
};

struct semver_clause {
	struct semver_predicate *preds;
	size_t nr, alloc;
};

struct semver_constraint {
	struct semver_clause *clauses; /* clauses[i] OR clauses[j] */
	size_t nr, alloc;
};

static struct semver_predicate *alloc_pred(struct semver_clause *cl)
{
	struct semver_predicate *p;
	ALLOC_GROW(cl->preds, cl->nr + 1, cl->alloc);
	p = &cl->preds[cl->nr++];
	memset(p, 0, sizeof(*p));
	return p;
}

static struct semver_clause *alloc_clause(struct semver_constraint *c)
{
	struct semver_clause *cl;
	ALLOC_GROW(c->clauses, c->nr + 1, c->alloc);
	cl = &c->clauses[c->nr++];
	memset(cl, 0, sizeof(*cl));
	return cl;
}

static struct semver_version make_v3(unsigned M, unsigned m, unsigned p)
{
	struct semver_version v = SEMVER_VERSION_INIT;
	v.major = M;
	v.minor = m;
	v.patch = p;
	return v;
}

/*
 * Parse a single predicate -- possibly expanding a ^/~/partial form
 * into multiple simple predicates appended to the current clause.
 * Returns 0 on success and advances *pp; returns -1 on parse error.
 */
static int parse_predicate(const char **pp, struct semver_constraint *c)
{
	const char *p = skip_ws(*pp);
	struct semver_clause *cl = &c->clauses[c->nr - 1];
	enum semver_op op = SEMVER_OP_EQ;
	int sigil = 0; /* 0 literal | 1 caret | 2 tilde */
	unsigned M = 0, m = 0, pa = 0, rv = 0;
	int specified = 0;
	char **pre = NULL;
	size_t pre_nr = 0;
	char **build = NULL;
	size_t build_nr = 0;

	if (*p == '*') {
		struct semver_predicate *pr = alloc_pred(cl);
		pr->op = SEMVER_OP_ANY;
		*pp = p + 1;
		return 0;
	}

	if (p[0] == '>' && p[1] == '=') {
		op = SEMVER_OP_GTE;
		p += 2;
	} else if (p[0] == '<' && p[1] == '=') {
		op = SEMVER_OP_LTE;
		p += 2;
	} else if (p[0] == '=' && p[1] == '=') {
		op = SEMVER_OP_EQ;
		p += 2;
	} else if (p[0] == '!' && p[1] == '=') {
		op = SEMVER_OP_NEQ;
		p += 2;
	} else if (p[0] == '>') {
		op = SEMVER_OP_GT;
		p++;
	} else if (p[0] == '<') {
		op = SEMVER_OP_LT;
		p++;
	} else if (p[0] == '=') {
		op = SEMVER_OP_EQ;
		p++;
	} else if (p[0] == '^') {
		sigil = 1;
		p++;
	} else if (p[0] == '~' && p[1] == '=') {
		sigil = 2;
		p += 2;
	} else if (p[0] == '~') {
		sigil = 2;
		p++;
	}

	p = skip_ws(p);

	if (!is_digit(*p))
		return -1;

	if (parse_uint(&p, &M) < 0)
		return -1;
	specified = 1;
	if (*p == '.') {
		p++;
		if (parse_uint(&p, &m) < 0)
			return -1;
		specified = 2;
		if (*p == '.') {
			p++;
			if (parse_uint(&p, &pa) < 0)
				return -1;
			specified = 3;
		}
	}

	if (specified == 3 && *p == '~') {
		p++;
		if (parse_uint(&p, &rv) < 0)
			return -1;
	}

	if (specified == 3 && *p == '-') {
		p++;
		if (parse_ident_list(&p, &pre, &pre_nr) < 0)
			return -1;
	}

	if (specified == 3 && *p == '+') {
		p++;
		if (parse_ident_list(&p, &build, &build_nr) < 0) {
			for (size_t i = 0; i < pre_nr; i++)
				free(pre[i]);
			free(pre);
			return -1;
		}
	}

	if (sigil == 1) {
		/* Caret: >=v, <bump-of-most-significant-nonzero. */
		struct semver_version lo =
		    make_v3(M, specified >= 2 ? m : 0, specified >= 3 ? pa : 0);
		struct semver_version hi;
		struct semver_predicate *pr;

		lo.revision = rv;
		if (pre_nr) {
			lo.prerelease = pre;
			lo.prerelease_nr = pre_nr;
		}

		if (M > 0)
			hi = make_v3(M + 1, 0, 0);
		else if (m > 0)
			hi = make_v3(0, m + 1, 0);
		else
			hi = make_v3(0, 0, pa + 1);

		pr = alloc_pred(cl);
		pr->op = SEMVER_OP_GTE;
		pr->v = lo;
		pr->has_pre = pre_nr > 0;
		pr = alloc_pred(cl);
		pr->op = SEMVER_OP_LT;
		pr->v = hi;
		pr->has_pre = 0;

		if (build_nr) {
			for (size_t i = 0; i < build_nr; i++)
				free(build[i]);
			free(build);
		}
	} else if (sigil == 2) {
		/* Tilde (python-semanticversion variant):
		 *   ~M.m.p -> >=M.m.p, <M.(m+1).0
		 *   ~M.m   -> >=M.m.0, <(M+1).0.0
		 *   ~M     -> >=M.0.0, <(M+1).0.0
		 */
		struct semver_version lo =
		    make_v3(M, specified >= 2 ? m : 0, specified >= 3 ? pa : 0);
		struct semver_version hi;
		struct semver_predicate *pr;

		lo.revision = rv;
		if (pre_nr) {
			lo.prerelease = pre;
			lo.prerelease_nr = pre_nr;
		}

		if (specified >= 3)
			hi = make_v3(M, m + 1, 0);
		else
			hi = make_v3(M + 1, 0, 0);

		pr = alloc_pred(cl);
		pr->op = SEMVER_OP_GTE;
		pr->v = lo;
		pr->has_pre = pre_nr > 0;
		pr = alloc_pred(cl);
		pr->op = SEMVER_OP_LT;
		pr->v = hi;
		pr->has_pre = 0;

		if (build_nr) {
			for (size_t i = 0; i < build_nr; i++)
				free(build[i]);
			free(build);
		}
	} else if (op == SEMVER_OP_EQ && specified < 3) {
		/* Wildcard-expand a partial bare version:
		 *   "1"     -> >=1.0.0, <2.0.0
		 *   "1.2"   -> >=1.2.0, <1.3.0
		 */
		struct semver_version lo =
		    make_v3(M, specified >= 2 ? m : 0, 0);
		struct semver_version hi = (specified == 2)
					       ? make_v3(M, m + 1, 0)
					       : make_v3(M + 1, 0, 0);
		struct semver_predicate *pr;

		pr = alloc_pred(cl);
		pr->op = SEMVER_OP_GTE;
		pr->v = lo;
		pr->has_pre = 0;
		pr = alloc_pred(cl);
		pr->op = SEMVER_OP_LT;
		pr->v = hi;
		pr->has_pre = 0;

		/* Partial bare versions don't carry pre/build; discard. */
		for (size_t i = 0; i < pre_nr; i++)
			free(pre[i]);
		free(pre);
		for (size_t i = 0; i < build_nr; i++)
			free(build[i]);
		free(build);
	} else {
		struct semver_predicate *pr = alloc_pred(cl);
		pr->op = op;
		pr->v =
		    make_v3(M, specified >= 2 ? m : 0, specified >= 3 ? pa : 0);
		pr->v.revision = rv;
		pr->has_pre = pre_nr > 0;
		if (pre_nr) {
			pr->v.prerelease = pre;
			pr->v.prerelease_nr = pre_nr;
		}
		if (build_nr) {
			pr->v.build = build;
			pr->v.build_nr = build_nr;
		}
	}

	*pp = p;
	return 0;
}

static void release_clause(struct semver_clause *cl)
{
	for (size_t i = 0; i < cl->nr; i++)
		semver_release(&cl->preds[i].v);
	free(cl->preds);
}

void semver_constraint_free(struct semver_constraint *c)
{
	if (!c)
		return;
	for (size_t i = 0; i < c->nr; i++)
		release_clause(&c->clauses[i]);
	free(c->clauses);
	free(c);
}

struct semver_constraint *semver_constraint_parse(const char *s)
{
	struct semver_constraint *c;
	const char *p;

	if (!s)
		return NULL;

	c = calloc(1, sizeof(*c));
	if (!c)
		die_errno("calloc");

	alloc_clause(c);

	p = skip_ws(s);
	while (*p) {
		if (p[0] == '|' && p[1] == '|') {
			p = skip_ws(p + 2);
			alloc_clause(c);
			continue;
		}
		if (*p == ',') {
			p = skip_ws(p + 1);
			continue;
		}
		if (parse_predicate(&p, c) < 0)
			goto fail;
		p = skip_ws(p);
	}

	for (size_t i = 0; i < c->nr; i++)
		if (c->clauses[i].nr == 0)
			goto fail;

	return c;

fail:
	semver_constraint_free(c);
	return NULL;
}

/*
 * Does @p v match a single predicate @p pr?
 *
 * Under the natural prerelease policy a candidate with a prerelease
 * identifier only matches if the predicate's version also carries one
 * AND the M.m.p.rev bases are identical.  Otherwise, ordered compare.
 */
static int predicate_matches(const struct semver_predicate *pr,
			     const struct semver_version *v)
{
	int cmp;

	if (pr->op == SEMVER_OP_ANY)
		return v->prerelease_nr == 0;

	if (v->prerelease_nr > 0) {
		if (!pr->has_pre)
			return 0;
		if (v->major != pr->v.major || v->minor != pr->v.minor ||
		    v->patch != pr->v.patch || v->revision != pr->v.revision)
			return 0;
	}

	cmp = semver_cmp(v, &pr->v);
	switch (pr->op) {
	case SEMVER_OP_EQ:
		return cmp == 0;
	case SEMVER_OP_NEQ:
		return cmp != 0;
	case SEMVER_OP_LT:
		return cmp < 0;
	case SEMVER_OP_LTE:
		return cmp <= 0;
	case SEMVER_OP_GT:
		return cmp > 0;
	case SEMVER_OP_GTE:
		return cmp >= 0;
	default:
		return 0;
	}
}

int semver_constraint_matches(const struct semver_constraint *c,
			      const struct semver_version *v)
{
	for (size_t i = 0; i < c->nr; i++) {
		const struct semver_clause *cl = &c->clauses[i];
		int ok = 1;
		for (size_t j = 0; j < cl->nr; j++) {
			if (!predicate_matches(&cl->preds[j], v)) {
				ok = 0;
				break;
			}
		}
		if (ok)
			return 1;
	}
	return 0;
}

static const char *op_symbol(enum semver_op op)
{
	switch (op) {
	case SEMVER_OP_ANY:
		return "*";
	case SEMVER_OP_EQ:
		return "==";
	case SEMVER_OP_NEQ:
		return "!=";
	case SEMVER_OP_LT:
		return "<";
	case SEMVER_OP_LTE:
		return "<=";
	case SEMVER_OP_GT:
		return ">";
	case SEMVER_OP_GTE:
		return ">=";
	}
	return "?";
}

void semver_constraint_format(struct sbuf *out,
			      const struct semver_constraint *c)
{
	for (size_t i = 0; i < c->nr; i++) {
		const struct semver_clause *cl = &c->clauses[i];
		if (i)
			sbuf_addstr(out, " || ");
		for (size_t j = 0; j < cl->nr; j++) {
			const struct semver_predicate *pr = &cl->preds[j];
			if (j)
				sbuf_addch(out, ',');
			if (pr->op == SEMVER_OP_ANY) {
				sbuf_addstr(out, "*");
			} else {
				sbuf_addstr(out, op_symbol(pr->op));
				semver_format(out, &pr->v);
			}
		}
	}
}
