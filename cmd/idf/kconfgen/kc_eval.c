/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_eval.c
 * @brief Kconfig symbol evaluation implementation.
 *
 * Six-pass pipeline:
 *
 *   1. Menu ctx_dep propagation (top-down walk).
 *   2. Symbol effective_dep (context AND every KP_DEPENDS).
 *   3. rev_dep / weak_rev_dep from @c select / @c imply.
 *   4. Seed cur_val from user input (--config/--defaults) where set.
 *   5. Fixpoint: visibility + default application + select enforcement.
 *   6. Range clamping (int/hex/float).
 *
 * Scope of this first cut: full boolean semantics (tristate collapsed to
 * bool since ESP-IDF has no mod), int/hex/string/float stored as
 * strings, default application, select enforcement, imply weak floor,
 * range clamping.  Choice-group mutual-exclusion is deferred to the
 * next iteration -- choice member symbols are evaluated like plain
 * bools for now.
 */
#include "kc_eval.h"
#include "ice.h"
#include "kc_ast.h"

#define MAX_FIXPOINT_ITERS 50

/* ================================================================== */
/*  Expression utilities                                              */
/* ================================================================== */

static struct kexpr *expr_alloc(enum kexpr_op op)
{
	struct kexpr *e = calloc(1, sizeof(*e));
	if (!e)
		die_errno("calloc");
	e->op = op;
	return e;
}

static struct kexpr *expr_clone(const struct kexpr *e)
{
	if (!e)
		return NULL;
	struct kexpr *n = expr_alloc(e->op);
	n->sym = e->sym;
	if (e->str)
		n->str = sbuf_strdup(e->str);
	n->l = expr_clone(e->l);
	n->r = expr_clone(e->r);
	return n;
}

/* dst := dst AND src (taking ownership of both; @p src is consumed). */
static struct kexpr *expr_and_take(struct kexpr *dst, struct kexpr *src)
{
	if (!dst)
		return src;
	if (!src)
		return dst;
	struct kexpr *n = expr_alloc(KE_AND);
	n->l = dst;
	n->r = src;
	return n;
}

/* dst := dst OR src (taking ownership of both). */
static struct kexpr *expr_or_take(struct kexpr *dst, struct kexpr *src)
{
	if (!dst)
		return src;
	if (!src)
		return dst;
	struct kexpr *n = expr_alloc(KE_OR);
	n->l = dst;
	n->r = src;
	return n;
}

/* ================================================================== */
/*  Value helpers                                                     */
/* ================================================================== */

/* Return 1 if the symbol name denotes the constant "y", 0 for "n" /
 * "m", -1 for anything else. */
static int name_bool_const(const char *name)
{
	if (!name)
		return -1;
	if (!strcmp(name, "y") || !strcmp(name, "yes"))
		return 1;
	if (!strcmp(name, "n") || !strcmp(name, "no"))
		return 0;
	if (!strcmp(name, "m"))
		return 0; /* ESP has no modules; collapse to n */
	return -1;
}

/* Return the string value for a symbol (for use in comparisons /
 * string extraction).  Never returns NULL. */
static const char *sym_str(const struct ksym *s)
{
	if (!s)
		return "";
	int c = name_bool_const(s->name);
	if (c == 1)
		return "y";
	if (c == 0)
		return "n";
	/* A bareword that looks like a number: its "value" is its name
	 * (the lexer stored numeric literals as KE_SYMREF with the digit
	 * string as the name).  Prefer that over an empty cur_val. */
	if (s->type == KS_UNKNOWN && s->name[0] &&
	    (s->name[0] == '-' || s->name[0] == '+' ||
	     (s->name[0] >= '0' && s->name[0] <= '9')))
		return s->name;
	return s->cur_val ? s->cur_val : "";
}

/* Return the string value of a leaf expression (KE_LITERAL or KE_SYMREF). */
static const char *expr_str(const struct kexpr *e)
{
	if (!e)
		return "";
	if (e->op == KE_LITERAL)
		return e->str ? e->str : "";
	if (e->op == KE_SYMREF)
		return sym_str(e->sym);
	return "";
}

/* Evaluate a string as a 0/1 boolean: "y" -> 1, "n"/"" -> 0, numbers
 * -> nonzero, other strings -> 0 (matches python kconfgen's relaxed
 * truthiness). */
static int str_is_true(const char *s)
{
	if (!s || !*s)
		return 0;
	if (!strcmp(s, "y") || !strcmp(s, "yes"))
		return 1;
	if (!strcmp(s, "n") || !strcmp(s, "no") || !strcmp(s, "m"))
		return 0;
	/* Numeric? */
	char *end;
	long long v = strtoll(s, &end, 0);
	if (end != s && !*end)
		return v != 0;
	return 0;
}

/* Evaluate a comparison of two leaf values.  Uses numeric compare when
 * both sides parse as integers, else string compare. */
static int cmp_eval(enum kexpr_op op, const char *a, const char *b)
{
	char *ea, *eb;
	long long va = strtoll(a, &ea, 0);
	long long vb = strtoll(b, &eb, 0);
	int numeric = (ea != a && !*ea && eb != b && !*eb);

	int c;
	if (numeric)
		c = (va < vb) ? -1 : (va > vb) ? 1 : 0;
	else
		c = strcmp(a, b);

	switch (op) {
	case KE_EQ:
		return c == 0;
	case KE_NE:
		return c != 0;
	case KE_LT:
		return c < 0;
	case KE_LE:
		return c <= 0;
	case KE_GT:
		return c > 0;
	case KE_GE:
		return c >= 0;
	default:
		return 0;
	}
}

/* Boolean evaluation of an expression.  NULL cond means "always". */
static int eval_bool(const struct kexpr *e)
{
	if (!e)
		return 1;
	switch (e->op) {
	case KE_LITERAL:
		return str_is_true(e->str);
	case KE_SYMREF: {
		int c = name_bool_const(e->sym ? e->sym->name : NULL);
		if (c >= 0)
			return c;
		if (e->sym && e->sym->type == KS_BOOL)
			return e->sym->cur_val && !strcmp(e->sym->cur_val, "y");
		return str_is_true(sym_str(e->sym));
	}
	case KE_NOT:
		return !eval_bool(e->l);
	case KE_AND:
		return eval_bool(e->l) && eval_bool(e->r);
	case KE_OR:
		return eval_bool(e->l) || eval_bool(e->r);
	case KE_EQ:
	case KE_NE:
	case KE_LT:
	case KE_LE:
	case KE_GT:
	case KE_GE:
		return cmp_eval(e->op, expr_str(e->l), expr_str(e->r));
	}
	return 0;
}

/* ================================================================== */
/*  Pass 1: menu context deps                                         */
/* ================================================================== */

static void pass_ctx_dep(struct kmenu *m, const struct kexpr *parent_ctx)
{
	/* Own ctx_dep = parent ctx AND menu's own dep AND visible_if. */
	struct kexpr *own = NULL;
	if (parent_ctx)
		own = expr_clone(parent_ctx);
	if (m->dep)
		own = expr_and_take(own, expr_clone(m->dep));
	if (m->visible_if)
		own = expr_and_take(own, expr_clone(m->visible_if));
	m->ctx_dep = own;

	for (struct kmenu *c = m->children; c; c = c->next)
		pass_ctx_dep(c, m->ctx_dep);
}

/* ================================================================== */
/*  Pass 2: symbol effective_dep                                      */
/* ================================================================== */

static void pass_sym_dep(struct kc_ctx *ctx)
{
	/* Pass 2a: seed each symbol's effective_dep from the FIRST menu
	 * node that references it (walk via symlist → symtab). */
	for (size_t i = 0; i < ctx->symlist.nr; i++) {
		struct ksym *s = smap_get(&ctx->symtab, ctx->symlist.v[i]);
		(void)s; /* menu-walk below seeds this. */
	}

	/* Pass 2b: walk menu tree; for each menu that owns a symbol,
	 * seed effective_dep from the FIRST encountered ctx_dep, then
	 * skip on subsequent encounters. */
	struct kmenu *stack[256];
	int sp = 0;
	stack[sp++] = ctx->root;
	while (sp) {
		struct kmenu *m = stack[--sp];
		if (m->sym && !m->sym->effective_dep && m->ctx_dep &&
		    (m->kind == KM_SYM || m->kind == KM_CHOICE))
			m->sym->effective_dep = expr_clone(m->ctx_dep);
		for (struct kmenu *c = m->children; c; c = c->next)
			if (sp < 256)
				stack[sp++] = c;
	}

	/* Pass 2c: AND in each symbol's own KP_DEPENDS properties
	 * exactly once, iterating symbol-by-symbol. */
	for (size_t i = 0; i < ctx->symlist.nr; i++) {
		struct ksym *s = smap_get(&ctx->symtab, ctx->symlist.v[i]);
		if (!s)
			continue;
		for (struct kprop *p = s->props; p; p = p->next) {
			if (p->kind != KP_DEPENDS)
				continue;
			s->effective_dep = expr_and_take(s->effective_dep,
							 expr_clone(p->expr));
		}
	}
}

/* ================================================================== */
/*  Pass 3: reverse deps from select / imply                          */
/* ================================================================== */

static void pass_rev_dep(struct kc_ctx *ctx)
{
	for (size_t i = 0; i < ctx->symlist.nr; i++) {
		struct ksym *src = smap_get(&ctx->symtab, ctx->symlist.v[i]);
		if (!src)
			continue;
		for (struct kprop *p = src->props; p; p = p->next) {
			if (p->kind != KP_SELECT && p->kind != KP_IMPLY)
				continue;
			if (!p->expr || p->expr->op != KE_SYMREF)
				continue;
			struct ksym *dst = p->expr->sym;
			if (!dst)
				continue;
			/* term = (src AND cond) */
			struct kexpr *selref = expr_alloc(KE_SYMREF);
			selref->sym = src;
			struct kexpr *term =
			    p->cond ? expr_and_take(selref, expr_clone(p->cond))
				    : selref;
			struct kexpr **slot = (p->kind == KP_SELECT)
						  ? &dst->rev_dep
						  : &dst->weak_rev_dep;
			*slot = expr_or_take(*slot, term);
		}
	}
}

/* ================================================================== */
/*  Default / value helpers                                           */
/* ================================================================== */

/* Extract a concrete string value from a default-expression given the
 * symbol's type.  For bool symbols the expr is evaluated as a boolean
 * ("y"/"n"); for others we pull the leaf literal. */
static char *default_value(const struct kexpr *expr, enum ksym_type type)
{
	if (!expr)
		return sbuf_strdup("");
	if (type == KS_BOOL) {
		return sbuf_strdup(eval_bool(expr) ? "y" : "n");
	}
	/* Non-bool: prefer a literal leaf; otherwise fall back to the
	 * symref's current string.  Complex expressions are rare on
	 * non-bool defaults in ESP-IDF. */
	if (expr->op == KE_LITERAL)
		return sbuf_strdup(expr->str ? expr->str : "");
	if (expr->op == KE_SYMREF)
		return sbuf_strdup(sym_str(expr->sym));
	/* Fallback. */
	return sbuf_strdup("");
}

/* Apply range clamping to a numeric-looking value string.  Modifies
 * @p val in place (via replacement). */
static char *range_clamp(char *val, const struct kexpr *lo,
			 const struct kexpr *hi, enum ksym_type type)
{
	if (type != KS_INT && type != KS_HEX && type != KS_FLOAT)
		return val;
	if (!lo || !hi || !val)
		return val;

	char *ve;
	long long v = strtoll(val, &ve, 0);
	if (ve == val || *ve)
		return val; /* non-numeric; leave alone */

	const char *los = expr_str(lo);
	const char *his = expr_str(hi);
	char *le, *he;
	long long lv = strtoll(los, &le, 0);
	long long hv = strtoll(his, &he, 0);
	if (le == los || *le || he == his || *he)
		return val;

	if (v < lv)
		v = lv;
	else if (v > hv)
		v = hv;
	else
		return val;

	char buf[32];
	if (type == KS_HEX)
		snprintf(buf, sizeof(buf), "0x%llx", v);
	else
		snprintf(buf, sizeof(buf), "%lld", v);
	free(val);
	return sbuf_strdup(buf);
}

/* ================================================================== */
/*  Pass 5: fixpoint                                                  */
/* ================================================================== */

static int fixpoint_step(struct kc_ctx *ctx)
{
	int changed = 0;
	for (size_t i = 0; i < ctx->symlist.nr; i++) {
		struct ksym *s = smap_get(&ctx->symtab, ctx->symlist.v[i]);
		if (!s)
			continue;
		/* Pseudo-symbols "y"/"n"/"m" don't get resolved. */
		if (name_bool_const(s->name) >= 0)
			continue;
		/* Numeric/string bareword literals aren't real symbols. */
		if (s->type == KS_UNKNOWN && !s->props)
			continue;

		int new_visible = eval_bool(s->effective_dep);

		char *new_val = NULL;
		if (s->user_set && new_visible) {
			new_val = sbuf_strdup(s->cur_val ? s->cur_val : "");
		} else {
			for (struct kprop *p = s->props; p; p = p->next) {
				if (p->kind != KP_DEFAULT)
					continue;
				if (!eval_bool(p->cond))
					continue;
				new_val = default_value(p->expr, s->type);
				break;
			}
			if (!new_val) {
				const char *zero;
				switch (s->type) {
				case KS_BOOL:
					zero = "n";
					break;
				case KS_INT:
					zero = "0";
					break;
				case KS_HEX:
					zero = "0x0";
					break;
				case KS_FLOAT:
					zero = "0.0";
					break;
				default:
					zero = "";
					break;
				}
				new_val = sbuf_strdup(zero);
			}
		}

		/*
		 * Select: force "y" if any selector is active.  NULL
		 * rev_dep means "nothing selects this", so guard with
		 * the pointer check -- eval_bool(NULL) returns 1, which
		 * is the right default for conditions (`if` guards) but
		 * wrong here where absence means false.
		 */
		if (s->type == KS_BOOL && s->rev_dep && eval_bool(s->rev_dep)) {
			free(new_val);
			new_val = sbuf_strdup("y");
		}
		/* Imply: nudge "n" -> "y" if visible and implier active. */
		if (s->type == KS_BOOL && new_visible && s->weak_rev_dep &&
		    eval_bool(s->weak_rev_dep)) {
			if (strcmp(new_val, "y") != 0) {
				free(new_val);
				new_val = sbuf_strdup("y");
			}
		}

		/* Range clamp for int/hex/float. */
		for (struct kprop *p = s->props; p; p = p->next) {
			if (p->kind != KP_RANGE)
				continue;
			if (!eval_bool(p->cond))
				continue;
			new_val =
			    range_clamp(new_val, p->expr, p->expr2, s->type);
			break;
		}

		if (!s->cur_val || strcmp(s->cur_val, new_val) != 0 ||
		    s->visible != new_visible) {
			free(s->cur_val);
			s->cur_val = new_val;
			s->visible = new_visible;
			changed = 1;
		} else {
			free(new_val);
		}
	}
	return changed;
}

/* ================================================================== */
/*  Entry point                                                       */
/* ================================================================== */

void kc_eval(struct kc_ctx *ctx)
{
	pass_ctx_dep(ctx->root, NULL);
	pass_sym_dep(ctx);
	pass_rev_dep(ctx);

	int iter = 0;
	while (fixpoint_step(ctx)) {
		if (++iter >= MAX_FIXPOINT_ITERS)
			die("kc_eval: fixpoint did not converge after %d "
			    "iterations",
			    MAX_FIXPOINT_ITERS);
	}
}

/* ================================================================== */
/*  User-value seed                                                   */
/* ================================================================== */

void kc_sym_set_user(struct kc_ctx *ctx, const char *name, const char *val)
{
	struct ksym *s = smap_get(&ctx->symtab, name);
	if (!s)
		return; /* tolerant: skip unknown (matches python kconfgen) */
	free(s->cur_val);
	s->cur_val = sbuf_strdup(val ? val : "");
	s->user_set = 1;
}

/* ================================================================== */
/*  Debug dump                                                        */
/* ================================================================== */

static const char *type_name(enum ksym_type t)
{
	switch (t) {
	case KS_BOOL:
		return "bool";
	case KS_INT:
		return "int";
	case KS_HEX:
		return "hex";
	case KS_STRING:
		return "string";
	case KS_FLOAT:
		return "float";
	case KS_UNKNOWN:
		return "?";
	}
	return "?";
}

void kc_symbols_dump(const struct kc_ctx *ctx)
{
	for (size_t i = 0; i < ctx->symlist.nr; i++) {
		struct ksym *s = smap_get(&ctx->symtab, ctx->symlist.v[i]);
		if (!s)
			continue;
		/* Skip bareword constants and numeric literals. */
		if (name_bool_const(s->name) >= 0)
			continue;
		if (s->type == KS_UNKNOWN && !s->props)
			continue;
		printf("%s : %-6s = %-16s %s\n", s->name, type_name(s->type),
		       s->cur_val ? s->cur_val : "(null)",
		       s->visible ? "[visible]" : "[hidden]");
	}
}
