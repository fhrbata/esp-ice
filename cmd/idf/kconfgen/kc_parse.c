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

static struct kprop *prop_new(enum kprop_kind kind, const char *src_file,
			      int src_line)
{
	struct kprop *p = calloc(1, sizeof(*p));
	if (!p)
		die_errno("calloc");
	p->kind = kind;
	p->src_file = src_file;
	p->src_line = src_line;
	return p;
}

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
}

/* ================================================================== */
/*  Parser state + helpers                                            */
/* ================================================================== */

struct kc_parser {
	struct kc_ctx *ctx;
	struct kc_lexer lex;
};

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
		return 1;
	}
	return 0;
}

static enum ksym_type tok_to_ksym_type(int tok)
{
	switch (tok) {
	case KT_BOOL:
		return KS_BOOL;
	case KT_TRISTATE:
		return KS_BOOL; /* ESP has no tristate. */
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
		struct kprop *pr = prop_new(KP_PROMPT, p->lex.path, line);
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
		t = KS_BOOL;
		break;
	case KT_DEF_TRISTATE:
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

	struct kprop *pr = prop_new(KP_DEFAULT, p->lex.path, line);
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
	struct kprop *pr = prop_new(KP_PROMPT, p->lex.path, line);
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
	struct kprop *pr = prop_new(KP_DEFAULT, p->lex.path, line);
	pr->expr = parse_expr(p);
	pr->cond = parse_if_tail(p);
	sym_append_prop(sym, pr);
	p_expect(p, KT_NL);
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

	if (sym) {
		/* Attach to the symbol as a KP_DEPENDS property so the
		 * evaluator can combine it with menu context later. */
		struct kprop *pr = prop_new(KP_DEPENDS, p->lex.path, line);
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
	struct kprop *pr = prop_new(kind, p->lex.path, line);
	pr->expr = expr_new(KE_SYMREF);
	pr->expr->sym = kc_sym_intern(p->ctx, p->lex.val);
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
	struct kprop *pr = prop_new(KP_RANGE, p->lex.path, line);
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
	struct kprop *pr = prop_new(KP_HELP, p->lex.path, line);
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
		struct kprop *pr = prop_new(KP_ENV, p->lex.path, line);
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
	struct ksym *sym = kc_sym_intern(p->ctx, p->lex.val);
	p_advance(p);
	p_expect(p, KT_NL);

	struct kmenu *m = menu_new(KM_SYM, p->lex.path, line);
	m->sym = sym;
	menu_append(parent, m);

	/* Prop-body loop. */
	while (is_prop_tok(p->lex.tok))
		parse_prop_line(p, sym, m);
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
		choice_sym = kc_sym_intern(p->ctx, p->lex.val);
		p_advance(p);
	} else {
		/* Anonymous choice: mint a unique name. */
		static int anon_id;
		char buf[32];
		snprintf(buf, sizeof(buf), "__anon_choice_%d", anon_id++);
		choice_sym = kc_sym_intern(p->ctx, buf);
	}
	choice_sym->is_choice = 1;
	p_expect(p, KT_NL);

	struct kmenu *m = menu_new(KM_CHOICE, p->lex.path, line);
	m->sym = choice_sym;
	menu_append(parent, m);

	/* Loose props that precede members attach to the choice sym. */
	parse_block_body(p, m, choice_sym, KT_ENDCHOICE);
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

		/* Loose prop line (choice bodies only). */
		if (prop_sym && parse_prop_line(p, prop_sym, NULL))
			continue;

		die("%s:%d: unexpected %s", p->lex.path, p->lex.line,
		    kc_tok_name(tok));
	}
}

/* ================================================================== */
/*  Entry point                                                       */
/* ================================================================== */

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
	struct kc_parser p = {.ctx = ctx};
	kc_lex_open(&p.lex, sb.buf, interned, env);
	p_advance(&p); /* prime first token */

	parse_block_body(&p, ctx->root, NULL, KT_EOF);
	if (p.lex.tok != KT_EOF)
		die("%s:%d: trailing input (tok=%s)", p.lex.path, p.lex.line,
		    kc_tok_name(p.lex.tok));

	kc_lex_close(&p.lex);
	sbuf_release(&sb);
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
