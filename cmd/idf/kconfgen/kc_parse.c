/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_parse.c
 * @brief Kconfig recursive-descent parser + AST lifecycle + debug dump.
 *
 * Drives a @p kc_lexer (see kc_lex.c) through the Kconfig LL(1)
 * grammar, building a menu tree of @p kmenu, a symbol table of
 * @p ksym, and property lists of @p kprop hanging off symbols.
 *
 * Layout:
 *   1. AST lifecycle -- kc_ctx_init / kc_ctx_release, interning helpers,
 *      recursive AST free'rs.
 *   2. Parser helpers -- advance/consume/accept, NL skipping.
 *   3. Expression parser -- recursive descent with explicit precedence
 *      levels (or → and → cmp → unary → primary).
 *   4. Property parsers -- one per kprop_kind.
 *   5. Statement parsers -- config, menu, choice, if, comment, source.
 *   6. Entry point -- kc_parse_file(ctx, path).
 *   7. AST dump.
 */
#include "ice.h"
#include "kc_ast.h"
#include "kc_lex.h"

/* ================================================================== */
/*  AST allocation / lifecycle                                        */
/* ================================================================== */

static struct kexpr *expr_new(enum kexpr_op op)
{
	struct kexpr *e = calloc(1, sizeof(*e));
	if (!e)
		die_errno("calloc");
	e->op = op;
	return e;
}

static void expr_free(struct kexpr *e)
{
	if (!e)
		return;
	expr_free(e->l);
	expr_free(e->r);
	free(e->str);
	free(e);
}

/* @c prop_new (defined below, after @c struct kc_parser is declared) stamps
 * the parser's current declaration menu onto each fresh kprop so the
 * evaluator can fold the declaring node's ctx_dep into prop activation
 * conditions.  Forward the type here; the declaration-menu pointer is
 * read through it. */
struct kc_parser;
static struct kprop *prop_new(struct kc_parser *p, enum kprop_kind kind,
			      const char *src_file, int src_line);

static void prop_free(struct kprop *p)
{
	if (!p)
		return;
	expr_free(p->cond);
	expr_free(p->expr);
	expr_free(p->expr2);
	free(p->text);
	free(p);
}

static void sym_append_prop(struct ksym *s, struct kprop *p)
{
	if (s->props_tail) {
		s->props_tail->next = p;
		s->props_tail = p;
	} else {
		s->props = s->props_tail = p;
	}
}

static struct kmenu *menu_new(enum kmenu_kind kind, const char *src_file,
			      int src_line)
{
	struct kmenu *m = calloc(1, sizeof(*m));
	if (!m)
		die_errno("calloc");
	m->kind = kind;
	m->src_file = src_file;
	m->src_line = src_line;
	return m;
}

static void menu_append(struct kmenu *parent, struct kmenu *child)
{
	child->parent = parent;
	if (parent->tail) {
		parent->tail->next = child;
		parent->tail = child;
	} else {
		parent->children = parent->tail = child;
	}
}

/* ---- context ---------------------------------------------------- */

void kc_ctx_init(struct kc_ctx *ctx)
{
	ctx->root = menu_new(KM_ROOT, NULL, 0);
	smap_init(&ctx->symtab);
	svec_init(&ctx->symlist);
	svec_init(&ctx->file_names);
	smap_init(&ctx->vars);
	ctx->renames = NULL;
	ctx->n_renames = 0;
	ctx->alloc_renames = 0;
	ctx->no_deprecated = 0;
	ctx->idf_version = NULL;
	ctx->srctree = NULL;
	ctx->root_file = NULL;
	ctx->defaults_policy = 0;
	ctx->n_notifications = 0;
}

/*
 * Print one diagnostic line to stderr and bump the context's
 * notification counter.  The counter feeds kconfgen.c's end-of-run
 * `Status: Finished ...` summary (successfully vs with notifications),
 * which the upstream esp-idf-kconfig test suite matches against in its
 * golden stderr fixtures.  Callers are responsible for including any
 * "warning:" / "error:" prefix in @p fmt -- the exact line shape has
 * to match what Python's esp_kconfiglib emits.
 */
void kc_ctx_notify(struct kc_ctx *ctx, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (fmt && fmt[0] && fmt[strlen(fmt) - 1] != '\n')
		fputc('\n', stderr);
	ctx->n_notifications++;
}

const char *kc_ctx_intern_file(struct kc_ctx *ctx, const char *path)
{
	for (size_t i = 0; i < ctx->file_names.nr; i++)
		if (strcmp(ctx->file_names.v[i], path) == 0)
			return ctx->file_names.v[i];
	svec_push(&ctx->file_names, path);
	return ctx->file_names.v[ctx->file_names.nr - 1];
}

struct ksym *kc_sym_intern(struct kc_ctx *ctx, const char *name)
{
	struct ksym *s = smap_get(&ctx->symtab, name);
	if (s)
		return s;
	s = calloc(1, sizeof(*s));
	if (!s)
		die_errno("calloc");
	s->name = sbuf_strdup(name);
	smap_put(&ctx->symtab, name, s);
	svec_push(&ctx->symlist, name);
	return s;
}

static void menu_free(struct kmenu *m)
{
	if (!m)
		return;
	struct kmenu *c = m->children;
	while (c) {
		struct kmenu *next = c->next;
		menu_free(c);
		c = next;
	}
	expr_free(m->dep);
	expr_free(m->visible_if);
	expr_free(m->ctx_dep);
	free(m->prompt);
	free(m);
}

void kc_ctx_release(struct kc_ctx *ctx)
{
	menu_free(ctx->root);
	ctx->root = NULL;

	/* Free every symbol held in the symtab. */
	const char *key;
	void *val;
	size_t it = 0;
	while (smap_iter(&ctx->symtab, &it, &key, &val)) {
		struct ksym *s = val;
		struct kprop *p = s->props;
		while (p) {
			struct kprop *next = p->next;
			prop_free(p);
			p = next;
		}
		expr_free(s->effective_dep);
		expr_free(s->rev_dep);
		expr_free(s->weak_rev_dep);
		free(s->cur_val);
		free(s->name);
		free(s);
	}
	smap_release(&ctx->symtab);
	svec_clear(&ctx->symlist);
	svec_clear(&ctx->file_names);

	/*
	 * Preset-variable map: keys are interned by smap itself, but the
	 * values are strdup'd strings we own and must release.
	 */
	{
		const char *vkey;
		void *vval;
		size_t vit = 0;
		while (smap_iter(&ctx->vars, &vit, &vkey, &vval))
			free(vval);
		smap_release(&ctx->vars);
	}

	for (size_t i = 0; i < ctx->n_renames; i++) {
		free(ctx->renames[i].old_name);
		free(ctx->renames[i].new_name);
	}
	free(ctx->renames);
	ctx->renames = NULL;
	ctx->n_renames = 0;
	ctx->alloc_renames = 0;

	free(ctx->idf_version);
	ctx->idf_version = NULL;

	free(ctx->srctree);
	ctx->srctree = NULL;

	free(ctx->root_file);
	ctx->root_file = NULL;
}

/* ================================================================== */
/*  Parser state + helpers                                            */
/* ================================================================== */

struct kc_parser {
	struct kc_ctx *ctx;
	struct kc_lexer lex;
	/*
	 * Non-zero while parsing the body of a @c choice ... @c endchoice
	 * block; used so @c parse_default_line can emit the
	 * DefaultInChoice warning when a choice member has its own
	 * @c default (esp_kconfiglib silently drops those defaults
	 * because the choice mechanism picks the member).
	 */
	int in_choice;
	/*
	 * Current @c KM_SYM / @c KM_CHOICE menu whose body is being
	 * parsed.  Set by @c parse_config_stmt / @c parse_choice_stmt
	 * around their prop-body loop and cleared on exit.  @c prop_new
	 * stamps this onto every @c kprop it creates so the evaluator can
	 * fold the specific declaration's @c ctx_dep into the prop's
	 * activation condition -- crucial for @c select / @c imply on a
	 * symbol with multiple definitions (the selector inherits its
	 * declaring block's @c if guard, matching python kconfiglib's
	 * @c _propagate_deps).
	 */
	struct kmenu *cur_decl_menu;
};

static struct kprop *prop_new(struct kc_parser *p, enum kprop_kind kind,
			      const char *src_file, int src_line)
{
	struct kprop *pr = calloc(1, sizeof(*pr));
	if (!pr)
		die_errno("calloc");
	pr->kind = kind;
	pr->src_file = src_file;
	pr->src_line = src_line;
	pr->menu = p ? p->cur_decl_menu : NULL;
	return pr;
}

static void p_advance(struct kc_parser *p) { kc_lex_next(&p->lex); }

static void p_expect(struct kc_parser *p, int tok)
{
	if (p->lex.tok != tok)
		kc_lex_die_unexpected(&p->lex, tok);
	p_advance(p);
}

static int p_accept(struct kc_parser *p, int tok)
{
	if (p->lex.tok == tok) {
		p_advance(p);
		return 1;
	}
	return 0;
}

/* Skip zero or more blank logical-lines. */
static void p_skip_nl(struct kc_parser *p)
{
	while (p->lex.tok == KT_NL)
		p_advance(p);
}

/* Consume a newline if present -- used at the end of single-line
 * statements where the preceding element may have already eaten it. */
static void p_eat_nl(struct kc_parser *p)
{
	if (p->lex.tok == KT_NL)
		p_advance(p);
}

/* Take ownership of the current NAME/STR value, leaving the lexer's
 * @c val slot cleared. */
static char *p_take_val(struct kc_parser *p)
{
	char *s = p->lex.val;
	p->lex.val = NULL;
	return s;
}

/* ================================================================== */
/*  Expression parser                                                 */
/* ================================================================== */

static struct kexpr *parse_or_expr(struct kc_parser *p);

static struct kexpr *parse_primary(struct kc_parser *p)
{
	if (p->lex.tok == KT_LPAREN) {
		p_advance(p);
		struct kexpr *e = parse_or_expr(p);
		p_expect(p, KT_RPAREN);
		return e;
	}
	if (p->lex.tok == KT_STR) {
		struct kexpr *e = expr_new(KE_LITERAL);
		e->str = p_take_val(p);
		p_advance(p);
		return e;
	}
	if (p->lex.tok == KT_NAME) {
		/*
		 * Bareword.  Could be a symbol reference, or a bareword
		 * literal (y / n / m / numeric).  We always store it as
		 * a symbol reference here; the evaluator recognises the
		 * special "y" / "n" / "m" names and numeric-looking
		 * payloads as constants later.
		 */
		struct kexpr *e = expr_new(KE_SYMREF);
		e->sym = kc_sym_intern(p->ctx, p->lex.val);
		p_advance(p);
		return e;
	}
	die("%s:%d: expected expression, got %s", p->lex.path, p->lex.line,
	    kc_tok_name(p->lex.tok));
	return NULL;
}

static struct kexpr *parse_unary(struct kc_parser *p)
{
	if (p_accept(p, KT_NOT)) {
		struct kexpr *e = expr_new(KE_NOT);
		e->l = parse_unary(p);
		return e;
	}
	return parse_primary(p);
}

static struct kexpr *parse_cmp(struct kc_parser *p)
{
	struct kexpr *lhs = parse_unary(p);
	enum kexpr_op op;
	switch (p->lex.tok) {
	case KT_EQ:
		op = KE_EQ;
		break;
	case KT_NE:
		op = KE_NE;
		break;
	case KT_LT:
		op = KE_LT;
		break;
	case KT_LE:
		op = KE_LE;
		break;
	case KT_GT:
		op = KE_GT;
		break;
	case KT_GE:
		op = KE_GE;
		break;
	default:
		return lhs;
	}
	p_advance(p);
	struct kexpr *e = expr_new(op);
	e->l = lhs;
	e->r = parse_unary(p);
	return e;
}

static struct kexpr *parse_and_expr(struct kc_parser *p)
{
	struct kexpr *e = parse_cmp(p);
	while (p_accept(p, KT_AND)) {
		struct kexpr *rhs = parse_cmp(p);
		struct kexpr *n = expr_new(KE_AND);
		n->l = e;
		n->r = rhs;
		e = n;
	}
	return e;
}

static struct kexpr *parse_or_expr(struct kc_parser *p)
{
	struct kexpr *e = parse_and_expr(p);
	while (p_accept(p, KT_OR)) {
		struct kexpr *rhs = parse_and_expr(p);
		struct kexpr *n = expr_new(KE_OR);
		n->l = e;
		n->r = rhs;
		e = n;
	}
	return e;
}

static struct kexpr *parse_expr(struct kc_parser *p)
{
	return parse_or_expr(p);
}

/* Optional `if EXPR` tail. */
static struct kexpr *parse_if_tail(struct kc_parser *p)
{
	if (p_accept(p, KT_IF))
		return parse_expr(p);
	return NULL;
}

/* ================================================================== */
/*  Property parsers                                                  */
/* ================================================================== */

/* True when @p tok is a property keyword that can appear inside a
 * config / choice body.  Used to detect body termination. */
static int is_prop_tok(int tok)
{
	switch (tok) {
	case KT_BOOL:
	case KT_TRISTATE:
	case KT_INT:
	case KT_HEX:
	case KT_STRING:
	case KT_FLOAT:
	case KT_DEFAULT:
	case KT_DEF_BOOL:
	case KT_DEF_INT:
	case KT_DEF_HEX:
	case KT_DEF_STRING:
	case KT_DEF_TRISTATE:
	case KT_PROMPT:
	case KT_DEPENDS:
	case KT_SELECT:
	case KT_IMPLY:
	case KT_RANGE:
	case KT_HELP:
	case KT_OPTION:
	case KT_OPTIONAL:
	case KT_MODULES:
	case KT_VISIBLE:
	case KT_WARNING:
	case KT_SET:
		return 1;
	}
	return 0;
}

static enum ksym_type tok_to_ksym_type(int tok)
{
	switch (tok) {
	case KT_BOOL:
	case KT_TRISTATE: /* ESP has no tristate; treat as bool. */
		return KS_BOOL;
	case KT_INT:
		return KS_INT;
	case KT_HEX:
		return KS_HEX;
	case KT_STRING:
		return KS_STRING;
	case KT_FLOAT:
		return KS_FLOAT;
	default:
		return KS_UNKNOWN;
	}
}

/* bool / int / hex / string / float [ "prompt" [ if EXPR ] ] NL */
static void parse_type_line(struct kc_parser *p, struct ksym *sym)
{
	enum ksym_type t = tok_to_ksym_type(p->lex.tok);
	int line = p->lex.line;
	p_advance(p); /* type keyword */

	if (sym->type == KS_UNKNOWN || sym->type == t)
		sym->type = t;
	/* If already typed differently, keep the first type (matches upstream
	 * kconfig's "first wins" on type). */

	if (p->lex.tok == KT_STR) {
		struct kprop *pr = prop_new(p, KP_PROMPT, p->lex.path, line);
		pr->text = p_take_val(p);
		p_advance(p);
		pr->cond = parse_if_tail(p);
		sym_append_prop(sym, pr);
	}
	p_expect(p, KT_NL);
}

/* def_bool / def_int / ... EXPR [ if EXPR ] NL
 * Equivalent to a type declaration + a default prop. */
static void parse_deftype_line(struct kc_parser *p, struct ksym *sym)
{
	int tok = p->lex.tok;
	int line = p->lex.line;
	enum ksym_type t;
	switch (tok) {
	case KT_DEF_BOOL:
	case KT_DEF_TRISTATE: /* ESP has no tristate; treat as bool. */
		t = KS_BOOL;
		break;
	case KT_DEF_INT:
		t = KS_INT;
		break;
	case KT_DEF_HEX:
		t = KS_HEX;
		break;
	case KT_DEF_STRING:
		t = KS_STRING;
		break;
	default:
		t = KS_UNKNOWN;
		break;
	}
	p_advance(p);
	if (sym->type == KS_UNKNOWN)
		sym->type = t;

	struct kprop *pr = prop_new(p, KP_DEFAULT, p->lex.path, line);
	pr->expr = parse_expr(p);
	pr->cond = parse_if_tail(p);
	sym_append_prop(sym, pr);
	p_expect(p, KT_NL);
}

/* prompt "text" [ if EXPR ] NL */
static void parse_prompt_line(struct kc_parser *p, struct ksym *sym)
{
	int line = p->lex.line;
	p_advance(p); /* prompt */
	if (p->lex.tok != KT_STR)
		kc_lex_die_unexpected(&p->lex, KT_STR);
	/*
	 * Warn when a symbol accumulates more than one prompt within the
	 * same declaration -- python's esp_kconfiglib produces the
	 * `defined with multiple prompts in single location` diagnostic
	 * the MultiplePrompts fixture checks for.  Walk the existing
	 * props looking for a prior KP_PROMPT; the check runs before we
	 * append the new prop so we only fire on the 2nd (or later)
	 * prompt encountered.
	 */
	for (const struct kprop *prev = sym->props; prev; prev = prev->next) {
		if (prev->kind != KP_PROMPT)
			continue;
		const char *dfile =
		    sym->decl_file ? sym->decl_file : p->lex.path;
		int dline = sym->decl_line ? sym->decl_line : line;
		kc_ctx_notify(p->ctx,
			      "warning: %s (defined at %s:%d) defined with "
			      "multiple prompts in single location",
			      sym->name, dfile, dline);
		break;
	}
	struct kprop *pr = prop_new(p, KP_PROMPT, p->lex.path, line);
	pr->text = p_take_val(p);
	p_advance(p);
	pr->cond = parse_if_tail(p);
	sym_append_prop(sym, pr);
	p_expect(p, KT_NL);
}

/* default EXPR [ if EXPR ] NL */
static void parse_default_line(struct kc_parser *p, struct ksym *sym)
{
	int line = p->lex.line;
	p_advance(p);
	struct kprop *pr = prop_new(p, KP_DEFAULT, p->lex.path, line);
	pr->expr = parse_expr(p);
	pr->cond = parse_if_tail(p);
	sym_append_prop(sym, pr);
	p_expect(p, KT_NL);

	/*
	 * A `default` on a symbol that lives directly inside a
	 * @c choice body has no effect -- the choice itself picks a
	 * member via its own @c default.  Emit the matching
	 * DefaultInChoice warning; the AST still carries the property
	 * so subsequent passes ignore it harmlessly.
	 */
	if (p->in_choice && sym && sym->name && !sym->is_choice) {
		kc_ctx_notify(p->ctx,
			      "warning: default on the choice symbol %s "
			      "will have no effect, as defaults do not "
			      "affect choice symbols",
			      sym->name);
	}
}

/* depends on EXPR NL
 *   (accepted on symbols, menus, choices, comments, and if-blocks). */
static void parse_depends_line(struct kc_parser *p, struct ksym *sym,
			       struct kmenu *m)
{
	int line = p->lex.line;
	p_advance(p); /* depends */
	p_expect(p, KT_ON);
	struct kexpr *e = parse_expr(p);
	p_expect(p, KT_NL);

	/*
	 * For config / menuconfig: attach to the per-declaration @c KM_SYM
	 * menu node's @c dep.  pass_ctx_dep then AND-folds it into this
	 * declaration's ctx_dep, and pass_sym_dep OR-folds each declaration
	 * of the same symbol to produce the python direct_dep semantics:
	 *   direct_dep = OR over defs of (parent_ctx AND local_depends)
	 *
	 * A shared KP_DEPENDS on the ksym would instead AND every
	 * definition's `depends on` together, hiding the symbol whenever
	 * any stub definition's condition was false (as happens with the
	 * WiFi symbols re-declared under `if !ESP_WIFI_ENABLED` in
	 * components/esp_wifi/remote/Kconfig.wifi_is_remote.in).
	 */
	if (m && m->kind == KM_SYM) {
		if (m->dep) {
			struct kexpr *n = expr_new(KE_AND);
			n->l = m->dep;
			n->r = e;
			m->dep = n;
		} else {
			m->dep = e;
		}
	} else if (sym) {
		/* Loose `depends on` inside a choice body attaches to the
		 * choice sym; pass 2c still ANDs it into effective_dep. */
		struct kprop *pr = prop_new(p, KP_DEPENDS, p->lex.path, line);
		pr->expr = e;
		sym_append_prop(sym, pr);
	} else if (m) {
		/* Menu / comment / if-block head: fold into menu dep. */
		if (m->dep) {
			struct kexpr *n = expr_new(KE_AND);
			n->l = m->dep;
			n->r = e;
			m->dep = n;
		} else {
			m->dep = e;
		}
	} else {
		expr_free(e);
	}
}

/* select/imply NAME [ if EXPR ] NL */
static void parse_select_line(struct kc_parser *p, struct ksym *sym)
{
	int line = p->lex.line;
	enum kprop_kind kind = (p->lex.tok == KT_SELECT) ? KP_SELECT : KP_IMPLY;
	p_advance(p);
	if (p->lex.tok != KT_NAME)
		kc_lex_die_unexpected(&p->lex, KT_NAME);
	struct kprop *pr = prop_new(p, kind, p->lex.path, line);
	pr->expr = expr_new(KE_SYMREF);
	pr->expr->sym = kc_sym_intern(p->ctx, p->lex.val);
	p_advance(p);
	pr->cond = parse_if_tail(p);
	sym_append_prop(sym, pr);
	p_expect(p, KT_NL);
}

/*
 * set [default] NAME = EXPR [ if EXPR ] NL
 *
 * ESP-IDF extension for indirect value setting.  On the source symbol,
 * declares that when the source evaluates to bool y (and any optional
 * @c if condition is true), the given target's value should become
 * @c EXPR.  Without the @c default modifier the rule is strong: the
 * first active source in declaration order wins, and the target is
 * treated as user-set for evaluator purposes.  With @c default the
 * rule is weak -- applied only when no strong rule has fired for the
 * same target -- matching python esp_kconfiglib's semantics.
 */
static void parse_set_line(struct kc_parser *p, struct ksym *sym)
{
	int line = p->lex.line;
	p_advance(p); /* past 'set' */
	enum kprop_kind kind = KP_SET;
	if (p->lex.tok == KT_DEFAULT) {
		kind = KP_SET_DEFAULT;
		p_advance(p);
	}
	if (p->lex.tok != KT_NAME)
		kc_lex_die_unexpected(&p->lex, KT_NAME);
	char *target = p_take_val(p);
	p_advance(p);
	p_expect(p, KT_EQ);
	/*
	 * RHS is a primary expression (literal or symref), exactly like
	 * the RHS of @c default, so reuse parse_expr -- it handles
	 * `true/false/numbers/symbols` uniformly.  Strongly-typed checking
	 * (e.g. type-compatibility between target and value) happens at
	 * eval time rather than here.
	 */
	struct kprop *pr = prop_new(p, kind, p->lex.path, line);
	pr->text = target;
	pr->expr = parse_expr(p);
	pr->cond = parse_if_tail(p);
	sym_append_prop(sym, pr);
	p_expect(p, KT_NL);

	/*
	 * `set` and `set default` only make sense on a boolean source:
	 * a non-bool symbol has no "active" state to trigger propagation.
	 * esp_kconfiglib warns in this case (IndirectSetNonBool fixture)
	 * and discards the rule at evaluation time; mirror the warning
	 * and rely on pass_apply_sets's bool-y check to ignore it.  If
	 * the type hasn't been declared yet (e.g. an unusual ordering
	 * that puts @c set before @c bool), skip the warning -- the
	 * canonical style always declares type first.
	 */
	if (sym->type != KS_UNKNOWN && sym->type != KS_BOOL) {
		const char *t = "";
		switch (sym->type) {
		case KS_INT:
			t = "int";
			break;
		case KS_HEX:
			t = "hex";
			break;
		case KS_STRING:
			t = "string";
			break;
		case KS_FLOAT:
			t = "float";
			break;
		default:
			break;
		}
		kc_ctx_notify(p->ctx,
			      "%s of type %s has '%s' option, which is "
			      "only supported for boolean symbols.",
			      sym->name, t,
			      kind == KP_SET ? "set" : "set default");
	}
}

/*
 * warning "TEXT" [ if EXPR ] NL
 *
 * ESP-IDF extension attached to a symbol: when the symbol ends up set
 * to a non-default value, the associated text is emitted to stderr at
 * config-generation time.  Recording the property here gives the
 * evaluator something to scan during its fixpoint pass; the actual
 * diagnostic is fired by kc_eval.
 */
static void parse_warning_line(struct kc_parser *p, struct ksym *sym)
{
	int line = p->lex.line;
	p_advance(p);
	if (p->lex.tok != KT_STR)
		kc_lex_die_unexpected(&p->lex, KT_STR);
	struct kprop *pr = prop_new(p, KP_WARNING, p->lex.path, line);
	pr->text = p_take_val(p);
	p_advance(p);
	pr->cond = parse_if_tail(p);
	sym_append_prop(sym, pr);
	p_expect(p, KT_NL);
}

/* range EXPR EXPR [ if EXPR ] NL */
static void parse_range_line(struct kc_parser *p, struct ksym *sym)
{
	int line = p->lex.line;
	p_advance(p);
	struct kprop *pr = prop_new(p, KP_RANGE, p->lex.path, line);
	pr->expr = parse_expr(p);
	pr->expr2 = parse_expr(p);
	pr->cond = parse_if_tail(p);
	sym_append_prop(sym, pr);
	p_expect(p, KT_NL);
}

/* help NL HELPTEXT */
static void parse_help_line(struct kc_parser *p, struct ksym *sym)
{
	int line = p->lex.line;
	p_advance(p); /* 'help' -> KT_NL */
	if (p->lex.tok != KT_NL)
		kc_lex_die_unexpected(&p->lex, KT_NL);
	/*
	 * Don't p_advance() past the NL here: that would tokenize the
	 * first help-body line, eating its leading whitespace and the
	 * first word.  kc_lex_read_help() expects the lexer to sit at
	 * the start of the first body line (l->pos), which is exactly
	 * where the last kc_lex_next() left it when returning KT_NL.
	 */
	kc_lex_read_help(&p->lex);
	struct kprop *pr = prop_new(p, KP_HELP, p->lex.path, line);
	pr->text = p_take_val(p);
	sym_append_prop(sym, pr);
	/* Consume the synthetic HELPTEXT token by lexing the next real
	 * token from l->pos (already sitting at the post-help line). */
	p_advance(p);
}

/* option NAME [ = STRING ] NL
 *   Only `env` is meaningful for config generation; record it verbatim. */
static void parse_option_line(struct kc_parser *p, struct ksym *sym)
{
	int line = p->lex.line;
	p_advance(p);
	if (p->lex.tok != KT_NAME)
		kc_lex_die_unexpected(&p->lex, KT_NAME);
	char *name = p_take_val(p);
	p_advance(p);

	char *value = NULL;
	if (p_accept(p, KT_EQ)) {
		if (p->lex.tok != KT_STR && p->lex.tok != KT_NAME)
			die("%s:%d: option value must be string or identifier",
			    p->lex.path, p->lex.line);
		value = p_take_val(p);
		p_advance(p);
	}

	if (strcmp(name, "env") == 0 && value) {
		struct kprop *pr = prop_new(p, KP_ENV, p->lex.path, line);
		pr->text = value;
		value = NULL;
		sym_append_prop(sym, pr);
	}
	/* Other option flavours (defconfig_list, modules, allnoconfig_y,
	 * ...) are accepted for grammar compatibility but produce no AST
	 * node -- none of them affect config generation output. */
	free(name);
	free(value);
	p_expect(p, KT_NL);
}

/* optional NL
 *   For choice groups only: marks the group as non-exclusive.  In
 *   Phase 1 we only record the grammar -- evaluator will act on it. */
static void parse_optional_line(struct kc_parser *p, struct ksym *sym)
{
	(void)sym;
	p_advance(p);
	p_expect(p, KT_NL);
}

static void parse_modules_line(struct kc_parser *p)
{
	p_advance(p);
	p_expect(p, KT_NL);
}

/* visible if EXPR NL   (on menus only, stored on the menu directly). */
static void parse_visible_line(struct kc_parser *p, struct kmenu *m)
{
	p_advance(p);
	p_expect(p, KT_IF);
	struct kexpr *e = parse_expr(p);
	p_expect(p, KT_NL);
	if (!m) {
		expr_free(e);
		return;
	}
	/*
	 * `visible if` on a config or choice is nonsensical in
	 * esp_kconfiglib and only legal on @c menu.  Emit the warning
	 * the python parser prints and DROP the expression -- attaching
	 * it to a non-KM_MENU node risks double-free paths in the AST
	 * cleanup, and the evaluator would ignore it anyway.
	 */
	if (m->kind != KM_MENU) {
		if (m->kind == KM_SYM && m->sym) {
			kc_ctx_notify(p->ctx,
				      "config %s has a \"visible if\" "
				      "option, which is not supported "
				      "for configs",
				      m->sym->name);
		} else if (m->kind == KM_CHOICE) {
			/* choice_sym's name carries a reserved
			 * `__choice:` prefix the parser adds so the
			 * symbol table can host both `choice FOO` and
			 * `config FOO` entries; strip it for display. */
			const char *n =
			    m->sym && m->sym->name ? m->sym->name : "";
			const char prefix[] = "__choice:";
			if (!strncmp(n, prefix, sizeof(prefix) - 1))
				n += sizeof(prefix) - 1;
			if (!*n)
				n = "(unnamed)";
			kc_ctx_notify(p->ctx,
				      "choice %s has a \"visible if\" "
				      "option, which is not supported "
				      "for choices",
				      n);
		}
		expr_free(e);
		return;
	}
	if (m->visible_if) {
		struct kexpr *n = expr_new(KE_AND);
		n->l = m->visible_if;
		n->r = e;
		m->visible_if = n;
	} else {
		m->visible_if = e;
	}
}

/* Dispatch a single prop line.  prop_sym / prop_menu describe where
 * the prop attaches; one may be NULL when that kind doesn't apply.
 * Returns 0 if the current token is not a recognised prop. */
static int parse_prop_line(struct kc_parser *p, struct ksym *prop_sym,
			   struct kmenu *prop_menu)
{
	int tok = p->lex.tok;

	if (!is_prop_tok(tok))
		return 0;

	switch (tok) {
	case KT_BOOL:
	case KT_TRISTATE:
	case KT_INT:
	case KT_HEX:
	case KT_STRING:
	case KT_FLOAT:
		if (prop_sym)
			parse_type_line(p, prop_sym);
		else
			die("%s:%d: type declaration outside config/choice",
			    p->lex.path, p->lex.line);
		return 1;
	case KT_DEF_BOOL:
	case KT_DEF_INT:
	case KT_DEF_HEX:
	case KT_DEF_STRING:
	case KT_DEF_TRISTATE:
		if (prop_sym)
			parse_deftype_line(p, prop_sym);
		else
			die("%s:%d: def_* outside config/choice", p->lex.path,
			    p->lex.line);
		return 1;
	case KT_PROMPT:
		if (prop_sym)
			parse_prompt_line(p, prop_sym);
		else
			die("%s:%d: prompt outside config/choice", p->lex.path,
			    p->lex.line);
		return 1;
	case KT_DEFAULT:
		if (prop_sym)
			parse_default_line(p, prop_sym);
		else
			die("%s:%d: default outside config/choice", p->lex.path,
			    p->lex.line);
		return 1;
	case KT_DEPENDS:
		parse_depends_line(p, prop_sym, prop_menu);
		return 1;
	case KT_SELECT:
	case KT_IMPLY:
		if (prop_sym)
			parse_select_line(p, prop_sym);
		else
			die("%s:%d: select/imply outside config/choice",
			    p->lex.path, p->lex.line);
		return 1;
	case KT_RANGE:
		if (prop_sym)
			parse_range_line(p, prop_sym);
		else
			die("%s:%d: range outside config/choice", p->lex.path,
			    p->lex.line);
		return 1;
	case KT_WARNING:
		if (prop_sym)
			parse_warning_line(p, prop_sym);
		else
			die("%s:%d: warning outside config/choice", p->lex.path,
			    p->lex.line);
		return 1;
	case KT_SET:
		if (prop_sym) {
			parse_set_line(p, prop_sym);
		} else {
			/*
			 * `set` outside a config / menuconfig / choice body
			 * (typically inside a bare @c menu) is meaningless
			 * -- there's no source symbol to gate on -- but
			 * python's esp_kconfiglib accepts the line and
			 * warns.  Mirror that: eat the tokens through the
			 * trailing NL and emit the matching stderr line the
			 * SetOnMenu fixture checks for.
			 */
			kc_ctx_notify(p->ctx, "'set' option is only valid for "
					      "config and menuconfig entries.");
			while (p->lex.tok != KT_NL && p->lex.tok != KT_EOF)
				p_advance(p);
			p_accept(p, KT_NL);
		}
		return 1;
	case KT_HELP:
		if (prop_sym)
			parse_help_line(p, prop_sym);
		else
			die("%s:%d: help outside config/choice", p->lex.path,
			    p->lex.line);
		return 1;
	case KT_OPTION:
		if (prop_sym)
			parse_option_line(p, prop_sym);
		else
			die("%s:%d: option outside config/choice", p->lex.path,
			    p->lex.line);
		return 1;
	case KT_OPTIONAL:
		parse_optional_line(p, prop_sym);
		return 1;
	case KT_MODULES:
		parse_modules_line(p);
		return 1;
	case KT_VISIBLE:
		parse_visible_line(p, prop_menu);
		return 1;
	}
	return 0;
}

/* ================================================================== */
/*  Statement parsers                                                 */
/* ================================================================== */

static void parse_block_body(struct kc_parser *p, struct kmenu *menu,
			     struct ksym *prop_sym, int end_tok);

static void parse_config_stmt(struct kc_parser *p, struct kmenu *parent)
{
	int line = p->lex.line;
	p_advance(p); /* config / menuconfig */
	if (p->lex.tok != KT_NAME)
		kc_lex_die_unexpected(&p->lex, KT_NAME);
	/*
	 * Track definition count for the Multiple-Definition diagnostic
	 * and scan the tail of the current line for a
	 * @c # ignore: @c multiple-definition marker.  The marker sets
	 * a sticky flag on the symbol; if any of the symbol's
	 * declarations carries it the warning is suppressed globally.
	 * The actual warning is emitted from a post-parse pass once we
	 * know the final n_defs / ignore_multidef state -- otherwise
	 * ordering would matter (a marker on the second declaration
	 * wouldn't back-suppress a warning already printed for the
	 * first).  The lexer strips comments before they reach us, so we
	 * peek at the raw buffer for the pragma between the NAME token
	 * and its trailing newline.
	 */
	struct ksym *sym = kc_sym_intern(p->ctx, p->lex.val);
	sym->n_defs++;
	if (!sym->decl_file) {
		sym->decl_file = kc_ctx_intern_file(p->ctx, p->lex.path);
		sym->decl_line = line;
	}
	const char *peek_start = p->lex.pos;
	p_advance(p);
	for (const char *q = peek_start; *q && *q != '\n'; q++) {
		if (*q != '#')
			continue;
		const char *r = q + 1;
		while (*r == ' ' || *r == '\t')
			r++;
		if (!strncmp(r, "ignore:", 7)) {
			r += 7;
			while (*r == ' ' || *r == '\t')
				r++;
			if (!strncmp(r, "multiple-definition",
				     strlen("multiple-definition")))
				sym->ignore_multidef = 1;
		}
		break;
	}
	p_expect(p, KT_NL);

	struct kmenu *m = menu_new(KM_SYM, p->lex.path, line);
	m->sym = sym;
	menu_append(parent, m);

	/* Prop-body loop.  Stamp every prop created during this declaration
	 * with @p m so the evaluator can AND in this specific declaration's
	 * ctx_dep when firing selects / imply / set / defaults -- see
	 * kc_ast.h::kprop::menu. */
	struct kmenu *save = p->cur_decl_menu;
	p->cur_decl_menu = m;
	while (is_prop_tok(p->lex.tok))
		parse_prop_line(p, sym, m);
	p->cur_decl_menu = save;
}

static void parse_menu_stmt(struct kc_parser *p, struct kmenu *parent)
{
	int line = p->lex.line;
	p_advance(p);
	if (p->lex.tok != KT_STR)
		kc_lex_die_unexpected(&p->lex, KT_STR);
	char *prompt = p_take_val(p);
	p_advance(p);
	p_expect(p, KT_NL);

	struct kmenu *m = menu_new(KM_MENU, p->lex.path, line);
	m->prompt = prompt;
	menu_append(parent, m);

	/* Menu head can carry depends-on / visible-if / help lines before
	 * children.  Menu-level help is documentation for the menu itself;
	 * the three config-generation output formats (config / header /
	 * cmake) don't emit it, so we parse and discard. */
	for (;;) {
		int tok = p->lex.tok;
		if (tok == KT_DEPENDS || tok == KT_VISIBLE) {
			parse_prop_line(p, NULL, m);
		} else if (tok == KT_HELP) {
			p_advance(p);
			if (p->lex.tok != KT_NL)
				kc_lex_die_unexpected(&p->lex, KT_NL);
			kc_lex_read_help(&p->lex);
			p_advance(p);
		} else {
			break;
		}
	}

	parse_block_body(p, m, NULL, KT_ENDMENU);
	p_expect(p, KT_ENDMENU);
	p_eat_nl(p);
}

static void parse_choice_stmt(struct kc_parser *p, struct kmenu *parent)
{
	int line = p->lex.line;
	p_advance(p);

	struct ksym *choice_sym;
	if (p->lex.tok == KT_NAME) {
		/*
		 * ESP-IDF's Kconfig uses `choice NAME` + `config NAME` to
		 * declare two distinct symbols that happen to share a
		 * source-level name -- the choice groups bool members
		 * under NAME, a separate `config NAME` (int) derives its
		 * value from which member is selected.  Our symbol table
		 * is name-keyed, so store the choice under a reserved
		 * "__choice:NAME" namespace; `config NAME` then interns
		 * a fresh NAME.  The @c choice_parent linkage on members
		 * points at the interned choice-sym, not its source-level
		 * name, so this rename is transparent to evaluation.
		 */
		char key[256];
		snprintf(key, sizeof(key), "__choice:%s", p->lex.val);
		choice_sym = kc_sym_intern(p->ctx, key);
		p_advance(p);
	} else {
		/* Anonymous choice: mint a unique name. */
		static int anon_id;
		char buf[32];
		snprintf(buf, sizeof(buf), "__anon_choice_%d", anon_id++);
		choice_sym = kc_sym_intern(p->ctx, buf);
	}
	choice_sym->is_choice = 1;
	if (!choice_sym->decl_file) {
		choice_sym->decl_file = kc_ctx_intern_file(p->ctx, p->lex.path);
		choice_sym->decl_line = line;
	}
	p_expect(p, KT_NL);

	struct kmenu *m = menu_new(KM_CHOICE, p->lex.path, line);
	m->sym = choice_sym;
	menu_append(parent, m);

	/*
	 * Loose props that precede members attach to the choice sym.
	 * Flip @c in_choice while the body runs so nested @c default
	 * properties can emit the DefaultInChoice warning.
	 *
	 * Install @p m as the current declaration menu so props parsed
	 * directly on the choice (rather than on one of its members) are
	 * stamped with the KM_CHOICE node -- matching the fact that
	 * @c select / @c imply / @c default on a choice really belong to
	 * the choice's own block.  Nested @c parse_config_stmt saves and
	 * restores this cell for each member.
	 */
	struct kmenu *save_decl = p->cur_decl_menu;
	p->cur_decl_menu = m;
	p->in_choice++;
	parse_block_body(p, m, choice_sym, KT_ENDCHOICE);
	p->in_choice--;
	p->cur_decl_menu = save_decl;
	p_expect(p, KT_ENDCHOICE);
	p_eat_nl(p);
}

static void parse_if_stmt(struct kc_parser *p, struct kmenu *parent)
{
	int line = p->lex.line;
	p_advance(p);
	struct kexpr *cond = parse_expr(p);
	p_expect(p, KT_NL);

	struct kmenu *m = menu_new(KM_IF, p->lex.path, line);
	m->dep = cond;
	menu_append(parent, m);

	parse_block_body(p, m, NULL, KT_ENDIF);
	p_expect(p, KT_ENDIF);
	p_eat_nl(p);
}

static void parse_comment_stmt(struct kc_parser *p, struct kmenu *parent)
{
	int line = p->lex.line;
	p_advance(p);
	if (p->lex.tok != KT_STR)
		kc_lex_die_unexpected(&p->lex, KT_STR);
	char *prompt = p_take_val(p);
	p_advance(p);
	p_expect(p, KT_NL);

	struct kmenu *m = menu_new(KM_COMMENT, p->lex.path, line);
	m->prompt = prompt;
	menu_append(parent, m);

	/* A comment may carry depends-on / visible-if but not much else. */
	while (p->lex.tok == KT_DEPENDS || p->lex.tok == KT_VISIBLE)
		parse_prop_line(p, NULL, m);
}

/*
 * Resolve a source path for the rsource / orsource variants: if @p
 * raw is absolute or starts with "~", return a copy unchanged;
 * otherwise prepend the directory of @p cur_path.  Returns a newly
 * malloc'd string owned by the caller.
 */
static char *resolve_rsource_path(const char *cur_path, const char *raw)
{
	if (raw[0] == '/' || raw[0] == '~')
		return sbuf_strdup(raw);
#ifdef _WIN32
	if (raw[0] && raw[1] == ':') /* drive-letter absolute */
		return sbuf_strdup(raw);
#endif
	const char *slash = strrchr(cur_path, '/');
#ifdef _WIN32
	const char *bslash = strrchr(cur_path, '\\');
	if (bslash && (!slash || bslash > slash))
		slash = bslash;
#endif
	if (!slash)
		return sbuf_strdup(raw);

	struct sbuf sb = SBUF_INIT;
	sbuf_add(&sb, cur_path, (size_t)(slash - cur_path + 1));
	sbuf_addstr(&sb, raw);
	return sbuf_detach(&sb);
}

static void parse_source_stmt(struct kc_parser *p)
{
	int variant = p->lex.tok;
	p_advance(p); /* past source variant */
	if (p->lex.tok != KT_STR && p->lex.tok != KT_NAME)
		die("%s:%d: source expects a path", p->lex.path, p->lex.line);

	char *raw = p_take_val(p);
	p_advance(p); /* past the path string */
	if (p->lex.tok != KT_NL)
		kc_lex_die_unexpected(&p->lex, KT_NL);
	/* Do NOT advance past the NL yet: we want the subsequent
	 * kc_lex_next() to read the FIRST token of the included file
	 * (if we push one), not the next token of the parent. */

	int optional = (variant == KT_OSOURCE || variant == KT_ORSOURCE);
	int relative = (variant == KT_RSOURCE || variant == KT_ORSOURCE);

	/*
	 * Expand bare @c $VAR references in the source path.  The lexer
	 * keeps them literal inside quoted string values (to match python
	 * kconfgen on string defaults like `default "$IDF_INIT_VERSION"`),
	 * but source paths routinely interpolate env vars via `$IDF_TARGET`
	 * / `$COMPONENT_KCONFIGS_SOURCE_FILE` -- handle those here.
	 */
	char *expanded = kc_lex_expand_bare_vars(&p->lex, raw);
	free(raw);
	raw = expanded;

	char *resolved = relative ? resolve_rsource_path(p->lex.path, raw)
				  : sbuf_strdup(raw);

	/* Empty path (after env expansion) -- skip silently; ESP-IDF uses
	 * `source "$COMPONENT_KCONFIGS_SOURCE_FILE"` and if the env var
	 * is unset we don't want to die(). */
	if (!*resolved) {
		free(raw);
		free(resolved);
		p_advance(p); /* consume the NL */
		return;
	}

	/* The lexer stores only the pointer, not a copy, so the path must
	 * outlive the frame.  Intern it in the context. */
	const char *interned = kc_ctx_intern_file(p->ctx, resolved);
	free(resolved);

	int r = kc_lex_push_file(&p->lex, interned, optional);
	(void)r; /* optional path may return -1 silently */

	free(raw);
	p_advance(
	    p); /* consume NL -> first token of child (or next of parent) */
}

static void parse_mainmenu_stmt(struct kc_parser *p, struct kmenu *parent)
{
	p_advance(p);
	if (p->lex.tok != KT_STR)
		kc_lex_die_unexpected(&p->lex, KT_STR);
	free(parent->prompt);
	parent->prompt = p_take_val(p);
	p_advance(p);
	p_expect(p, KT_NL);
}

/* ================================================================== */
/*  Block body                                                        */
/* ================================================================== */

/*
 * Parse statements until we encounter @p end_tok (or EOF if end_tok is
 * KT_EOF).  Within a block, top-level prop lines (those not preceded by
 * a config/choice) are accepted only when @p prop_sym is non-NULL --
 * that's how choice bodies collect loose defaults/prompts onto the
 * choice symbol.
 */
static void parse_block_body(struct kc_parser *p, struct kmenu *menu,
			     struct ksym *prop_sym, int end_tok)
{
	for (;;) {
		p_skip_nl(p);
		int tok = p->lex.tok;

		if (tok == end_tok)
			return;
		if (tok == KT_EOF)
			return;

		switch (tok) {
		case KT_CONFIG:
		case KT_MENUCONFIG:
			parse_config_stmt(p, menu);
			continue;
		case KT_MENU:
			parse_menu_stmt(p, menu);
			continue;
		case KT_CHOICE:
			parse_choice_stmt(p, menu);
			continue;
		case KT_IF:
			parse_if_stmt(p, menu);
			continue;
		case KT_COMMENT:
			parse_comment_stmt(p, menu);
			continue;
		case KT_SOURCE:
		case KT_RSOURCE:
		case KT_OSOURCE:
		case KT_ORSOURCE:
			parse_source_stmt(p);
			continue;
		case KT_MAINMENU:
			if (menu->kind == KM_ROOT)
				parse_mainmenu_stmt(p, menu);
			else
				die("%s:%d: 'mainmenu' only allowed at top "
				    "level",
				    p->lex.path, p->lex.line);
			continue;
		}

		/*
		 * Preset-variable assignment at statement position:
		 * `NAME = VALUE` or `NAME := VALUE`.  Only valid outside a
		 * choice body (choice bodies allow loose prop lines, which
		 * take priority below).  A bareword-followed-by-equals that
		 * isn't a variable assignment is a syntax error, consistent
		 * with Python.
		 */
		/*
		 * A bareword at statement position outside a choice body
		 * can only start a preset-variable assignment:
		 *   NAME = VALUE      (recursive form)
		 *   NAME := VALUE     (immediate form)
		 * esp_kconfiglib supports only literal VALUEs here, so the
		 * two forms behave identically.  Take ownership of the NAME
		 * before advancing so we can still name it in any error.
		 */
		if (tok == KT_NAME && !prop_sym) {
			char *name = p_take_val(p);
			p_advance(p);
			int nxt = p->lex.tok;
			if (nxt != KT_EQ && nxt != KT_COLON_EQ) {
				die("%s:%d: unexpected %s (bareword at "
				    "statement position -- did you mean "
				    "a preset-variable assignment `%s "
				    "= VALUE`?)",
				    p->lex.path, p->lex.line, kc_tok_name(nxt),
				    name);
			}
			p_advance(p); /* past '=' / ':=' */
			char *value;
			if (p->lex.tok == KT_STR || p->lex.tok == KT_NAME) {
				value = p_take_val(p);
				p_advance(p);
			} else if (p->lex.tok == KT_NL) {
				value = sbuf_strdup("");
			} else {
				die("%s:%d: expected value after '%s =', "
				    "got %s",
				    p->lex.path, p->lex.line, name,
				    kc_tok_name(p->lex.tok));
			}
			p_expect(p, KT_NL);
			/* Repeat-assignment replaces the earlier value;
			 * free the old one so we don't leak. */
			char *old = smap_get(&p->ctx->vars, name);
			free(old);
			smap_put(&p->ctx->vars, name, value);
			free(name);
			continue;
		}

		/* Loose prop line (choice bodies only).  Pass the enclosing
		 * menu (the KM_CHOICE node in practice) as prop_menu so
		 * `visible if` lands on the right node and its warning
		 * hook sees the KM_CHOICE kind. */
		if (prop_sym && parse_prop_line(p, prop_sym, menu))
			continue;

		/*
		 * `set` directly inside a menu body (no surrounding
		 * config / menuconfig / choice) is nonsensical -- there
		 * is no source symbol to gate on -- but esp_kconfiglib
		 * accepts and warns rather than failing hard so downstream
		 * Kconfig authors can spot the mistake without breaking
		 * the build.  Swallow the tokens up through the trailing
		 * newline and emit the SetOnMenu diagnostic the fixture
		 * grep-matches.
		 */
		if (tok == KT_SET) {
			kc_ctx_notify(p->ctx, "'set' option is only valid for "
					      "config and menuconfig entries.");
			while (p->lex.tok != KT_NL && p->lex.tok != KT_EOF)
				p_advance(p);
			p_accept(p, KT_NL);
			continue;
		}

		die("%s:%d: unexpected %s", p->lex.path, p->lex.line,
		    kc_tok_name(tok));
	}
}

/* ================================================================== */
/*  Entry point                                                       */
/* ================================================================== */

/* ================================================================== */
/*  Implicit-menuconfig hoisting                                      */
/* ================================================================== */

/*
 * True when expression @p e's truth value requires @p sym to be y
 * (or non-zero).  Reimplements esp_kconfiglib's @c _expr_depends_on
 * so we can build the same menu tree shape the Python loader does.
 * Matches on:
 *   - bare symref to @p sym
 *   - @c sym = y, @c y = sym, @c sym != n, @c n != sym
 *   - AND where either side depends on @p sym
 */
static int finalize_expr_depends_on(const struct kexpr *e,
				    const struct ksym *sym)
{
	if (!e || !sym)
		return 0;
	if (e->op == KE_SYMREF)
		return e->sym == sym;
	if (e->op == KE_EQ || e->op == KE_NE) {
		/* Normalise so @p sym is on the left side; only @p b is
		 * inspected below, so we just pick whichever side isn't
		 * @p sym. */
		const struct kexpr *a = e->l, *b = e->r;
		if (b && b->op == KE_SYMREF && b->sym == sym) {
			b = e->l;
		} else if (!(a && a->op == KE_SYMREF && a->sym == sym)) {
			return 0;
		}
		/* @p sym is on one side; b must be the constant y (for EQ) /
		 * n (for NE). */
		if (!b)
			return 0;
		const char *name = NULL;
		if (b->op == KE_SYMREF && b->sym)
			name = b->sym->name;
		else if (b->op == KE_LITERAL)
			name = b->str;
		if (!name)
			return 0;
		if (e->op == KE_EQ)
			return !strcmp(name, "y") || !strcmp(name, "yes");
		return !strcmp(name, "n") || !strcmp(name, "no");
	}
	if (e->op == KE_AND)
		return finalize_expr_depends_on(e->l, sym) ||
		       finalize_expr_depends_on(e->r, sym);
	return 0;
}

/*
 * Return 1 iff @p next has an automatic-menu dependency on
 * @p prev's symbol.  Python's @c _auto_menu_dep checks
 * `node2.prompt[1] if node2.prompt else node2.dep`, but kconfiglib
 * stores the fully-propagated dependency chain on prompt.cond (the
 * original `if COND` guard AND'd with the symbol's own @c depends on
 * chain).  Ice keeps those two separate (KP_PROMPT.cond vs
 * KP_DEPENDS.expr), so mirror python's effective check by scanning
 * both: hoist if the target appears in @em any of the per-prop
 * conds or in the KP_DEPENDS chain -- the conjunction / disjunction
 * distinction doesn't change the set of symbols required to be y.
 */
static int auto_menu_dep(const struct kmenu *prev, const struct kmenu *next)
{
	if (!prev || prev->kind != KM_SYM || !prev->sym)
		return 0;

	/*
	 * Symbols and choices keep their @c depends on chain as
	 * KP_DEPENDS properties on the symbol (choice's sym is the
	 * interned `__choice:NAME` placeholder).  Menus / comments put
	 * @c depends on into @p m->dep directly.  Check both shapes.
	 */
	if ((next->kind == KM_SYM || next->kind == KM_CHOICE) && next->sym) {
		for (const struct kprop *p = next->sym->props; p; p = p->next) {
			if (p->kind == KP_PROMPT && p->cond &&
			    finalize_expr_depends_on(p->cond, prev->sym))
				return 1;
			if (p->kind == KP_DEPENDS && p->expr &&
			    finalize_expr_depends_on(p->expr, prev->sym))
				return 1;
		}
		return 0;
	}

	if ((next->kind == KM_MENU || next->kind == KM_COMMENT) && next->dep)
		return finalize_expr_depends_on(next->dep, prev->sym);

	return 0;
}

/*
 * Transplant the trailing sibling chain of @p node -- while each
 * subsequent sibling has an "automatic menu dependency" on @p node's
 * symbol, splice it under @p node as a child instead.  Matches
 * esp_kconfiglib's _finalize_node() hoisting step so ice's
 * kconfig_menus.json mirrors python's tree shape for the common
 * pattern of `config X` followed by `config Y depends on X`.
 *
 * Because the hoisted siblings still carry their @c depends on X
 * property, evaluation semantics are unaffected -- only the walked
 * menu tree changes shape.
 */
static void finalize_node(struct kmenu *node)
{
	if (!node)
		return;

	if (node->kind == KM_SYM) {
		/* Collect consecutive dependent siblings and re-parent them
		 * as children.  Recurse into each before hoisting so nested
		 * hoists work. */
		struct kmenu *cur = node;
		while (cur->next && auto_menu_dep(node, cur->next)) {
			finalize_node(cur->next);
			cur = cur->next;
			cur->parent = node;
		}
		if (cur != node) {
			/*
			 * `node->next` .. `cur` become node's children.
			 * The original sibling link continues past `cur`.
			 */
			node->children = node->next;
			node->tail = cur;
			node->next = cur->next;
			cur->next = NULL;
		}
	} else if (node->children) {
		for (struct kmenu *c = node->children; c; c = c->next)
			finalize_node(c);
	}
}

/**
 * @brief Parse a Kconfig file into @p ctx.
 *
 * @p ctx must have been initialised with kc_ctx_init().  Subsequent
 * parses append to the same context (the root menu accumulates).
 * Dies on syntax errors.  @p env is the optional NAME=VAL table for
 * $(VAR) interpolation; may be NULL.
 */
void kc_parse_file(struct kc_ctx *ctx, const char *path, const char *const *env)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0)
		die_errno("cannot read '%s'", path);

	const char *interned = kc_ctx_intern_file(ctx, path);
	if (!ctx->root_file)
		ctx->root_file = sbuf_strdup(path);
	struct kc_parser p = {.ctx = ctx};
	kc_lex_open(&p.lex, sb.buf, interned, env);
	p.lex.vars = &ctx->vars;
	p_advance(&p); /* prime first token */

	parse_block_body(&p, ctx->root, NULL, KT_EOF);
	if (p.lex.tok != KT_EOF)
		die("%s:%d: trailing input (tok=%s)", p.lex.path, p.lex.line,
		    kc_tok_name(p.lex.tok));

	kc_lex_close(&p.lex);
	sbuf_release(&sb);

	/*
	 * esp_kconfiglib (v2 parser) rejects a root Kconfig that has
	 * content but no `mainmenu "..."` declaration.  A truly-empty
	 * file still passes -- Empty.in exercises that path -- so only
	 * fire when ctx->root accumulated children.  The @c prompt
	 * field is set by parse_mainmenu_stmt; its absence alongside
	 * child menus means we parsed real statements without seeing
	 * the required mainmenu header.
	 */
	if (ctx->root_file && !strcmp(path, ctx->root_file) &&
	    ctx->root->children && !ctx->root->prompt) {
		die("%s: missing mainmenu", path);
	}

	/*
	 * Record the source-tree root used for file-path relativization
	 * in kconfig_menus.json id slugs.  Matches python kconfiglib's
	 * @c $srctree env var, with @c getcwd() as the fallback.  The
	 * stored value always ends with a trailing slash so later prefix
	 * checks are a plain @c strncmp.
	 */
	if (!ctx->srctree) {
		const char *s = getenv("srctree");
		char cwd[4096];
		if (!s || !*s) {
			if (getcwd(cwd, sizeof(cwd)))
				s = cwd;
		}
		if (s && *s) {
			size_t n = strlen(s);
			int has_slash = (n && s[n - 1] == '/');
			ctx->srctree = calloc(1, n + (has_slash ? 1 : 2));
			if (!ctx->srctree)
				die_errno("calloc");
			memcpy(ctx->srctree, s, n);
			if (!has_slash)
				ctx->srctree[n] = '/';
		}
	}

	/*
	 * Post-parse: apply python kconfiglib's implicit-menuconfig
	 * hoisting.  Done here so subsequent evaluation / output all
	 * see the final tree shape.
	 */
	finalize_node(ctx->root);

	/*
	 * Multiple-Definition diagnostic: emit one notification per symbol
	 * that was declared more than once and does not carry a
	 * `# ignore: multiple-definition` pragma on any of its
	 * declarations.  Deferring the emit until here means an ignore
	 * marker on a LATER declaration retroactively suppresses the
	 * warning for an earlier duplicate, matching esp_kconfiglib's
	 * behaviour in IgnorePragma.in (pragma on the first definition
	 * suppresses the warning for the duplicate).
	 */
	const char *key;
	void *val;
	size_t it = 0;
	while (smap_iter(&ctx->symtab, &it, &key, &val)) {
		struct ksym *s = val;
		if (s->n_defs > 1 && !s->ignore_multidef && !s->is_choice)
			kc_ctx_notify(ctx, "Multiple definitions of %s",
				      s->name);
	}
}

/* ================================================================== */
/*  AST dump                                                          */
/* ================================================================== */

static void dump_indent(int n)
{
	for (int i = 0; i < n; i++)
		fputs("  ", stdout);
}

static void dump_expr(const struct kexpr *e)
{
	if (!e) {
		fputs("(null)", stdout);
		return;
	}
	switch (e->op) {
	case KE_LITERAL:
		printf("\"%s\"", e->str ? e->str : "");
		return;
	case KE_SYMREF:
		printf("%s", e->sym ? e->sym->name : "?");
		return;
	case KE_NOT:
		fputs("!", stdout);
		dump_expr(e->l);
		return;
	case KE_AND:
	case KE_OR: {
		const char *op = (e->op == KE_AND) ? " && " : " || ";
		fputs("(", stdout);
		dump_expr(e->l);
		fputs(op, stdout);
		dump_expr(e->r);
		fputs(")", stdout);
		return;
	}
	case KE_EQ:
	case KE_NE:
	case KE_LT:
	case KE_LE:
	case KE_GT:
	case KE_GE: {
		const char *op;
		switch (e->op) {
		case KE_EQ:
			op = " = ";
			break;
		case KE_NE:
			op = " != ";
			break;
		case KE_LT:
			op = " < ";
			break;
		case KE_LE:
			op = " <= ";
			break;
		case KE_GT:
			op = " > ";
			break;
		case KE_GE:
			op = " >= ";
			break;
		default:
			op = " ? ";
			break;
		}
		dump_expr(e->l);
		fputs(op, stdout);
		dump_expr(e->r);
		return;
	}
	}
}

static const char *ksym_type_name(enum ksym_type t)
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

static const char *kprop_kind_name(enum kprop_kind k)
{
	switch (k) {
	case KP_PROMPT:
		return "prompt";
	case KP_DEFAULT:
		return "default";
	case KP_SELECT:
		return "select";
	case KP_IMPLY:
		return "imply";
	case KP_RANGE:
		return "range";
	case KP_HELP:
		return "help";
	case KP_ENV:
		return "env";
	case KP_DEPENDS:
		return "depends";
	case KP_VISIBLE:
		return "visible";
	case KP_WARNING:
		return "warning";
	case KP_SET:
		return "set";
	case KP_SET_DEFAULT:
		return "set default";
	}
	return "?";
}

static void dump_props(const struct ksym *sym, int indent)
{
	for (const struct kprop *p = sym->props; p; p = p->next) {
		dump_indent(indent);
		printf(".%s", kprop_kind_name(p->kind));
		if (p->text) {
			/* Truncate long help bodies for readability. */
			if (p->kind == KP_HELP) {
				int n = 0;
				for (const char *q = p->text; *q && n < 40;
				     q++, n++)
					;
				printf(" \"%.*s%s\"", n, p->text,
				       p->text[n] ? "..." : "");
			} else {
				printf(" \"%s\"", p->text);
			}
		}
		if (p->expr) {
			fputs(" expr=", stdout);
			dump_expr(p->expr);
		}
		if (p->expr2) {
			fputs(" expr2=", stdout);
			dump_expr(p->expr2);
		}
		if (p->cond) {
			fputs(" if ", stdout);
			dump_expr(p->cond);
		}
		fputs("\n", stdout);
	}
}

static void dump_menu(const struct kmenu *m, int indent)
{
	dump_indent(indent);
	switch (m->kind) {
	case KM_ROOT:
		printf("ROOT");
		if (m->prompt)
			printf(" mainmenu=\"%s\"", m->prompt);
		fputs("\n", stdout);
		break;
	case KM_MENU:
		printf("MENU \"%s\"", m->prompt ? m->prompt : "");
		if (m->dep) {
			fputs(" depends=", stdout);
			dump_expr(m->dep);
		}
		if (m->visible_if) {
			fputs(" visible_if=", stdout);
			dump_expr(m->visible_if);
		}
		fputs("\n", stdout);
		break;
	case KM_CHOICE:
		printf("CHOICE %s", m->sym ? m->sym->name : "(anon)");
		fputs("\n", stdout);
		if (m->sym)
			dump_props(m->sym, indent + 1);
		break;
	case KM_SYM:
		if (m->sym)
			printf("CONFIG %s : %s\n", m->sym->name,
			       ksym_type_name(m->sym->type));
		if (m->sym)
			dump_props(m->sym, indent + 1);
		break;
	case KM_COMMENT:
		printf("COMMENT \"%s\"\n", m->prompt ? m->prompt : "");
		break;
	case KM_IF:
		fputs("IF ", stdout);
		dump_expr(m->dep);
		fputs("\n", stdout);
		break;
	}
	for (const struct kmenu *c = m->children; c; c = c->next)
		dump_menu(c, indent + 1);
}

void kc_ast_dump(const struct kc_ctx *ctx)
{
	if (ctx->root)
		dump_menu(ctx->root, 0);
}
