/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/ldgen/sdkconfig.c
 * @brief Sdkconfig reader + .lf conditional evaluator.
 */
#include "sdkconfig.h"
#include "ice.h"

/* -------------------------- parser -------------------------------- */

static const char *skip_ws(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

static void put(struct sdkconfig *s, const char *name, const char *value)
{
	for (int i = 0; i < s->nr; i++) {
		if (!strcmp(s->v[i].name, name)) {
			free(s->v[i].value);
			s->v[i].value = sbuf_strdup(value);
			return;
		}
	}
	ALLOC_GROW(s->v, s->nr + 1, s->alloc);
	s->v[s->nr].name = sbuf_strdup(name);
	s->v[s->nr].value = sbuf_strdup(value);
	s->nr++;
}

static const char *lookup(const struct sdkconfig *s, const char *name)
{
	for (int i = 0; i < s->nr; i++)
		if (!strcmp(s->v[i].name, name))
			return s->v[i].value;
	return NULL;
}

void sdkconfig_load(struct sdkconfig *s, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0)
		die_errno("cannot read '%s'", path);

	size_t pos = 0;
	char *line;
	while ((line = sbuf_getline(sb.buf, sb.len, &pos)) != NULL) {
		const char *p = skip_ws(line);
		if (!*p)
			continue;
		/* Skip comment unless it's the "# CONFIG_X is not set" form. */
		if (*p == '#') {
			p++;
			p = skip_ws(p);
			if (strncmp(p, "CONFIG_", 7) != 0)
				continue;
			p += 7;
			const char *name = p;
			while (*p && *p != ' ' && *p != '\t')
				p++;
			size_t nl = p - name;
			p = skip_ws(p);
			if (strncmp(p, "is not set", 10) != 0)
				continue;
			char *n = sbuf_strndup(name, nl);
			put(s, n, "n");
			free(n);
			continue;
		}
		if (strncmp(p, "CONFIG_", 7) != 0)
			continue;
		p += 7;
		const char *name = p;
		while (*p && *p != '=')
			p++;
		if (*p != '=')
			continue;
		size_t nl = p - name;
		p++;
		/* Strip surrounding quotes from string values. */
		const char *val = p;
		size_t vl = strlen(val);
		if (vl >= 2 && val[0] == '"' && val[vl - 1] == '"') {
			val++;
			vl -= 2;
		}
		char *n = sbuf_strndup(name, nl);
		char *v = sbuf_strndup(val, vl);
		put(s, n, v);
		free(n);
		free(v);
	}

	sbuf_release(&sb);
}

void sdkconfig_free(struct sdkconfig *s)
{
	for (int i = 0; i < s->nr; i++) {
		free(s->v[i].name);
		free(s->v[i].value);
	}
	free(s->v);
	memset(s, 0, sizeof(*s));
}

/* -------------------------- expression evaluator ------------------ */

/*
 * Recursive-descent over the subset that .lf files actually use.
 *
 * Grammar:
 *   or_expr   := and_expr { '||' and_expr }
 *   and_expr  := not_expr { '&&' not_expr }
 *   not_expr  := '!' not_expr | cmp_expr
 *   cmp_expr  := atom [ ('='|'!=') atom ]
 *   atom      := IDENT | NUMBER | STRING | '(' or_expr ')'
 *
 * A bare atom resolves to truthy (nonempty, non-"n", non-zero).
 * An equality compares stringified values.
 */

struct lexer {
	const char *src;
	const char *p;
	/* current token: */
	enum tok {
		T_END,
		T_LPAREN,
		T_RPAREN,
		T_NOT,
		T_AND,
		T_OR,
		T_EQ,
		T_NE,
		T_LT,
		T_LE,
		T_GT,
		T_GE,
		T_IDENT,
		T_NUM,
		T_STR,
	} tok;
	char *lexeme; /* for T_IDENT / T_NUM / T_STR -- heap-allocated */
};

static void lex_next(struct lexer *l)
{
	free(l->lexeme);
	l->lexeme = NULL;
	const char *p = l->p;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p) {
		l->tok = T_END;
		l->p = p;
		return;
	}
	if (*p == '(') {
		l->tok = T_LPAREN;
		l->p = p + 1;
		return;
	}
	if (*p == ')') {
		l->tok = T_RPAREN;
		l->p = p + 1;
		return;
	}
	if (*p == '!' && p[1] == '=') {
		l->tok = T_NE;
		l->p = p + 2;
		return;
	}
	if (*p == '!') {
		l->tok = T_NOT;
		l->p = p + 1;
		return;
	}
	if (*p == '&' && p[1] == '&') {
		l->tok = T_AND;
		l->p = p + 2;
		return;
	}
	if (*p == '|' && p[1] == '|') {
		l->tok = T_OR;
		l->p = p + 2;
		return;
	}
	if (*p == '=') {
		l->tok = T_EQ;
		l->p = p + 1;
		return;
	}
	if (*p == '<' && p[1] == '=') {
		l->tok = T_LE;
		l->p = p + 2;
		return;
	}
	if (*p == '>' && p[1] == '=') {
		l->tok = T_GE;
		l->p = p + 2;
		return;
	}
	if (*p == '<') {
		l->tok = T_LT;
		l->p = p + 1;
		return;
	}
	if (*p == '>') {
		l->tok = T_GT;
		l->p = p + 1;
		return;
	}
	if (*p == '"') {
		const char *q = ++p;
		while (*q && *q != '"')
			q++;
		l->lexeme = sbuf_strndup(p, q - p);
		l->tok = T_STR;
		l->p = *q ? q + 1 : q;
		return;
	}
	if ((*p >= '0' && *p <= '9') ||
	    (*p == '-' && p[1] >= '0' && p[1] <= '9')) {
		const char *q = p;
		if (*q == '-')
			q++;
		while ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') ||
		       (*q >= 'A' && *q <= 'F') || *q == 'x' || *q == 'X')
			q++;
		l->lexeme = sbuf_strndup(p, q - p);
		l->tok = T_NUM;
		l->p = q;
		return;
	}
	if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_') {
		const char *q = p;
		while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
		       (*q >= '0' && *q <= '9') || *q == '_')
			q++;
		l->lexeme = sbuf_strndup(p, q - p);
		l->tok = T_IDENT;
		l->p = q;
		return;
	}
	die("sdkconfig: unexpected character '%c' in expression '%s'", *p,
	    l->src);
}

static int is_truthy(const char *v)
{
	if (!v || !*v)
		return 0;
	if (!strcmp(v, "n") || !strcmp(v, "0"))
		return 0;
	/* "0x0" and similar hex zero values are rare; treat everything else
	 * as truthy to match Kconfig semantics for set symbols. */
	return 1;
}

/* Forward declarations for recursive descent. */
static int parse_or(struct lexer *l, const struct sdkconfig *s);

static char *take_lexeme(struct lexer *l)
{
	char *v = l->lexeme;
	l->lexeme = NULL;
	return v;
}

static int parse_atom_truthy(struct lexer *l, const struct sdkconfig *s)
{
	if (l->tok == T_LPAREN) {
		lex_next(l);
		int v = parse_or(l, s);
		if (l->tok != T_RPAREN)
			die("sdkconfig: expected ')' in '%s'", l->src);
		lex_next(l);
		return v;
	}
	if (l->tok == T_IDENT) {
		const char *name = l->lexeme;
		/* Some fragments use the CONFIG_ prefix explicitly. */
		if (!strncmp(name, "CONFIG_", 7))
			name += 7;
		int v = is_truthy(lookup(s, name));
		lex_next(l);
		return v;
	}
	if (l->tok == T_NUM) {
		int v = is_truthy(l->lexeme);
		lex_next(l);
		return v;
	}
	if (l->tok == T_STR) {
		int v = is_truthy(l->lexeme);
		lex_next(l);
		return v;
	}
	die("sdkconfig: expected atom in '%s'", l->src);
	return 0;
}

/* Kconfig tristate literals: y / n / m are constants, not symbols. */
static int is_tristate_literal(const char *s)
{
	return s && s[0] && s[1] == '\0' &&
	       (s[0] == 'y' || s[0] == 'n' || s[0] == 'm');
}

/* For equality, we need the VALUE of an atom (string form), not just
 * its truthiness.  Resolves identifiers against sdkconfig, except for
 * y/n/m which are tristate literals (matching Kconfig semantics). */
static char *atom_value(struct lexer *l, const struct sdkconfig *s)
{
	if (l->tok == T_IDENT) {
		const char *name = l->lexeme;
		if (is_tristate_literal(name)) {
			char *v = take_lexeme(l);
			lex_next(l);
			return v;
		}
		if (!strncmp(name, "CONFIG_", 7))
			name += 7;
		const char *v = lookup(s, name);
		/* Absent symbol => Kconfig tristate "n" (matches kconfiglib).
		 */
		char *ret = sbuf_strdup(v ? v : "n");
		lex_next(l);
		return ret;
	}
	if (l->tok == T_NUM || l->tok == T_STR) {
		char *v = take_lexeme(l);
		lex_next(l);
		return v;
	}
	die("sdkconfig: expected value in '%s'", l->src);
	return NULL;
}

/* Resolve a saved IDENT/NUM/STR lexeme to its Kconfig value string. */
static char *resolve_lex(const char *lex_saved, enum tok kind,
			 const struct sdkconfig *s)
{
	if (kind != T_IDENT || is_tristate_literal(lex_saved))
		return sbuf_strdup(lex_saved);
	const char *name = lex_saved;
	if (!strncmp(name, "CONFIG_", 7))
		name += 7;
	const char *v = lookup(s, name);
	return sbuf_strdup(v ? v : "n");
}

/* Parse a numeric value from an sdkconfig/literal string.  Supports
 * decimal, hex (0x...), and negative.  Returns 1 on success. */
static int parse_long(const char *s, long *out)
{
	if (!s || !*s)
		return 0;
	char *end;
	long v = strtol(s, &end, 0);
	if (end == s || *end != '\0')
		return 0;
	*out = v;
	return 1;
}

static int parse_cmp(struct lexer *l, const struct sdkconfig *s)
{
	if (l->tok == T_IDENT || l->tok == T_NUM || l->tok == T_STR) {
		/* Save current lexeme and advance to see the op. */
		char *lex_saved = take_lexeme(l);
		enum tok kind = l->tok;
		lex_next(l);
		if (l->tok == T_EQ || l->tok == T_NE || l->tok == T_LT ||
		    l->tok == T_LE || l->tok == T_GT || l->tok == T_GE) {
			enum tok op = l->tok;
			lex_next(l);
			char *rhs = atom_value(l, s);
			char *lhs = resolve_lex(lex_saved, kind, s);
			int result;
			if (op == T_EQ) {
				result = !strcmp(lhs, rhs);
			} else if (op == T_NE) {
				result = !!strcmp(lhs, rhs);
			} else {
				/* Numeric ordering.  If either side isn't
				 * parseable as a number, fall back to strcmp --
				 * matches Kconfig's behavior for string cmp. */
				long a, b;
				if (parse_long(lhs, &a) &&
				    parse_long(rhs, &b)) {
					switch (op) {
					case T_LT:
						result = a < b;
						break;
					case T_LE:
						result = a <= b;
						break;
					case T_GT:
						result = a > b;
						break;
					case T_GE:
						result = a >= b;
						break;
					default:
						result = 0;
						break;
					}
				} else {
					int c = strcmp(lhs, rhs);
					switch (op) {
					case T_LT:
						result = c < 0;
						break;
					case T_LE:
						result = c <= 0;
						break;
					case T_GT:
						result = c > 0;
						break;
					case T_GE:
						result = c >= 0;
						break;
					default:
						result = 0;
						break;
					}
				}
			}
			free(lhs);
			free(rhs);
			free(lex_saved);
			return result;
		}
		/* Not a comparison; treat as truthy atom. */
		int v;
		if (kind == T_IDENT) {
			const char *name = lex_saved;
			if (!strncmp(name, "CONFIG_", 7))
				name += 7;
			v = is_truthy(lookup(s, name));
		} else {
			v = is_truthy(lex_saved);
		}
		free(lex_saved);
		return v;
	}
	/* Paren or unary not. */
	return parse_atom_truthy(l, s);
}

static int parse_not(struct lexer *l, const struct sdkconfig *s)
{
	if (l->tok == T_NOT) {
		lex_next(l);
		return !parse_not(l, s);
	}
	return parse_cmp(l, s);
}

static int parse_and(struct lexer *l, const struct sdkconfig *s)
{
	int v = parse_not(l, s);
	while (l->tok == T_AND) {
		lex_next(l);
		int rhs = parse_not(l, s);
		v = v && rhs;
	}
	return v;
}

static int parse_or(struct lexer *l, const struct sdkconfig *s)
{
	int v = parse_and(l, s);
	while (l->tok == T_OR) {
		lex_next(l);
		int rhs = parse_and(l, s);
		v = v || rhs;
	}
	return v;
}

int sdkconfig_eval(const struct sdkconfig *s, const char *expr)
{
	if (!expr || !*expr)
		return 0;
	struct lexer l = {.src = expr, .p = expr};
	lex_next(&l);
	if (l.tok == T_END) {
		free(l.lexeme);
		return 0;
	}
	int v = parse_or(&l, s);
	free(l.lexeme);
	if (l.tok != T_END)
		die("sdkconfig: trailing tokens in '%s'", expr);
	return v;
}
