/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lf.c
 * @brief Linker fragment (.lf) parser -- hand-written LL(1).
 *
 * Two-layer design:
 *
 *   Lexer   Scans the input character-by-character, emitting a stream
 *           of tokens.  Indentation is tracked with a stack; the lexer
 *           emits synthetic INDENT/DEDENT tokens so the parser sees a
 *           flat, context-free token stream.
 *
 *   Parser  Recursive descent driven by the LL(1) grammar in README.md.
 *           Each grammar production maps to one static function.
 *           Conditionals (if/elif/else) are shared across all entry
 *           types through a function-pointer callback (@c stmt_parser_fn).
 */
#include "../../ice.h"
#include "lf.h"

/* ================================================================== */
/*  Character classification                                          */
/* ================================================================== */

/**
 * True for characters that may start a NAME token.
 * Covers IDENT, SEC_NAME, and OBJ_NAME first-char classes from the
 * grammar (letters, underscore, dot).
 */
static int is_name_start(int c)
{
	return isalpha(c) || c == '_' || c == '.';
}

/**
 * True for characters that may continue a NAME token (after the first).
 * Union of continuation classes: [a-zA-Z0-9._$+].
 * Hyphens are handled separately in the name-matching loop to avoid
 * consuming the '-' in '->'.
 */
static int is_name_cont(int c)
{
	return isalnum(c) || c == '_' || c == '.' || c == '$' || c == '+';
}

/* ================================================================== */
/*  Lexer -- token types                                              */
/* ================================================================== */

enum lf_tok {
	TOK_EOF = 0,
	TOK_NL,		/**< end of a content line */
	TOK_INDENT,		/**< indentation deeper than current level */
	TOK_DEDENT,		/**< indentation shallower than current level */
	TOK_LBRACK,		/**< '[' */
	TOK_RBRACK,		/**< ']' */
	TOK_LPAREN,		/**< '(' */
	TOK_RPAREN,		/**< ')' */
	TOK_COLON,		/**< ':' */
	TOK_SEMI,		/**< ';' */
	TOK_COMMA,		/**< ',' */
	TOK_ARROW,		/**< '->' */
	TOK_STAR,		/**< '*' */
	TOK_NAME,		/**< identifier / section name / entity name */
	TOK_NUM,		/**< integer literal */
};

/* ================================================================== */
/*  Lexer -- state                                                    */
/* ================================================================== */

/**
 * Lexer state.
 *
 * The lexer scans a NUL-terminated input buffer and produces one token
 * per lf_next() call.  It tracks:
 *
 *  - An indentation stack (@c indent / @c depth) for emitting
 *    INDENT/DEDENT tokens at block boundaries.
 *
 *  - A small ring buffer (@c pending) for cases where a single scan
 *    position produces multiple tokens (e.g. dedenting several levels
 *    at once, or flushing remaining DEDENTs at EOF).
 *
 *  - The current token value: @c tok (type), @c val (string, owned),
 *    @c num (integer).
 */
struct lexer {
	const char *pos;	/**< current scan position in input */
	const char *path;	/**< source file path (for diagnostics) */
	int line;		/**< current line number (1-based) */

	/** Indentation stack.  indent[0] is always 0 (column 0). */
	int indent[64];
	int depth;		/**< top-of-stack index */

	/**
	 * Pending-token ring buffer.  Used when a single scan position
	 * must emit multiple tokens (multi-level dedent, EOF cleanup).
	 * Indices wrap with & 63.
	 */
	int pending[64];
	int qh;			/**< head (next to dequeue) */
	int qt;			/**< tail (next slot to enqueue) */

	int tok;		/**< current token type */
	char *val;		/**< string value for TOK_NAME (owned) */
	int num;		/**< numeric value for TOK_NUM */

	int bol;		/**< true when next scan starts a new line */
	int eof;		/**< true after end-of-input was processed */
};

/** Return a human-readable name for a token type (for diagnostics). */
static const char *tok_str(int t)
{
	switch (t) {
	case TOK_EOF:    return "EOF";
	case TOK_NL:     return "newline";
	case TOK_INDENT: return "INDENT";
	case TOK_DEDENT: return "DEDENT";
	case TOK_LBRACK: return "'['";
	case TOK_RBRACK: return "']'";
	case TOK_LPAREN: return "'('";
	case TOK_RPAREN: return "')'";
	case TOK_COLON:  return "':'";
	case TOK_SEMI:   return "';'";
	case TOK_COMMA:  return "','";
	case TOK_ARROW:  return "'->'";
	case TOK_STAR:   return "'*'";
	case TOK_NAME:   return "name";
	case TOK_NUM:    return "number";
	default:         return "?";
	}
}

/* ---- pending-token ring buffer ---------------------------------- */

static void q_push(struct lexer *l, int t) { l->pending[l->qt++ & 63] = t; }
static int  q_pop(struct lexer *l)         { return l->pending[l->qh++ & 63]; }
static int  q_any(struct lexer *l)         { return l->qh < l->qt; }

/* ================================================================== */
/*  Lexer -- indentation                                              */
/* ================================================================== */

/**
 * Compare @p indent with the current stack top and emit INDENT or
 * DEDENT tokens as needed.  Same-level produces no token.
 *
 * On dedent, one DEDENT is queued per popped level.  If the new indent
 * does not match any level on the stack, die with an error.
 */
static void process_indent(struct lexer *l, int indent)
{
	int top = l->indent[l->depth];

	if (indent > top) {
		if (l->depth + 1 >= 64)
			die("%s:%d: indentation too deep", l->path, l->line);
		l->indent[++l->depth] = indent;
		q_push(l, TOK_INDENT);
	} else if (indent < top) {
		while (l->depth > 0 && l->indent[l->depth] > indent) {
			l->depth--;
			q_push(l, TOK_DEDENT);
		}
		if (l->indent[l->depth] != indent)
			die("%s:%d: indentation mismatch", l->path, l->line);
	}
}

/* ================================================================== */
/*  Lexer -- main entry point                                         */
/* ================================================================== */

/**
 * Advance the lexer to the next token, returning its type.
 *
 * The token type is also stored in @c l->tok.  For TOK_NAME the string
 * value is in @c l->val (owned by the lexer, freed on the next call or
 * when taken by expect_name()).  For TOK_NUM the value is in @c l->num.
 *
 * At the beginning of each line (bol flag set), the lexer:
 *  1. Skips blank and comment-only lines.
 *  2. Measures the leading whitespace and calls process_indent().
 *  3. Returns any queued INDENT/DEDENT before scanning content.
 *
 * At EOF, remaining DEDENT tokens are flushed from the indent stack.
 */
static int lf_next(struct lexer *l)
{
	/* drain pending queue first */
	if (q_any(l))
		return l->tok = q_pop(l);
	if (l->eof)
		return l->tok = TOK_EOF;

	/* beginning of line: indentation analysis */
	if (l->bol) {
		l->bol = 0;
		for (;;) {
			int indent = 0;
			while (*l->pos == ' ' || *l->pos == '\t') {
				indent++;
				l->pos++;
			}
			/* blank line — skip, stay in BOL loop */
			if (*l->pos == '\n') {
				l->pos++;
				l->line++;
				continue;
			}
			/* comment-only line — skip */
			if (*l->pos == '#') {
				while (*l->pos && *l->pos != '\n')
					l->pos++;
				if (*l->pos == '\n') {
					l->pos++;
					l->line++;
				}
				continue;
			}
			/* EOF at BOL — flush remaining indent levels */
			if (*l->pos == '\0') {
				while (l->depth > 0) {
					q_push(l, TOK_DEDENT);
					l->depth--;
				}
				l->eof = 1;
				return l->tok = q_any(l)
					? q_pop(l) : TOK_EOF;
			}
			/* content line — process indent, then scan */
			process_indent(l, indent);
			if (q_any(l))
				return l->tok = q_pop(l);
			break;
		}
	}

	/* skip inline whitespace */
	while (*l->pos == ' ' || *l->pos == '\t')
		l->pos++;

	/* newline → end current line, enter BOL */
	if (*l->pos == '\n') {
		l->pos++;
		l->line++;
		l->bol = 1;
		return l->tok = TOK_NL;
	}

	/* inline comment → treat as end of line */
	if (*l->pos == '#') {
		while (*l->pos && *l->pos != '\n')
			l->pos++;
		if (*l->pos == '\n') {
			l->pos++;
			l->line++;
		}
		l->bol = 1;
		return l->tok = TOK_NL;
	}

	/* EOF mid-line — synthesise NL + remaining DEDENTs */
	if (*l->pos == '\0') {
		q_push(l, TOK_NL);
		while (l->depth > 0) {
			q_push(l, TOK_DEDENT);
			l->depth--;
		}
		l->eof = 1;
		return l->tok = q_pop(l);
	}

	/* '->' (must precede single-char '-' check) */
	if (l->pos[0] == '-' && l->pos[1] == '>') {
		l->pos += 2;
		return l->tok = TOK_ARROW;
	}

	/* single-character tokens */
	switch (*l->pos) {
	case '[': l->pos++; return l->tok = TOK_LBRACK;
	case ']': l->pos++; return l->tok = TOK_RBRACK;
	case '(': l->pos++; return l->tok = TOK_LPAREN;
	case ')': l->pos++; return l->tok = TOK_RPAREN;
	case ':': l->pos++; return l->tok = TOK_COLON;
	case ';': l->pos++; return l->tok = TOK_SEMI;
	case ',': l->pos++; return l->tok = TOK_COMMA;
	case '*': l->pos++; return l->tok = TOK_STAR;
	}

	/*
	 * NAME — covers IDENT, SEC_NAME, OBJ_NAME and ENTITY from the
	 * grammar.  Hyphens are consumed mid-name only when the next
	 * character is NOT '>' (to avoid eating '->').
	 */
	if (is_name_start(*l->pos)) {
		const char *start = l->pos++;
		while (is_name_cont(*l->pos))
			l->pos++;
		while (*l->pos == '-' && l->pos[1] != '>'
		       && is_name_cont(l->pos[1])) {
			l->pos++;
			while (is_name_cont(*l->pos))
				l->pos++;
		}
		free(l->val);
		l->val = sbuf_strndup(start, (size_t)(l->pos - start));
		return l->tok = TOK_NAME;
	}

	/* NUM */
	if (isdigit((unsigned char)*l->pos)) {
		l->num = 0;
		while (isdigit((unsigned char)*l->pos))
			l->num = l->num * 10 + (*l->pos++ - '0');
		return l->tok = TOK_NUM;
	}

	die("%s:%d: unexpected character '%c' (0x%02x)",
	    l->path, l->line, *l->pos, (unsigned char)*l->pos);
	return TOK_EOF;
}

/* ================================================================== */
/*  Parser helpers                                                    */
/* ================================================================== */

/** True if the current token is NAME with value @p kw. */
static int is_kw(struct lexer *l, const char *kw)
{
	return l->tok == TOK_NAME && !strcmp(l->val, kw);
}

/** Consume the current token if it matches @p tok, otherwise die. */
static void expect(struct lexer *l, int tok)
{
	if (l->tok != tok)
		die("%s:%d: expected %s, got %s",
		    l->path, l->line, tok_str(tok), tok_str(l->tok));
	lf_next(l);
}

/**
 * Consume a NAME token: take ownership of its string value (caller
 * must free) and advance to the next token.  Dies if the current
 * token is not NAME.
 */
static char *expect_name(struct lexer *l)
{
	if (l->tok != TOK_NAME)
		die("%s:%d: expected name, got %s",
		    l->path, l->line, tok_str(l->tok));
	char *s = l->val;
	l->val = NULL;
	lf_next(l);
	return s;
}

/**
 * Read a condition expression after "if" or "elif".
 *
 * Called right after the lexer produced the keyword NAME token, so
 * l->pos points just past "if"/"elif".  Reads raw characters up to
 * ':', trims surrounding whitespace, consumes the ':', then advances
 * the lexer (the next token will normally be NL).
 *
 * @return  heap-allocated expression string (caller must free)
 */
static char *read_cond(struct lexer *l)
{
	while (*l->pos == ' ' || *l->pos == '\t')
		l->pos++;

	const char *start = l->pos;
	while (*l->pos && *l->pos != ':' && *l->pos != '\n')
		l->pos++;

	if (*l->pos != ':')
		die("%s:%d: expected ':' after condition", l->path, l->line);

	const char *end = l->pos;
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
		end--;

	l->pos++;
	char *expr = sbuf_strndup(start, (size_t)(end - start));
	lf_next(l);
	return expr;
}

/* ================================================================== */
/*  Parser -- entry-level conditionals                                */
/* ================================================================== */

/**
 * Callback type for parsing entry statements within a block.
 *
 * Each entry type (section, scheme, mapping, archive) provides its own
 * parser function matching this signature.  The conditional parser
 * calls the appropriate one to parse the body of each branch, allowing
 * a single parse_cond() to handle all entry types.
 */
typedef void (*stmt_parser_fn)(struct lexer *l,
			       struct lf_stmt **v, int *n, int *cap);

/**
 * Parse an if/elif/else conditional block and append it as a
 * single LF_STMT_COND entry to the statement list.
 *
 * Called when the current token is NAME("if").  The @p inner callback
 * parses the entry statements inside each branch's indented body.
 *
 * Grammar:
 *   cond(S) = "if" EXPR ":" NL INDENT {S} DEDENT
 *             {"elif" EXPR ":" NL INDENT {S} DEDENT}
 *             ["else:" NL INDENT {S} DEDENT]
 */
static void parse_cond(struct lexer *l,
		       struct lf_stmt **v, int *n, int *cap,
		       stmt_parser_fn inner)
{
	ALLOC_GROW(*v, *n + 1, *cap);
	struct lf_stmt *s = &(*v)[(*n)++];
	memset(s, 0, sizeof(*s));
	s->is_cond = 1;

	int bcap = 0;
	struct lf_branch *branches = NULL;
	int nb = 0;

	/* if branch */
	char *expr = read_cond(l);
	expect(l, TOK_NL);

	ALLOC_GROW(branches, nb + 1, bcap);
	struct lf_branch *b = &branches[nb++];
	memset(b, 0, sizeof(*b));
	b->expr = expr;

	int scap = 0;
	expect(l, TOK_INDENT);
	while (l->tok != TOK_DEDENT && l->tok != TOK_EOF)
		inner(l, &b->stmts, &b->n_stmts, &scap);
	expect(l, TOK_DEDENT);

	/* elif branches */
	while (is_kw(l, "elif")) {
		expr = read_cond(l);
		expect(l, TOK_NL);

		ALLOC_GROW(branches, nb + 1, bcap);
		b = &branches[nb++];
		memset(b, 0, sizeof(*b));
		b->expr = expr;

		scap = 0;
		expect(l, TOK_INDENT);
		while (l->tok != TOK_DEDENT && l->tok != TOK_EOF)
			inner(l, &b->stmts, &b->n_stmts, &scap);
		expect(l, TOK_DEDENT);
	}

	/* else branch */
	if (is_kw(l, "else")) {
		lf_next(l);
		expect(l, TOK_COLON);
		expect(l, TOK_NL);

		ALLOC_GROW(branches, nb + 1, bcap);
		b = &branches[nb++];
		memset(b, 0, sizeof(*b));

		scap = 0;
		expect(l, TOK_INDENT);
		while (l->tok != TOK_DEDENT && l->tok != TOK_EOF)
			inner(l, &b->stmts, &b->n_stmts, &scap);
		expect(l, TOK_DEDENT);
	}

	s->u.cond.branches = branches;
	s->u.cond.n_branches = nb;
}

/* ================================================================== */
/*  Parser -- entry types                                             */
/*                                                                    */
/*  Each function parses statements of one type until a token is seen */
/*  that cannot start a statement (typically DEDENT or EOF).          */
/*  "elif" and "else" break the loop so the caller (parse_cond) can  */
/*  handle them.                                                      */
/* ================================================================== */

/** Parse section entries: SEC_NAME NL | cond(sec_stmt). */
static void parse_sec_stmts(struct lexer *l,
			     struct lf_stmt **v, int *n, int *cap)
{
	while (l->tok == TOK_NAME) {
		if (is_kw(l, "if")) {
			parse_cond(l, v, n, cap, parse_sec_stmts);
			continue;
		}
		if (is_kw(l, "elif") || is_kw(l, "else"))
			break;

		ALLOC_GROW(*v, *n + 1, *cap);
		struct lf_stmt *s = &(*v)[(*n)++];
		memset(s, 0, sizeof(*s));
		s->u.entry.name = expect_name(l);
		expect(l, TOK_NL);
	}
}

/** Parse scheme entries: IDENT '->' IDENT NL | cond(sch_stmt). */
static void parse_sch_stmts(struct lexer *l,
			     struct lf_stmt **v, int *n, int *cap)
{
	while (l->tok == TOK_NAME) {
		if (is_kw(l, "if")) {
			parse_cond(l, v, n, cap, parse_sch_stmts);
			continue;
		}
		if (is_kw(l, "elif") || is_kw(l, "else"))
			break;

		ALLOC_GROW(*v, *n + 1, *cap);
		struct lf_stmt *s = &(*v)[(*n)++];
		memset(s, 0, sizeof(*s));
		s->u.entry.name = expect_name(l);
		expect(l, TOK_ARROW);
		s->u.entry.target = expect_name(l);
		expect(l, TOK_NL);
	}
}

/**
 * Parse a single flag: KEEP() | ALIGN(...) | SORT(...) | SURROUND(...).
 *
 * Called when the current token is a NAME that should be a flag keyword.
 */
static void parse_flag(struct lexer *l, struct lf_flag *f)
{
	memset(f, 0, sizeof(*f));

	if (is_kw(l, "KEEP")) {
		f->kind = LF_FLAG_KEEP;
		f->pre = 1;
		lf_next(l);
		expect(l, TOK_LPAREN);
		expect(l, TOK_RPAREN);
	} else if (is_kw(l, "ALIGN")) {
		f->kind = LF_FLAG_ALIGN;
		f->pre = 1;
		lf_next(l);
		expect(l, TOK_LPAREN);
		if (l->tok != TOK_NUM)
			die("%s:%d: expected number in ALIGN()",
			    l->path, l->line);
		f->alignment = l->num;
		lf_next(l);
		/* optional ', pre' */
		if (l->tok == TOK_COMMA && l->pos[0] != ' '
		    ? 0 : l->tok == TOK_COMMA) {
			/* peek: is the next name 'pre' or 'post'? */
			if (l->tok == TOK_COMMA) {
				lf_next(l);
				if (is_kw(l, "pre")) {
					lf_next(l);
					/* optional ', post' */
					if (l->tok == TOK_COMMA) {
						lf_next(l);
						if (is_kw(l, "post")) {
							f->post = 1;
							lf_next(l);
						}
					}
				} else if (is_kw(l, "post")) {
					f->pre = 0;
					f->post = 1;
					lf_next(l);
				}
			}
		}
		expect(l, TOK_RPAREN);
	} else if (is_kw(l, "SORT")) {
		f->kind = LF_FLAG_SORT;
		lf_next(l);
		expect(l, TOK_LPAREN);
		if (l->tok == TOK_NAME && l->tok != TOK_RPAREN) {
			f->sort_first = expect_name(l);
			if (l->tok == TOK_COMMA) {
				lf_next(l);
				f->sort_second = expect_name(l);
			}
		}
		expect(l, TOK_RPAREN);
	} else if (is_kw(l, "SURROUND")) {
		f->kind = LF_FLAG_SURROUND;
		lf_next(l);
		expect(l, TOK_LPAREN);
		f->symbol = expect_name(l);
		expect(l, TOK_RPAREN);
	} else {
		die("%s:%d: expected flag keyword (KEEP/ALIGN/SORT/SURROUND), got '%s'",
		    l->path, l->line,
		    l->tok == TOK_NAME ? l->val : tok_str(l->tok));
	}
}

/**
 * Parse a flag item: IDENT '->' IDENT flag { flag }.
 */
static void parse_flag_item(struct lexer *l, struct lf_flag_item *item)
{
	memset(item, 0, sizeof(*item));
	item->sections = expect_name(l);
	expect(l, TOK_ARROW);
	item->target = expect_name(l);

	int fcap = 0;
	while (l->tok == TOK_NAME
	       && (is_kw(l, "KEEP") || is_kw(l, "ALIGN")
		   || is_kw(l, "SORT") || is_kw(l, "SURROUND"))) {
		ALLOC_GROW(item->flags, item->n_flags + 1, fcap);
		parse_flag(l, &item->flags[item->n_flags++]);
	}

	if (item->n_flags == 0)
		die("%s:%d: expected at least one flag after section->target",
		    l->path, l->line);
}

/**
 * Parse a flag list after ';' on a mapping entry.
 *
 * Grammar:
 *   ';' flag_list
 *   flag_list = flag_item { ',' [NL] flag_item }
 *
 * The flag list may be on the same line or span an indented block
 * separated by commas.
 */
static void parse_flag_list(struct lexer *l, struct lf_entry *entry)
{
	lf_next(l); /* consume ';' */

	/*
	 * The flag list can be either:
	 *   1. Inline on the same line (single flag_item, no indent)
	 *   2. Indented block with comma-separated flag_items across lines
	 *
	 * In case 2 the ';' is followed by NL INDENT.
	 */
	int in_block = 0;
	if (l->tok == TOK_NL) {
		lf_next(l);
		if (l->tok == TOK_INDENT) {
			in_block = 1;
			lf_next(l);
		}
	}

	int cap = 0;

	ALLOC_GROW(entry->flag_items, entry->n_flag_items + 1, cap);
	parse_flag_item(l, &entry->flag_items[entry->n_flag_items++]);

	while (l->tok == TOK_COMMA) {
		lf_next(l); /* consume ',' */
		if (l->tok == TOK_NL)
			lf_next(l); /* optional NL after comma */
		ALLOC_GROW(entry->flag_items, entry->n_flag_items + 1, cap);
		parse_flag_item(l, &entry->flag_items[entry->n_flag_items++]);
	}

	if (in_block) {
		if (l->tok == TOK_NL)
			lf_next(l);
		expect(l, TOK_DEDENT);
	} else {
		expect(l, TOK_NL);
	}
}

/**
 * Parse mapping entries.
 *
 * Grammar:
 *   map_stmt  = map_entry NL | map_entry ';' flag_list | cond(map_stmt)
 *   map_entry = ('*' | OBJ_NAME [':' IDENT]) '(' IDENT ')'
 */
static void parse_map_stmts(struct lexer *l,
			     struct lf_stmt **v, int *n, int *cap)
{
	while (l->tok == TOK_NAME || l->tok == TOK_STAR) {
		if (is_kw(l, "if")) {
			parse_cond(l, v, n, cap, parse_map_stmts);
			continue;
		}
		if (is_kw(l, "elif") || is_kw(l, "else"))
			break;

		char *name;
		char *symbol = NULL;

		if (l->tok == TOK_STAR) {
			name = sbuf_strdup("*");
			lf_next(l);
		} else {
			name = expect_name(l);
			if (l->tok == TOK_COLON) {
				lf_next(l);
				symbol = expect_name(l);
			}
		}
		expect(l, TOK_LPAREN);
		char *scheme = expect_name(l);
		expect(l, TOK_RPAREN);

		ALLOC_GROW(*v, *n + 1, *cap);
		struct lf_stmt *s = &(*v)[(*n)++];
		memset(s, 0, sizeof(*s));
		s->u.entry.name = name;
		s->u.entry.target = symbol;
		s->u.entry.scheme = scheme;

		if (l->tok == TOK_SEMI)
			parse_flag_list(l, &s->u.entry);
		else
			expect(l, TOK_NL);
	}
}

/**
 * Parse archive entries: (ENTITY | '*') NL | cond(archive_stmt).
 *
 * Used for the indented-block form of "archive:", where the archive
 * name is selected by a conditional.
 */
static void parse_archive_stmts(struct lexer *l,
				struct lf_stmt **v, int *n, int *cap)
{
	while (l->tok == TOK_NAME || l->tok == TOK_STAR) {
		if (is_kw(l, "if")) {
			parse_cond(l, v, n, cap, parse_archive_stmts);
			continue;
		}
		if (is_kw(l, "elif") || is_kw(l, "else"))
			break;

		ALLOC_GROW(*v, *n + 1, *cap);
		struct lf_stmt *s = &(*v)[(*n)++];
		memset(s, 0, sizeof(*s));
		if (l->tok == TOK_STAR) {
			s->u.entry.name = sbuf_strdup("*");
			lf_next(l);
		} else {
			s->u.entry.name = expect_name(l);
		}
		expect(l, TOK_NL);
	}
}

/* ================================================================== */
/*  Parser -- fragments                                               */
/* ================================================================== */

static void parse_frags(struct lexer *l,
			struct lf_frag **v, int *n, int *cap);

/**
 * Parse a fragment-level conditional (if/elif/else wrapping whole
 * fragments).  Same structure as entry-level conditionals but the
 * body contains fragments instead of entry statements.
 */
static void parse_frag_cond(struct lexer *l,
			    struct lf_frag **v, int *n, int *cap)
{
	ALLOC_GROW(*v, *n + 1, *cap);
	struct lf_frag *f = &(*v)[(*n)++];
	memset(f, 0, sizeof(*f));
	f->kind = LF_FRAG_COND;

	int bcap = 0;
	struct lf_frag_branch *branches = NULL;
	int nb = 0;

	/* if */
	char *expr = read_cond(l);
	expect(l, TOK_NL);

	ALLOC_GROW(branches, nb + 1, bcap);
	struct lf_frag_branch *b = &branches[nb++];
	memset(b, 0, sizeof(*b));
	b->expr = expr;

	int fcap = 0;
	expect(l, TOK_INDENT);
	while (l->tok != TOK_DEDENT && l->tok != TOK_EOF)
		parse_frags(l, &b->frags, &b->n_frags, &fcap);
	expect(l, TOK_DEDENT);

	/* elif */
	while (is_kw(l, "elif")) {
		expr = read_cond(l);
		expect(l, TOK_NL);
		ALLOC_GROW(branches, nb + 1, bcap);
		b = &branches[nb++];
		memset(b, 0, sizeof(*b));
		b->expr = expr;
		fcap = 0;
		expect(l, TOK_INDENT);
		while (l->tok != TOK_DEDENT && l->tok != TOK_EOF)
			parse_frags(l, &b->frags, &b->n_frags, &fcap);
		expect(l, TOK_DEDENT);
	}

	/* else */
	if (is_kw(l, "else")) {
		lf_next(l);
		expect(l, TOK_COLON);
		expect(l, TOK_NL);
		ALLOC_GROW(branches, nb + 1, bcap);
		b = &branches[nb++];
		memset(b, 0, sizeof(*b));
		fcap = 0;
		expect(l, TOK_INDENT);
		while (l->tok != TOK_DEDENT && l->tok != TOK_EOF)
			parse_frags(l, &b->frags, &b->n_frags, &fcap);
		expect(l, TOK_DEDENT);
	}

	f->u.cond.branches = branches;
	f->u.cond.n = nb;
}

/**
 * Parse the "entries:" key and its indented body.
 *
 * Handles both the standard form (INDENT stmts DEDENT) and the
 * same-level form where entries appear at the same indent as the key.
 * The @p fn callback determines which entry type is parsed.
 */
static void parse_entries(struct lexer *l, stmt_parser_fn fn,
			  struct lf_stmt **v, int *n, int *cap)
{
	if (!is_kw(l, "entries"))
		die("%s:%d: expected 'entries'", l->path, l->line);
	lf_next(l);
	expect(l, TOK_COLON);
	expect(l, TOK_NL);

	if (l->tok == TOK_INDENT) {
		lf_next(l);
		while (l->tok != TOK_DEDENT && l->tok != TOK_EOF)
			fn(l, v, n, cap);
		expect(l, TOK_DEDENT);
	} else {
		/* same-level: entries at same indent as "entries:" key */
		fn(l, v, n, cap);
	}
}

/** Parse a [sections:NAME] fragment (header already consumed up to "sections"). */
static void parse_sections(struct lexer *l,
			   struct lf_frag **v, int *n, int *cap)
{
	lf_next(l);
	expect(l, TOK_COLON);
	char *name = expect_name(l);
	expect(l, TOK_RBRACK);
	expect(l, TOK_NL);

	struct lf_stmt *stmts = NULL;
	int ns = 0, scap = 0;
	parse_entries(l, parse_sec_stmts, &stmts, &ns, &scap);

	ALLOC_GROW(*v, *n + 1, *cap);
	struct lf_frag *f = &(*v)[(*n)++];
	memset(f, 0, sizeof(*f));
	f->kind = LF_SECTIONS;
	f->u.sec.name = name;
	f->u.sec.stmts = stmts;
	f->u.sec.n = ns;
}

/** Parse a [scheme:NAME] fragment. */
static void parse_scheme(struct lexer *l,
			 struct lf_frag **v, int *n, int *cap)
{
	lf_next(l);
	expect(l, TOK_COLON);
	char *name = expect_name(l);
	expect(l, TOK_RBRACK);
	expect(l, TOK_NL);

	struct lf_stmt *stmts = NULL;
	int ns = 0, scap = 0;
	parse_entries(l, parse_sch_stmts, &stmts, &ns, &scap);

	ALLOC_GROW(*v, *n + 1, *cap);
	struct lf_frag *f = &(*v)[(*n)++];
	memset(f, 0, sizeof(*f));
	f->kind = LF_SCHEME;
	f->u.sch.name = name;
	f->u.sch.stmts = stmts;
	f->u.sch.n = ns;
}

/**
 * Parse a [mapping:NAME] fragment.
 *
 * The archive value can appear inline ("archive: libfoo.a") or in an
 * indented conditional block.  Both forms are stored as a statement
 * list in the AST.
 */
static void parse_mapping(struct lexer *l,
			  struct lf_frag **v, int *n, int *cap)
{
	lf_next(l);
	expect(l, TOK_COLON);
	char *name = expect_name(l);
	expect(l, TOK_RBRACK);
	expect(l, TOK_NL);

	/* archive: VALUE NL  or  archive: NL INDENT archive_stmts DEDENT */
	if (!is_kw(l, "archive"))
		die("%s:%d: expected 'archive'", l->path, l->line);
	lf_next(l);
	expect(l, TOK_COLON);

	struct lf_stmt *archive = NULL;
	int na = 0, acap = 0;

	if (l->tok == TOK_NAME || l->tok == TOK_STAR) {
		/* inline: archive value on the same line */
		ALLOC_GROW(archive, na + 1, acap);
		struct lf_stmt *s = &archive[na++];
		memset(s, 0, sizeof(*s));
		if (l->tok == TOK_STAR) {
			s->u.entry.name = sbuf_strdup("*");
			lf_next(l);
		} else {
			s->u.entry.name = expect_name(l);
		}
		expect(l, TOK_NL);
	} else {
		/* conditional: archive value in indented block */
		expect(l, TOK_NL);
		expect(l, TOK_INDENT);
		while (l->tok != TOK_DEDENT && l->tok != TOK_EOF)
			parse_archive_stmts(l, &archive, &na, &acap);
		expect(l, TOK_DEDENT);
	}

	/* entries */
	struct lf_stmt *entries = NULL;
	int ne = 0, ecap = 0;
	parse_entries(l, parse_map_stmts, &entries, &ne, &ecap);

	ALLOC_GROW(*v, *n + 1, *cap);
	struct lf_frag *f = &(*v)[(*n)++];
	memset(f, 0, sizeof(*f));
	f->kind = LF_MAPPING;
	f->u.map.name = name;
	f->u.map.archive = archive;
	f->u.map.n_archive = na;
	f->u.map.entries = entries;
	f->u.map.n_entries = ne;
}

/**
 * Parse a sequence of fragments until EOF or DEDENT.
 *
 * This is the top-level loop and also the body of fragment-level
 * conditionals.  Stray newlines between fragments are skipped.
 */
static void parse_frags(struct lexer *l,
			struct lf_frag **v, int *n, int *cap)
{
	while (l->tok != TOK_EOF && l->tok != TOK_DEDENT) {
		if (l->tok == TOK_NL) {
			lf_next(l);
			continue;
		}
		if (is_kw(l, "if")) {
			parse_frag_cond(l, v, n, cap);
			continue;
		}
		if (l->tok != TOK_LBRACK)
			die("%s:%d: expected '[' or 'if', got %s",
			    l->path, l->line, tok_str(l->tok));
		lf_next(l);

		if (is_kw(l, "sections"))
			parse_sections(l, v, n, cap);
		else if (is_kw(l, "scheme"))
			parse_scheme(l, v, n, cap);
		else if (is_kw(l, "mapping"))
			parse_mapping(l, v, n, cap);
		else
			die("%s:%d: unknown fragment type '%s'",
			    l->path, l->line,
			    l->tok == TOK_NAME ? l->val : tok_str(l->tok));
	}
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

struct lf_file *lf_parse(const char *src, const char *path)
{
	struct lexer l;
	memset(&l, 0, sizeof(l));
	l.pos = src;
	l.path = path;
	l.line = 1;
	l.bol = 1;

	lf_next(&l); /* prime the first token */

	struct lf_file *f = calloc(1, sizeof(*f));
	if (!f)
		die_errno("calloc");
	f->path = sbuf_strdup(path);

	int cap = 0;
	parse_frags(&l, &f->frags, &f->n_frags, &cap);

	if (l.tok != TOK_EOF)
		die("%s:%d: trailing content after fragments",
		    l.path, l.line);

	free(l.val);
	return f;
}

/* ================================================================== */
/*  Free                                                              */
/* ================================================================== */

static void free_flag(struct lf_flag *f)
{
	free(f->sort_first);
	free(f->sort_second);
	free(f->symbol);
}

static void free_flag_item(struct lf_flag_item *item)
{
	free(item->sections);
	free(item->target);
	for (int i = 0; i < item->n_flags; i++)
		free_flag(&item->flags[i]);
	free(item->flags);
}

static void free_entry(struct lf_entry *e)
{
	free(e->name);
	free(e->target);
	free(e->scheme);
	for (int i = 0; i < e->n_flag_items; i++)
		free_flag_item(&e->flag_items[i]);
	free(e->flag_items);
}

static void free_stmts(struct lf_stmt *v, int n);

static void free_branch(struct lf_branch *b)
{
	free(b->expr);
	free_stmts(b->stmts, b->n_stmts);
}

static void free_stmts(struct lf_stmt *v, int n)
{
	for (int i = 0; i < n; i++) {
		if (v[i].is_cond) {
			for (int j = 0; j < v[i].u.cond.n_branches; j++)
				free_branch(&v[i].u.cond.branches[j]);
			free(v[i].u.cond.branches);
		} else {
			free_entry(&v[i].u.entry);
		}
	}
	free(v);
}

static void free_frag_branch(struct lf_frag_branch *v, int n);

static void free_frag(struct lf_frag *f)
{
	switch (f->kind) {
	case LF_SECTIONS:
		free(f->u.sec.name);
		free_stmts(f->u.sec.stmts, f->u.sec.n);
		break;
	case LF_SCHEME:
		free(f->u.sch.name);
		free_stmts(f->u.sch.stmts, f->u.sch.n);
		break;
	case LF_MAPPING:
		free(f->u.map.name);
		free_stmts(f->u.map.archive, f->u.map.n_archive);
		free_stmts(f->u.map.entries, f->u.map.n_entries);
		break;
	case LF_FRAG_COND:
		free_frag_branch(f->u.cond.branches, f->u.cond.n);
		break;
	}
}

static void free_frag_branch(struct lf_frag_branch *v, int n)
{
	for (int i = 0; i < n; i++) {
		free(v[i].expr);
		for (int j = 0; j < v[i].n_frags; j++)
			free_frag(&v[i].frags[j]);
		free(v[i].frags);
	}
	free(v);
}

void lf_file_free(struct lf_file *f)
{
	if (!f)
		return;
	for (int i = 0; i < f->n_frags; i++)
		free_frag(&f->frags[i]);
	free(f->frags);
	free(f->path);
	free(f);
}

/* ================================================================== */
/*  Dump (debugging)                                                  */
/* ================================================================== */

static void dump_stmts(const struct lf_stmt *v, int n,
		       enum lf_frag_kind ctx, int depth);

static void pr_indent(int depth)
{
	for (int i = 0; i < depth; i++)
		printf("    ");
}

static void dump_flag(const struct lf_flag *f)
{
	switch (f->kind) {
	case LF_FLAG_KEEP:
		printf(" KEEP()");
		break;
	case LF_FLAG_ALIGN:
		printf(" ALIGN(%d", f->alignment);
		if (f->pre && f->post)
			printf(", pre, post");
		else if (f->post)
			printf(", post");
		printf(")");
		break;
	case LF_FLAG_SORT:
		printf(" SORT(");
		if (f->sort_first) {
			printf("%s", f->sort_first);
			if (f->sort_second)
				printf(", %s", f->sort_second);
		}
		printf(")");
		break;
	case LF_FLAG_SURROUND:
		printf(" SURROUND(%s)", f->symbol);
		break;
	}
}

static void dump_entry(const struct lf_entry *e, enum lf_frag_kind ctx,
		       int depth)
{
	pr_indent(depth);
	switch (ctx) {
	case LF_SECTIONS:
		printf("%s\n", e->name);
		break;
	case LF_SCHEME:
		printf("%s -> %s\n", e->name, e->target);
		break;
	case LF_MAPPING:
		if (e->target)
			printf("%s:%s (%s)", e->name, e->target, e->scheme);
		else
			printf("%s (%s)", e->name, e->scheme);
		if (e->n_flag_items > 0) {
			printf(";\n");
			for (int i = 0; i < e->n_flag_items; i++) {
				const struct lf_flag_item *fi =
					&e->flag_items[i];
				pr_indent(depth + 1);
				printf("%s -> %s", fi->sections, fi->target);
				for (int j = 0; j < fi->n_flags; j++)
					dump_flag(&fi->flags[j]);
				if (i + 1 < e->n_flag_items)
					printf(",");
				printf("\n");
			}
		} else {
			printf("\n");
		}
		break;
	default:
		printf("%s\n", e->name);
		break;
	}
}

static void dump_cond(const struct lf_branch *branches, int nb,
		      enum lf_frag_kind ctx, int depth)
{
	for (int i = 0; i < nb; i++) {
		pr_indent(depth);
		if (branches[i].expr)
			printf("%s %s:\n", i == 0 ? "if" : "elif",
			       branches[i].expr);
		else
			printf("else:\n");
		dump_stmts(branches[i].stmts, branches[i].n_stmts,
			   ctx, depth + 1);
	}
}

static void dump_stmts(const struct lf_stmt *v, int n,
		       enum lf_frag_kind ctx, int depth)
{
	for (int i = 0; i < n; i++) {
		if (v[i].is_cond)
			dump_cond(v[i].u.cond.branches,
				  v[i].u.cond.n_branches, ctx, depth);
		else
			dump_entry(&v[i].u.entry, ctx, depth);
	}
}

static void dump_frag(const struct lf_frag *f, int depth);

static void dump_frag_cond(const struct lf_frag_branch *branches, int nb,
			   int depth)
{
	for (int i = 0; i < nb; i++) {
		pr_indent(depth);
		if (branches[i].expr)
			printf("%s %s:\n", i == 0 ? "if" : "elif",
			       branches[i].expr);
		else
			printf("else:\n");
		for (int j = 0; j < branches[i].n_frags; j++)
			dump_frag(&branches[i].frags[j], depth + 1);
	}
}

static void dump_frag(const struct lf_frag *f, int depth)
{
	switch (f->kind) {
	case LF_SECTIONS:
		pr_indent(depth);
		printf("[sections:%s]\n", f->u.sec.name);
		dump_stmts(f->u.sec.stmts, f->u.sec.n, LF_SECTIONS,
			   depth + 1);
		break;
	case LF_SCHEME:
		pr_indent(depth);
		printf("[scheme:%s]\n", f->u.sch.name);
		dump_stmts(f->u.sch.stmts, f->u.sch.n, LF_SCHEME,
			   depth + 1);
		break;
	case LF_MAPPING:
		pr_indent(depth);
		printf("[mapping:%s]\n", f->u.map.name);
		pr_indent(depth + 1);
		printf("archive:\n");
		dump_stmts(f->u.map.archive, f->u.map.n_archive,
			   LF_FRAG_COND, depth + 2);
		dump_stmts(f->u.map.entries, f->u.map.n_entries,
			   LF_MAPPING, depth + 1);
		break;
	case LF_FRAG_COND:
		dump_frag_cond(f->u.cond.branches, f->u.cond.n, depth);
		break;
	}
}

void lf_file_dump(const struct lf_file *f)
{
	printf("--- %s ---\n", f->path);
	for (int i = 0; i < f->n_frags; i++)
		dump_frag(&f->frags[i], 0);
}
