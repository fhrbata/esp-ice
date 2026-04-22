/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_lex.c
 * @brief Kconfig lexer implementation.
 *
 * Scans a NUL-terminated buffer char-by-char and emits one token per
 * kc_lex_next() call.  Three non-trivial pieces:
 *
 *   - Keyword recognition: identifiers matching a small perfect table
 *     are reclassified to their specific KT_* token.
 *
 *   - Quoted strings: @c "..." / @c '...' with \\-escapes and
 *     environment interpolation (@c $(VAR), @c ${VAR}, @c $VAR).
 *
 *   - Help blocks: kc_lex_read_help() switches to a line-based mode
 *     that captures the indent-dedented help body as one KT_HELPTEXT.
 *
 * Kconfig does not use Python-style significant indentation outside
 * help blocks, so the normal lexer loop does NOT emit synthetic
 * INDENT/DEDENT tokens -- blocks are closed explicitly by
 * @c endmenu / @c endchoice / @c endif.
 */
#include "kc_lex.h"
#include "ice.h"

/* ================================================================== */
/*  Character classification                                          */
/* ================================================================== */

static int is_name_start(int c) { return isalpha(c) || c == '_'; }

/* Names are [A-Za-z0-9_-].  Dots/dollars are NOT part of a Kconfig
 * identifier (they appear inside quoted strings or in env-interpolation
 * syntax which is handled separately). */
static int is_name_cont(int c) { return isalnum(c) || c == '_' || c == '-'; }

/* Numeric literal: starts with a digit or a leading minus+digit.  We
 * lex these as a single bareword so the parser sees them uniformly
 * with symbol references; the evaluator parses the payload later. */
static int is_literal_cont(int c)
{
	return isalnum(c) || c == '_' || c == '.' || c == '-' || c == '+';
}

/* ================================================================== */
/*  Keyword table                                                     */
/* ================================================================== */

struct kw {
	const char *name;
	int tok;
};

/*
 * Keyword lookup table.  Small enough that a linear strcmp scan is
 * faster than a hashmap in practice; each sweep over this table
 * happens only for bareword-starting-with-a-letter tokens.
 */
static const struct kw kws[] = {
    {"mainmenu", KT_MAINMENU},
    {"menu", KT_MENU},
    {"endmenu", KT_ENDMENU},
    {"choice", KT_CHOICE},
    {"endchoice", KT_ENDCHOICE},
    {"config", KT_CONFIG},
    {"menuconfig", KT_MENUCONFIG},
    {"if", KT_IF},
    {"endif", KT_ENDIF},
    {"comment", KT_COMMENT},
    {"source", KT_SOURCE},
    {"rsource", KT_RSOURCE},
    {"osource", KT_OSOURCE},
    {"orsource", KT_ORSOURCE},
    {"depends", KT_DEPENDS},
    {"on", KT_ON},
    {"select", KT_SELECT},
    {"imply", KT_IMPLY},
    {"range", KT_RANGE},
    {"default", KT_DEFAULT},
    {"def_bool", KT_DEF_BOOL},
    {"def_int", KT_DEF_INT},
    {"def_hex", KT_DEF_HEX},
    {"def_string", KT_DEF_STRING},
    {"def_tristate", KT_DEF_TRISTATE},
    {"prompt", KT_PROMPT},
    {"help", KT_HELP},
    {"option", KT_OPTION},
    {"visible", KT_VISIBLE},
    {"optional", KT_OPTIONAL},
    {"modules", KT_MODULES},
    {"bool", KT_BOOL},
    {"boolean", KT_BOOL},
    {"int", KT_INT},
    {"hex", KT_HEX},
    {"string", KT_STRING},
    {"float", KT_FLOAT},
    {"tristate", KT_TRISTATE},
};

static int kw_lookup(const char *name)
{
	for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
		if (strcmp(kws[i].name, name) == 0)
			return kws[i].tok;
	}
	return KT_NAME;
}

/* ================================================================== */
/*  Token names (for diagnostics)                                     */
/* ================================================================== */

const char *kc_tok_name(int tok)
{
	switch (tok) {
	case KT_EOF:
		return "EOF";
	case KT_NL:
		return "newline";
	case KT_NAME:
		return "identifier";
	case KT_STR:
		return "string";
	case KT_HELPTEXT:
		return "help-text";
	case KT_LPAREN:
		return "'('";
	case KT_RPAREN:
		return "')'";
	case KT_AND:
		return "'&&'";
	case KT_OR:
		return "'||'";
	case KT_NOT:
		return "'!'";
	case KT_EQ:
		return "'='";
	case KT_NE:
		return "'!='";
	case KT_LT:
		return "'<'";
	case KT_LE:
		return "'<='";
	case KT_GT:
		return "'>'";
	case KT_GE:
		return "'>='";
	}
	/* Keywords -- reverse-lookup from the kws table. */
	for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
		if (kws[i].tok == tok)
			return kws[i].name;
	return "?";
}

void kc_lex_die_unexpected(struct kc_lexer *l, int want)
{
	die("%s:%d: expected %s, got %s", l->path, l->line, kc_tok_name(want),
	    kc_tok_name(l->tok));
}

/* ================================================================== */
/*  Environment interpolation                                         */
/* ================================================================== */

static const char *env_lookup(const struct kc_lexer *l, const char *name,
			      size_t n)
{
	/* First, caller-supplied --env table. */
	if (l->env) {
		for (const char *const *p = l->env; *p; p++) {
			const char *eq = strchr(*p, '=');
			if (!eq)
				continue;
			size_t klen = (size_t)(eq - *p);
			if (klen == n && memcmp(*p, name, n) == 0)
				return eq + 1;
		}
	}
	/* Then the real environment. */
	{
		char buf[256];
		if (n >= sizeof(buf))
			return NULL;
		memcpy(buf, name, n);
		buf[n] = '\0';
		const char *v = getenv(buf);
		if (v)
			return v;
	}
	return NULL;
}

static void env_warn_once(struct kc_lexer *l, const char *name, size_t n)
{
	if (l->warned_undef_env)
		return;
	l->warned_undef_env = 1;
	warn("%s:%d: undefined environment variable '%.*s' expands to empty",
	     l->path, l->line, (int)n, name);
}

/* ================================================================== */
/*  Quoted string lexing                                              */
/* ================================================================== */

/*
 * Lex a quoted string starting at @c l->pos (which points to the
 * opening quote).  The closing quote matches the opening one.  Handles
 * \\-escapes and $VAR / $(VAR) / ${VAR} interpolation in place,
 * producing a heap-allocated, owned string.  Leaves @c l->pos just
 * past the closing quote.
 */
static void lex_quoted(struct kc_lexer *l)
{
	char quote = *l->pos++;
	struct sbuf sb = SBUF_INIT;

	while (*l->pos && *l->pos != quote) {
		if (*l->pos == '\n') {
			sbuf_release(&sb);
			die("%s:%d: unterminated string", l->path, l->line);
		}
		if (*l->pos == '\\' && l->pos[1]) {
			char c = l->pos[1];
			switch (c) {
			case 'n':
				sbuf_addch(&sb, '\n');
				break;
			case 't':
				sbuf_addch(&sb, '\t');
				break;
			case 'r':
				sbuf_addch(&sb, '\r');
				break;
			case '\\':
				sbuf_addch(&sb, '\\');
				break;
			case '"':
				sbuf_addch(&sb, '"');
				break;
			case '\'':
				sbuf_addch(&sb, '\'');
				break;
			default:
				/* Unknown escape -- keep literal. */
				sbuf_addch(&sb, '\\');
				sbuf_addch(&sb, c);
				break;
			}
			l->pos += 2;
			continue;
		}
		if (*l->pos == '$') {
			const char *name;
			size_t n;
			if (l->pos[1] == '(') {
				name = l->pos + 2;
				const char *end = strchr(name, ')');
				if (!end || memchr(name, '\n', end - name)) {
					sbuf_release(&sb);
					die("%s:%d: unterminated $( ) "
					    "reference",
					    l->path, l->line);
				}
				n = (size_t)(end - name);
				l->pos = end + 1;
			} else if (l->pos[1] == '{') {
				name = l->pos + 2;
				const char *end = strchr(name, '}');
				if (!end || memchr(name, '\n', end - name)) {
					sbuf_release(&sb);
					die("%s:%d: unterminated ${ } "
					    "reference",
					    l->path, l->line);
				}
				n = (size_t)(end - name);
				l->pos = end + 1;
			} else if (is_name_start((unsigned char)l->pos[1])) {
				name = l->pos + 1;
				const char *end = name;
				while (is_name_cont((unsigned char)*end))
					end++;
				n = (size_t)(end - name);
				l->pos = end;
			} else {
				/* Bare '$' with nothing recognisable --
				 * keep literal. */
				sbuf_addch(&sb, *l->pos++);
				continue;
			}
			const char *val = env_lookup(l, name, n);
			if (val)
				sbuf_addstr(&sb, val);
			else
				env_warn_once(l, name, n);
			continue;
		}
		sbuf_addch(&sb, *l->pos++);
	}
	if (!*l->pos)
		die("%s:%d: unterminated string at EOF", l->path, l->line);
	l->pos++; /* consume closing quote */

	free(l->val);
	l->val = sbuf_detach(&sb);
}

/* ================================================================== */
/*  Main lexer loop                                                   */
/* ================================================================== */

/* Skip inline whitespace and line-continuation backslashes. */
static void skip_ws(struct kc_lexer *l)
{
	while (*l->pos) {
		if (*l->pos == ' ' || *l->pos == '\t') {
			l->pos++;
			continue;
		}
		if (*l->pos == '\\' && l->pos[1] == '\n') {
			l->pos += 2;
			l->line++;
			continue;
		}
		break;
	}
}

/* Skip a '#' comment to end of line; leaves the newline to the caller. */
static void skip_comment(struct kc_lexer *l)
{
	while (*l->pos && *l->pos != '\n')
		l->pos++;
}

void kc_lex_open(struct kc_lexer *l, const char *src, const char *path,
		 const char *const *env)
{
	l->src = src;
	l->pos = src;
	l->path = path;
	l->line = 1;
	l->tok = 0;
	l->val = NULL;
	l->env = env;
	l->n_frames = 0;
	l->active_buf = NULL; /* root frame's buffer is caller-owned */
	l->eof = 0;
	l->warned_undef_env = 0;
}

void kc_lex_close(struct kc_lexer *l)
{
	/* Pop any still-live included frames (their buffers are ours). */
	while (l->n_frames > 0) {
		free(l->active_buf);
		l->n_frames--;
		struct kc_lex_frame *f = &l->frames[l->n_frames];
		l->src = f->src;
		l->pos = f->pos;
		l->path = f->path;
		l->line = f->line;
		l->active_buf = f->owned_buf;
	}
	free(l->val);
	l->val = NULL;
}

static int kc_lex_pop_frame(struct kc_lexer *l)
{
	if (l->n_frames == 0)
		return 0;
	free(l->active_buf);
	l->n_frames--;
	struct kc_lex_frame *f = &l->frames[l->n_frames];
	l->src = f->src;
	l->pos = f->pos;
	l->path = f->path;
	l->line = f->line;
	l->active_buf = f->owned_buf;
	return 1;
}

int kc_lex_push_file(struct kc_lexer *l, const char *path, int optional)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0) {
		if (optional && errno == ENOENT)
			return -1;
		die_errno("cannot source '%s'", path);
	}
	if (l->n_frames >= (int)(sizeof(l->frames) / sizeof(l->frames[0]))) {
		sbuf_release(&sb);
		die("source nesting too deep at '%s'", path);
	}

	/* Save the active frame. */
	struct kc_lex_frame *f = &l->frames[l->n_frames++];
	f->src = l->src;
	f->pos = l->pos;
	f->path = l->path;
	f->line = l->line;
	f->owned_buf = l->active_buf;

	/* Install the child frame.  The path is stored as a duplicate
	 * here; the caller (kc_parse.c) separately interns it in the
	 * context so diagnostics produced later (e.g. KP_DEPENDS' src_file
	 * set during parsing of the child file) remain valid. */
	char *owned = sbuf_detach(&sb);
	l->active_buf = owned;
	l->src = owned;
	l->pos = owned;
	l->path = path; /* caller must ensure lifetime ≥ parse */
	l->line = 1;
	return 0;
}

int kc_lex_next(struct kc_lexer *l)
{
top:
	if (l->eof)
		return l->tok = KT_EOF;

	skip_ws(l);

	/* Comments and blank lines: collapse runs into a single KT_NL so
	 * the parser never sees multiple consecutive newlines. */
	if (*l->pos == '#')
		skip_comment(l);

	if (*l->pos == '\n') {
		/*
		 * Collapse a run of blank and comment-only lines into a
		 * single KT_NL, but leave @c l->pos at the FIRST character
		 * of the next non-blank line.  Preserving that line's
		 * leading whitespace is critical for kc_lex_read_help(),
		 * which measures base indentation from it.
		 */
		do {
			l->pos++; /* consume the '\n' */
			l->line++;
			const char *q = l->pos;
			while (*q == ' ' || *q == '\t')
				q++;
			if (*q == '\n') {
				/* Blank line -- drop its leading whitespace
				 * and fall through to consume the '\n'. */
				l->pos = q;
			} else if (*q == '#') {
				/* Comment-only line -- swallow the comment;
				 * the trailing '\n' drives the next iter. */
				l->pos = q;
				while (*l->pos && *l->pos != '\n')
					l->pos++;
			} else {
				/* Real content line -- stop WITHOUT eating
				 * its leading whitespace. */
				break;
			}
		} while (*l->pos == '\n');
		return l->tok = KT_NL;
	}

	if (!*l->pos) {
		/* End of active buffer: pop included frame if any, else EOF. */
		if (kc_lex_pop_frame(l))
			goto top;
		l->eof = 1;
		return l->tok = KT_EOF;
	}

	/* Two-character operators. */
	if (l->pos[0] == '&' && l->pos[1] == '&') {
		l->pos += 2;
		return l->tok = KT_AND;
	}
	if (l->pos[0] == '|' && l->pos[1] == '|') {
		l->pos += 2;
		return l->tok = KT_OR;
	}
	if (l->pos[0] == '!' && l->pos[1] == '=') {
		l->pos += 2;
		return l->tok = KT_NE;
	}
	if (l->pos[0] == '<' && l->pos[1] == '=') {
		l->pos += 2;
		return l->tok = KT_LE;
	}
	if (l->pos[0] == '>' && l->pos[1] == '=') {
		l->pos += 2;
		return l->tok = KT_GE;
	}

	/* Single-character operators. */
	switch (*l->pos) {
	case '(':
		l->pos++;
		return l->tok = KT_LPAREN;
	case ')':
		l->pos++;
		return l->tok = KT_RPAREN;
	case '!':
		l->pos++;
		return l->tok = KT_NOT;
	case '=':
		l->pos++;
		return l->tok = KT_EQ;
	case '<':
		l->pos++;
		return l->tok = KT_LT;
	case '>':
		l->pos++;
		return l->tok = KT_GT;
	}

	/* Quoted string. */
	if (*l->pos == '"' || *l->pos == '\'') {
		lex_quoted(l);
		return l->tok = KT_STR;
	}

	/* Identifier / keyword (alpha-start). */
	if (is_name_start((unsigned char)*l->pos)) {
		const char *start = l->pos++;
		while (is_name_cont((unsigned char)*l->pos))
			l->pos++;
		free(l->val);
		l->val = sbuf_strndup(start, (size_t)(l->pos - start));

		/* '---help---' alias for 'help' (legacy kernel syntax). */
		if (strcmp(l->val, "help") == 0)
			return l->tok = KT_HELP;
		int kw = kw_lookup(l->val);
		return l->tok = kw;
	}

	/* Legacy '---help---' literal, if we see triple-dash at BOL. */
	if (l->pos[0] == '-' && l->pos[1] == '-' && l->pos[2] == '-') {
		const char *p = l->pos + 3;
		if (!strncmp(p, "help---", 7)) {
			l->pos = p + 7;
			free(l->val);
			l->val = sbuf_strdup("help");
			return l->tok = KT_HELP;
		}
	}

	/* Numeric literal (starts with digit, optional leading sign). */
	if (isdigit((unsigned char)*l->pos) ||
	    ((l->pos[0] == '-' || l->pos[0] == '+') &&
	     isdigit((unsigned char)l->pos[1]))) {
		const char *start = l->pos++;
		while (is_literal_cont((unsigned char)*l->pos))
			l->pos++;
		free(l->val);
		l->val = sbuf_strndup(start, (size_t)(l->pos - start));
		return l->tok = KT_NAME;
	}

	die("%s:%d: unexpected character '%c' (0x%02x)", l->path, l->line,
	    *l->pos, (unsigned char)*l->pos);
	return KT_EOF;
}

/* ================================================================== */
/*  Help-text reader                                                  */
/* ================================================================== */

/*
 * Called by the parser after accepting @c help and its trailing
 * KT_NL.  The scan position is at the start of the first line after
 * the @c help keyword.
 *
 * Algorithm:
 *  1. Skip leading blank lines.
 *  2. Take the indentation of the first non-blank line as the base.
 *  3. Accumulate lines while they are blank or indented >= base,
 *     stripping the base prefix.
 *  4. Stop at the first non-blank line with indent < base, or EOF.
 *     Leave scan position at the start of that line (or EOF).
 */
void kc_lex_read_help(struct kc_lexer *l)
{
	struct sbuf body = SBUF_INIT;
	int base = -1;

	/* Helper: measure leading whitespace of the line starting at p. */
	for (;;) {
		const char *line_start = l->pos;
		int indent = 0;
		while (*l->pos == ' ' || *l->pos == '\t') {
			indent += (*l->pos == '\t') ? 8 - (indent % 8) : 1;
			l->pos++;
		}
		/* Blank line -- always part of body, emit '\n'. */
		if (*l->pos == '\n' || *l->pos == '\0') {
			if (base >= 0)
				sbuf_addch(&body, '\n');
			if (*l->pos == '\n') {
				l->pos++;
				l->line++;
				continue;
			}
			break; /* EOF */
		}
		/* First non-blank line establishes the base indent. */
		if (base < 0) {
			base = indent;
		}
		if (indent < base) {
			/* Rewind to line start so the main lexer re-reads it.
			 */
			l->pos = line_start;
			break;
		}
		/* Skip `base` columns of leading whitespace for output. */
		{
			const char *p = line_start;
			int col = 0;
			while (col < base && (*p == ' ' || *p == '\t')) {
				col += (*p == '\t') ? 8 - (col % 8) : 1;
				p++;
			}
			/* If a tab crossed the base boundary we may have
			 * consumed more than `base` columns; keep any
			 * leftover as leading spaces. */
			while (col > base) {
				sbuf_addch(&body, ' ');
				col--;
			}
			while (*p && *p != '\n') {
				sbuf_addch(&body, *p);
				p++;
			}
			sbuf_addch(&body, '\n');
			l->pos = p;
			if (*l->pos == '\n') {
				l->pos++;
				l->line++;
			}
		}
	}

	free(l->val);
	l->val = sbuf_detach(&body);
	l->tok = KT_HELPTEXT;
}
