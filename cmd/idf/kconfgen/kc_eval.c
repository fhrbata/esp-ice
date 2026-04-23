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
	/* Undefined / bareword KE_SYMREF leaves: python kconfiglib returns
	 * the symbol's name as its str_value when the symbol has no type,
	 * which is how comparisons like `UNDEF_SYM >= 300` become truthy
	 * under strcmp ("UNDEF_SYM" > "300").  Covers both numeric
	 * literals the lexer stashed as KE_SYMREF and references to
	 * symbols that were never `config`'d anywhere. */
	if (s->type == KS_UNKNOWN && s->name[0] && !s->cur_val)
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

/*
 * Evaluate a comparison of two leaf values.
 *
 * Try integer parse on both sides first; if that succeeds, compare
 * numerically.  Otherwise try double parse -- if either side looks
 * like a float (e.g. `5.3`, `10.0`) we want numeric ordering so
 * `VERSION_2 > 5.3` with @c VERSION_2=10.0 returns true (python
 * kconfiglib does this to avoid the lexicographic trap where
 * "10.0" < "5.3").  Only if both paths fail do we fall back to
 * byte-wise string compare.
 */
static int cmp_eval(enum kexpr_op op, const char *a, const char *b)
{
	char *ea, *eb;
	long long va = strtoll(a, &ea, 0);
	long long vb = strtoll(b, &eb, 0);
	int integer = (ea != a && !*ea && eb != b && !*eb);

	int c;
	if (integer) {
		c = (va < vb) ? -1 : (va > vb) ? 1 : 0;
	} else {
		double da = strtod(a, &ea);
		double db = strtod(b, &eb);
		int floaty = (ea != a && !*ea && eb != b && !*eb);
		if (floaty)
			c = (da < db) ? -1 : (da > db) ? 1 : 0;
		else
			c = strcmp(a, b);
	}

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

int kc_expr_bool(const struct kexpr *e) { return eval_bool(e); }

/* ================================================================== */
/*  Pass 1: menu context deps                                         */
/* ================================================================== */

static void pass_ctx_dep(struct kmenu *m, const struct kexpr *parent_ctx)
{
	/*
	 * Own ctx_dep = parent ctx AND menu's own @c depends-on.
	 *
	 * @c visible_if is NOT folded in: it only hides a menu from the
	 * UI (so its child prompts stop being interactively set-able),
	 * but child symbols still compute their values normally.  Python
	 * kconfgen preserves this distinction -- e.g. the MMU_PAGE_SIZE_*
	 * family in components/soc/Kconfig lives under `visible if 0`
	 * yet is still emitted with its matched-default value.
	 */
	struct kexpr *own = NULL;
	if (parent_ctx)
		own = expr_clone(parent_ctx);
	if (m->dep)
		own = expr_and_take(own, expr_clone(m->dep));
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

	/*
	 * Pass 2b: walk menu tree; for each menu that owns a symbol,
	 * OR its ctx_dep into the symbol's effective_dep.  A Kconfig
	 * can define the same @c config X in multiple `if` blocks
	 * (ESP-IDF uses this pattern for WiFi: a main definition under
	 * @c if (WIFI_ENABLED || HOST_WIFI_ENABLED) and a stub under
	 * @c if !WIFI_ENABLED).  Kconfiglib's @c direct_dep is the OR
	 * of each definition's propagated dep, so the symbol stays
	 * visible whenever @em any of its declaration contexts is
	 * satisfied.  Previously ice picked one menu's ctx_dep, which
	 * happened to be the last-declared due to the stack-pop order,
	 * hiding symbols whose stub definition was under a negated
	 * condition.
	 *
	 * The walk stack grows dynamically via ALLOC_GROW -- ESP-IDF's
	 * menu tree has a few thousand nodes and a fixed 256-slot stack
	 * silently lost nodes past the cap.
	 */
	struct kmenu **stack = NULL;
	size_t sp = 0, salloc = 0;
	ALLOC_GROW(stack, sp + 1, salloc);
	stack[sp++] = ctx->root;
	while (sp) {
		struct kmenu *m = stack[--sp];
		if (m->sym && m->ctx_dep &&
		    (m->kind == KM_SYM || m->kind == KM_CHOICE))
			m->sym->effective_dep = expr_or_take(
			    m->sym->effective_dep, expr_clone(m->ctx_dep));
		for (struct kmenu *c = m->children; c; c = c->next) {
			ALLOC_GROW(stack, sp + 1, salloc);
			stack[sp++] = c;
		}
	}
	free(stack);

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
			/*
			 * term = src AND select-if-cond AND ctx_dep
			 *
			 * Python kconfiglib propagates each menu's dep into
			 * the cond of @c select / @c imply on every symbol
			 * declared under it (@c _propagate_deps), which means
			 * a @c select X written on a definition that sits under
			 * an @c if FOO block only fires when @c FOO is true.
			 * We mirror that by AND-ing in the declaring node's
			 * @c ctx_dep, which pass_ctx_dep computed from every
			 * ancestor @c menu / @c if / @c choice.
			 *
			 * Without this, a symbol whose primary definition is
			 * hidden by its menu context but whose stub definition
			 * in a separate file stays visible would still force
			 * the selected target -- we saw this on esp32p4 where
			 * ESP_WIFI_SLP_IRAM_OPT (visible via the host-Wi-Fi
			 * stub) was forcing ESP_PHY_IRAM_OPT=y even though
			 * the `select ESP_PHY_IRAM_OPT` lives on the native-
			 * Wi-Fi definition whose @c if is false on p4.
			 */
			struct kexpr *selref = expr_alloc(KE_SYMREF);
			selref->sym = src;
			struct kexpr *term = selref;
			if (p->cond)
				term = expr_and_take(term, expr_clone(p->cond));
			if (p->menu && p->menu->ctx_dep)
				term = expr_and_take(
				    term, expr_clone(p->menu->ctx_dep));
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
	 * non-bool defaults in ESP-IDF.  Hex literals are kept verbatim
	 * -- python's str_value for `default 0` on a hex symbol is "0",
	 * for `default 33` it's "33".  The sdkconfig.h / sdkconfig.cmake
	 * writers (emit_hex_for_header, emit_hex_for_cmake) add the 0x
	 * prefix at emit time, which is where python adds it too. */
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
/*  Pass 3.5: link choice members to their choice group               */
/* ================================================================== */

/*
 * Recursively attach @p choice_sym as the choice_parent of every
 * descendant bool symbol, stopping at nested choices / menus that
 * start their own scope.  @c if-blocks (KM_IF) are transparent so
 * that `if X config Y` inside a choice still makes Y a member.
 */
static void link_choice_members(struct kmenu *m, struct ksym *choice_sym)
{
	for (struct kmenu *c = m->children; c; c = c->next) {
		switch (c->kind) {
		case KM_SYM:
			if (c->sym && c->sym->type == KS_BOOL &&
			    !c->sym->choice_parent)
				c->sym->choice_parent = choice_sym;
			break;
		case KM_IF:
			link_choice_members(c, choice_sym);
			break;
		case KM_CHOICE:
		case KM_MENU:
		case KM_COMMENT:
		case KM_ROOT:
			/* Do not descend into nested scopes. */
			break;
		}
	}
}

static void pass_link_choices(struct kc_ctx *ctx)
{
	struct kmenu **stack = NULL;
	size_t sp = 0, salloc = 0;
	ALLOC_GROW(stack, sp + 1, salloc);
	stack[sp++] = ctx->root;
	while (sp) {
		struct kmenu *m = stack[--sp];
		if (m->kind == KM_CHOICE && m->sym) {
			m->sym->choice_menu = m;
			link_choice_members(m, m->sym);
		}
		if ((m->kind == KM_SYM || m->kind == KM_CHOICE) && m->sym &&
		    !m->sym->decl_menu)
			m->sym->decl_menu = m;
		for (struct kmenu *c = m->children; c; c = c->next) {
			ALLOC_GROW(stack, sp + 1, salloc);
			stack[sp++] = c;
		}
	}
	free(stack);
}

/* ================================================================== */
/*  Choice enforcement inside the fixpoint                            */
/* ================================================================== */

/*
 * Pick the "winner" member for @p choice_sym and force every other
 * member to "n".  Returns 1 if any value changed.
 *
 * Selection precedence (matches python kconfgen):
 *   1. The last user-set member with value "y" (CLI argv order).
 *   2. The first @c default MEMBER prop on the choice whose @c if-
 *      condition is satisfied and whose target is one of the members.
 *   3. Otherwise the first member in declaration order.
 */
static void collect_choice_members(const struct kmenu *m,
				   struct ksym *choice_sym, struct ksym **out,
				   int *n, int max)
{
	if (!m)
		return;
	if ((m->kind == KM_SYM || m->kind == KM_CHOICE) && m->sym &&
	    m->sym->choice_parent == choice_sym && *n < max) {
		/* Dedup: a Kconfig with `# ignore: multiple-definition` can
		 * back the same ksym from several KM_SYM nodes. */
		int dup = 0;
		for (int i = 0; i < *n; i++) {
			if (out[i] == m->sym) {
				dup = 1;
				break;
			}
		}
		if (!dup)
			out[(*n)++] = m->sym;
	}
	for (const struct kmenu *c = m->children; c; c = c->next)
		collect_choice_members(c, choice_sym, out, n, max);
}

static int enforce_choice(struct kc_ctx *ctx, struct ksym *choice_sym)
{
	struct ksym *winner = NULL;

	/*
	 * Collect members in Kconfig declaration order by walking the
	 * choice's KM_CHOICE menu subtree.  Using ctx->symlist here would
	 * be wrong: a forward reference like `default MEMBER if COND` in
	 * the choice body registers MEMBER in the symbol table before the
	 * member's own @c config line runs, so symlist order differs from
	 * source order.  Rule 3 below relies on source order.
	 */
	(void)ctx;
	struct ksym *members[64];
	int n_members = 0;
	collect_choice_members(choice_sym->choice_menu, choice_sym, members,
			       &n_members, 64);
	if (!n_members)
		return 0;

	/*
	 * When the choice group itself is hidden (its depends-on chain
	 * is false), every member is "n" -- no winner is selected, no
	 * default applies.  Python kconfgen then omits the members
	 * entirely from writable output.  Skipping the member loop
	 * below also means they stay at whatever zero value the per-
	 * symbol pass left them with, which is what we want.
	 */
	if (!choice_sym->visible) {
		int changed = 0;
		for (int i = 0; i < n_members; i++) {
			if (!members[i]->cur_val ||
			    strcmp(members[i]->cur_val, "n") != 0) {
				free(members[i]->cur_val);
				members[i]->cur_val = sbuf_strdup("n");
				changed = 1;
			}
		}
		return changed;
	}

	/* Rule 1: last user-set "y" wins. */
	for (int i = n_members - 1; i >= 0; i--) {
		if (members[i]->user_set && members[i]->cur_val &&
		    !strcmp(members[i]->cur_val, "y")) {
			winner = members[i];
			break;
		}
	}
	/* Rule 2: first applicable default that names a member. */
	if (!winner) {
		for (struct kprop *p = choice_sym->props; p; p = p->next) {
			if (p->kind != KP_DEFAULT || !p->expr ||
			    p->expr->op != KE_SYMREF)
				continue;
			if (!eval_bool(p->cond))
				continue;
			struct ksym *target = p->expr->sym;
			for (int i = 0; i < n_members; i++) {
				if (members[i] == target) {
					winner = members[i];
					break;
				}
			}
			if (winner)
				break;
		}
	}
	/* Rule 3: first member in declaration order. */
	if (!winner)
		winner = members[0];

	int changed = 0;
	for (int i = 0; i < n_members; i++) {
		const char *target = (members[i] == winner) ? "y" : "n";
		if (!members[i]->cur_val ||
		    strcmp(members[i]->cur_val, target) != 0) {
			free(members[i]->cur_val);
			members[i]->cur_val = sbuf_strdup(target);
			changed = 1;
		}
	}
	return changed;
}

static int fixpoint_enforce_choices(struct kc_ctx *ctx)
{
	int changed = 0;
	for (size_t i = 0; i < ctx->symlist.nr; i++) {
		struct ksym *s = smap_get(&ctx->symtab, ctx->symlist.v[i]);
		if (s && s->is_choice)
			changed |= enforce_choice(ctx, s);
	}
	return changed;
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

		/*
		 * Choice members are entirely owned by
		 * fixpoint_enforce_choices: it picks the winner and
		 * forces every other member to "n".  Touching their
		 * values here would fight with that pass and oscillate
		 * the fixpoint indefinitely.  We still need to keep
		 * @c visible up-to-date so depends-on expressions that
		 * reference members still read a sane value.
		 */
		if (s->choice_parent) {
			/*
			 * A choice member inherits the choice's visibility:
			 * if the choice group is hidden (its `depends on`
			 * chain is false), none of its members should be
			 * visible either.  Ice's menu tree doesn't
			 * automatically propagate the choice's KP_DEPENDS
			 * into the member's own effective_dep, so gate it
			 * here explicitly.
			 */
			int new_visible = eval_bool(s->effective_dep) &&
					  s->choice_parent->visible;
			if (s->visible != new_visible) {
				s->visible = new_visible;
				changed = 1;
			}
			continue;
		}

		int new_visible = eval_bool(s->effective_dep);

		char *new_val = NULL;
		int default_hit = 0;
		/*
		 * A symbol "sticks" at its current value (same treatment as
		 * an explicit user-set) when:
		 *   - the line in sdkconfig had no `# default:` pragma
		 *     (s->user_set), OR
		 *   - the line had the pragma and KCONFIG_DEFAULTS_POLICY is
		 *     the python default `"sdkconfig"` (ctx->defaults_policy
		 *     == 0) -- this prevents generation-to-generation drift
		 *     when the user has not touched the sdkconfig and the
		 *     Kconfig default later changes.
		 * Under policy `"kconfig"` (1), pragma'd values are ignored
		 * and the Kconfig default is re-applied.
		 */
		int stick = s->user_set ||
			    (s->default_seeded && ctx->defaults_policy == 0);
		if (stick && new_visible) {
			new_val = sbuf_strdup(s->cur_val ? s->cur_val : "");
			/* A stuck-default is still a default for emit
			 * purposes; only a genuine user_set drops the pragma.
			 * Propagate default_seeded to default_applied so the
			 * no-prompt int/hex/string emit gate (which checks
			 * default_applied || user_set) preserves round-tripped
			 * `# default:` symbols across sdkconfig reloads. */
			default_hit = s->default_seeded;
		} else if (new_visible) {
			/*
			 * Defaults only apply when the symbol is visible.
			 * Python kconfgen's semantics: a symbol whose
			 * depends-on chain is false is forced to the type's
			 * zero value regardless of what @c default lines say.
			 * Skipping this gate would turn bool symbols like
			 * CONFIG_SECURE_SIGNED_APPS (`default y` + a `depends
			 * on` that's currently false) into "y", when they
			 * should be "n" and thus not emitted at all.
			 */
			for (struct kprop *p = s->props; p; p = p->next) {
				if (p->kind != KP_DEFAULT)
					continue;
				if (!eval_bool(p->cond))
					continue;
				new_val = default_value(p->expr, s->type);
				default_hit = 1;
				break;
			}
		}
		if (s->default_applied != default_hit) {
			s->default_applied = default_hit;
			changed = 1;
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

		/*
		 * Select: force "y" if any selector is active.  NULL
		 * rev_dep means "nothing selects this", so guard with
		 * the pointer check -- eval_bool(NULL) returns 1, which
		 * is the right default for conditions (`if` guards) but
		 * wrong here where absence means false.
		 *
		 * Python's esp_kconfiglib makes `select` override the
		 * target's `depends on` chain: a selected symbol
		 * effectively becomes visible with value y even if its
		 * own depends-chain is false.  Mark the symbol visible
		 * here so the emit-gate picks it up (SelectImply.out
		 * expects CONFIG_L=y with `depends on NOT_DEFINED`).
		 */
		if (s->type == KS_BOOL && s->rev_dep && eval_bool(s->rev_dep)) {
			free(new_val);
			new_val = sbuf_strdup("y");
			if (!new_visible) {
				new_visible = 1;
			}
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

/*
 * ESP-IDF `set TARGET=VAL` / `set default TARGET=VAL` propagation.
 *
 * Applied after the main fixpoint converges.  Walks all symbols in
 * declaration order (the order kc_sym_intern recorded in @c symlist)
 * and, for each source symbol that ended up at bool "y" with its @c if
 * condition true, overwrites the target's cur_val.
 *
 * Per esp_kconfiglib:
 *   - @c set (KP_SET): strong.  First active source in declaration
 *     order wins; subsequent strong setters for the same target are
 *     ignored.  Beats any prior weak setter.
 *   - @c set default (KP_SET_DEFAULT): weak.  Only applies when the
 *     target has no strong setter.  First weak setter wins over
 *     later weak ones.
 *
 * After propagation we mark the target @c default_seeded so the
 * evaluator's stick-on-default behaviour carries the value through
 * any subsequent passes without requiring an explicit user_set,
 * while still letting emit mark the line with the @c # default:
 * pragma (set-propagated values are not user input).
 */
static void pass_apply_sets(struct kc_ctx *ctx)
{
	for (size_t pass = 0; pass < 2; pass++) {
		/* Pass 0: strong (KP_SET); Pass 1: weak (KP_SET_DEFAULT). */
		enum kprop_kind want = pass == 0 ? KP_SET : KP_SET_DEFAULT;
		int rank = pass == 0 ? 2 : 1;
		for (size_t i = 0; i < ctx->symlist.nr; i++) {
			struct ksym *src =
			    smap_get(&ctx->symtab, ctx->symlist.v[i]);
			if (!src || src->type != KS_BOOL || !src->cur_val ||
			    strcmp(src->cur_val, "y") != 0)
				continue;
			for (struct kprop *p = src->props; p; p = p->next) {
				if (p->kind != want)
					continue;
				if (p->cond && !eval_bool(p->cond))
					continue;
				struct ksym *tgt =
				    smap_get(&ctx->symtab, p->text);
				if (!tgt)
					continue;
				/* A strong rule overrides a weak one; a weak
				 * rule defers to any previously-applied rule
				 * (strong or weak). */
				if (tgt->set_rank >= rank)
					continue;
				char *val = default_value(p->expr, tgt->type);
				free(tgt->cur_val);
				tgt->cur_val = val;
				tgt->default_seeded = 1;
				tgt->visible = 1;
				tgt->set_rank = rank;
			}
		}
	}
}

void kc_eval(struct kc_ctx *ctx)
{
	pass_ctx_dep(ctx->root, NULL);
	pass_sym_dep(ctx);
	pass_rev_dep(ctx);
	pass_link_choices(ctx);

	int iter = 0;
	for (;;) {
		int changed = fixpoint_step(ctx);
		changed |= fixpoint_enforce_choices(ctx);
		if (!changed)
			break;
		if (++iter >= MAX_FIXPOINT_ITERS)
			die("kc_eval: fixpoint did not converge after %d "
			    "iterations",
			    MAX_FIXPOINT_ITERS);
	}

	pass_apply_sets(ctx);
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
	/* A direct user-set line (CONFIG_X=y without a `# default:` pragma)
	 * clears any previously-seeded default state: the user has taken
	 * over this symbol and the pragma no longer applies. */
	s->default_seeded = 0;
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
