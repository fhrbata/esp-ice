/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file yaml.c
 * @brief Small in-house YAML DOM reader.
 *
 * Indentation-driven recursive-descent parser covering the subset of
 * YAML needed for hints.yml and similar config-like documents.  See
 * yaml.h for the feature list and scope limits.
 */
#include "yaml.h"
#include "ice.h"

/* ------------------------------------------------------------------ */
/*  Construction                                                      */
/* ------------------------------------------------------------------ */

static struct yaml_value *make(enum yaml_type t)
{
	struct yaml_value *y = calloc(1, sizeof(*y));

	if (!y)
		die_errno("calloc");
	y->type = t;
	return y;
}

static struct yaml_value *new_null(void) { return make(YAML_NULL); }
static struct yaml_value *new_seq(void) { return make(YAML_SEQ); }
static struct yaml_value *new_map(void) { return make(YAML_MAP); }

static struct yaml_value *new_bool(int b)
{
	struct yaml_value *y = make(YAML_BOOL);

	y->u.boolean = b ? 1 : 0;
	return y;
}

static struct yaml_value *new_string(char *s_owned)
{
	struct yaml_value *y = make(YAML_STRING);

	y->u.string = s_owned;
	return y;
}

static void seq_push(struct yaml_value *seq, struct yaml_value *item)
{
	ALLOC_GROW(seq->u.seq.items, seq->u.seq.nr + 1, seq->u.seq.alloc);
	seq->u.seq.items[seq->u.seq.nr++] = item;
}

static void map_set(struct yaml_value *map, char *key_owned,
		    struct yaml_value *value)
{
	ALLOC_GROW(map->u.map.members, map->u.map.nr + 1, map->u.map.alloc);
	map->u.map.members[map->u.map.nr].key = key_owned;
	map->u.map.members[map->u.map.nr].value = value;
	map->u.map.nr++;
}

/* ------------------------------------------------------------------ */
/*  Release                                                           */
/* ------------------------------------------------------------------ */

void yaml_free(struct yaml_value *y)
{
	if (!y)
		return;

	switch (y->type) {
	case YAML_STRING:
		free(y->u.string);
		break;
	case YAML_SEQ:
		for (int i = 0; i < y->u.seq.nr; i++)
			yaml_free(y->u.seq.items[i]);
		free(y->u.seq.items);
		break;
	case YAML_MAP:
		for (int i = 0; i < y->u.map.nr; i++) {
			free(y->u.map.members[i].key);
			yaml_free(y->u.map.members[i].value);
		}
		free(y->u.map.members);
		break;
	case YAML_NULL:
	case YAML_BOOL:
		break;
	}
	free(y);
}

/* ------------------------------------------------------------------ */
/*  Accessors                                                         */
/* ------------------------------------------------------------------ */

enum yaml_type yaml_type(const struct yaml_value *y)
{
	return y ? y->type : YAML_NULL;
}

const char *yaml_as_string(const struct yaml_value *y)
{
	return (y && y->type == YAML_STRING) ? y->u.string : NULL;
}

int yaml_as_bool(const struct yaml_value *y)
{
	return (y && y->type == YAML_BOOL) ? y->u.boolean : 0;
}

struct yaml_value *yaml_get(const struct yaml_value *obj, const char *key)
{
	if (!obj || obj->type != YAML_MAP)
		return NULL;
	for (int i = 0; i < obj->u.map.nr; i++) {
		if (!strcmp(obj->u.map.members[i].key, key))
			return obj->u.map.members[i].value;
	}
	return NULL;
}

int yaml_seq_size(const struct yaml_value *seq)
{
	return (seq && seq->type == YAML_SEQ) ? seq->u.seq.nr : 0;
}

struct yaml_value *yaml_seq_at(const struct yaml_value *seq, int idx)
{
	if (!seq || seq->type != YAML_SEQ)
		return NULL;
	if (idx < 0 || idx >= seq->u.seq.nr)
		return NULL;
	return seq->u.seq.items[idx];
}

/* ------------------------------------------------------------------ */
/*  Parser                                                            */
/* ------------------------------------------------------------------ */

struct parser {
	const char *buf;
	size_t len;
	size_t pos;
	int failed;
};

#define FAIL(ps)                                                               \
	do {                                                                   \
		(ps)->failed = 1;                                              \
		return NULL;                                                   \
	} while (0)

static int at_eof(const struct parser *ps) { return ps->pos >= ps->len; }

static int peek(const struct parser *ps)
{
	return at_eof(ps) ? -1 : (unsigned char)ps->buf[ps->pos];
}

static int peek_at(const struct parser *ps, size_t off)
{
	return (ps->pos + off >= ps->len)
		   ? -1
		   : (unsigned char)ps->buf[ps->pos + off];
}

static void advance(struct parser *ps)
{
	if (!at_eof(ps))
		ps->pos++;
}

static int at_newline(const struct parser *ps)
{
	int c = peek(ps);
	return c == '\n' || c == '\r';
}

static void skip_newline(struct parser *ps)
{
	if (peek(ps) == '\r')
		advance(ps);
	if (peek(ps) == '\n')
		advance(ps);
}

static void skip_inline_ws(struct parser *ps)
{
	while (peek(ps) == ' ' || peek(ps) == '\t')
		advance(ps);
}

static void skip_to_eol(struct parser *ps)
{
	while (!at_eof(ps) && peek(ps) != '\n' && peek(ps) != '\r')
		advance(ps);
}

/*
 * Skip past any sequence of blank lines and comment-only lines so the
 * next read lands on content (or EOF).  Returns nothing; callers
 * re-inspect state.
 */
static void skip_blank_lines(struct parser *ps)
{
	while (!at_eof(ps)) {
		size_t save = ps->pos;
		skip_inline_ws(ps);
		if (peek(ps) == '#') {
			skip_to_eol(ps);
			skip_newline(ps);
			continue;
		}
		if (at_newline(ps)) {
			skip_newline(ps);
			continue;
		}
		if (at_eof(ps))
			break;
		/* Real content -- rewind to start of line whitespace. */
		ps->pos = save;
		break;
	}
}

/*
 * At start of line, return the number of leading space characters.
 * Caller is responsible for having already skipped blank/comment
 * lines via skip_blank_lines().  Tabs are *not* treated as indentation
 * (YAML disallows them there); we just pass through and let the
 * downstream tokenizer report the oddity as a parse error.
 */
static int line_indent(const struct parser *ps)
{
	int n = 0;
	while (peek_at(ps, n) == ' ')
		n++;
	return n;
}

static void consume_spaces(struct parser *ps, int n) { ps->pos += n; }

/* After inline content on a line, allow trailing whitespace + optional
 * comment before the newline.  Returns 0 on success, -1 if stray
 * non-whitespace is found. */
static int end_of_line(struct parser *ps)
{
	skip_inline_ws(ps);
	if (peek(ps) == '#')
		skip_to_eol(ps);
	if (at_eof(ps))
		return 0;
	if (at_newline(ps)) {
		skip_newline(ps);
		return 0;
	}
	return -1;
}

/* Parse a double-quoted string scalar.  Caller has verified the opening
 * '"'; we advance past it, collect the content (handling escapes),
 * and consume the closing '"'. */
static char *parse_dquoted(struct parser *ps)
{
	struct sbuf sb = SBUF_INIT;

	advance(ps); /* opening " */
	while (!at_eof(ps)) {
		int c = peek(ps);
		if (c == '"') {
			advance(ps);
			return sbuf_detach(&sb);
		}
		if (c == '\\') {
			advance(ps);
			int e = peek(ps);
			switch (e) {
			case '"':
				sbuf_addch(&sb, '"');
				break;
			case '\\':
				sbuf_addch(&sb, '\\');
				break;
			case '/':
				sbuf_addch(&sb, '/');
				break;
			case 'n':
				sbuf_addch(&sb, '\n');
				break;
			case 't':
				sbuf_addch(&sb, '\t');
				break;
			case 'r':
				sbuf_addch(&sb, '\r');
				break;
			case 'b':
				sbuf_addch(&sb, '\b');
				break;
			case 'f':
				sbuf_addch(&sb, '\f');
				break;
			case '0':
				sbuf_addch(&sb, '\0');
				break;
			default:
				/* Unknown escape -- preserve verbatim. */
				sbuf_addch(&sb, '\\');
				if (e < 0)
					goto fail;
				sbuf_addch(&sb, e);
				break;
			}
			advance(ps);
			continue;
		}
		if (c == '\n' || c == '\r')
			goto fail; /* no multi-line scalars */
		sbuf_addch(&sb, c);
		advance(ps);
	}
fail:
	sbuf_release(&sb);
	ps->failed = 1;
	return NULL;
}

/* Parse a single-quoted string scalar.  '' is an escape for a literal '. */
static char *parse_squoted(struct parser *ps)
{
	struct sbuf sb = SBUF_INIT;

	advance(ps); /* opening ' */
	while (!at_eof(ps)) {
		int c = peek(ps);
		if (c == '\'') {
			advance(ps);
			if (peek(ps) == '\'') {
				sbuf_addch(&sb, '\'');
				advance(ps);
				continue;
			}
			return sbuf_detach(&sb);
		}
		if (c == '\n' || c == '\r')
			goto fail;
		sbuf_addch(&sb, c);
		advance(ps);
	}
fail:
	sbuf_release(&sb);
	ps->failed = 1;
	return NULL;
}

/* Parse a plain (unquoted) scalar.  In block context, runs to end of
 * line (trailing whitespace trimmed).  In flow context, also terminates
 * on ',' or ']'. */
static char *parse_plain(struct parser *ps, int in_flow)
{
	struct sbuf sb = SBUF_INIT;

	while (!at_eof(ps)) {
		int c = peek(ps);
		if (c == '\n' || c == '\r')
			break;
		if (in_flow && (c == ',' || c == ']'))
			break;
		if (c == '#' && sb.len > 0 &&
		    (sb.buf[sb.len - 1] == ' ' || sb.buf[sb.len - 1] == '\t'))
			break; /* ' #' starts a comment */
		sbuf_addch(&sb, c);
		advance(ps);
	}
	/* Trim trailing whitespace. */
	while (sb.len > 0 &&
	       (sb.buf[sb.len - 1] == ' ' || sb.buf[sb.len - 1] == '\t'))
		sb.buf[--sb.len] = '\0';

	if (sb.len == 0) {
		sbuf_release(&sb);
		return sbuf_strdup("");
	}
	return sbuf_detach(&sb);
}

/* Classify a plain scalar: true/false -> bool, everything else -> string. */
static struct yaml_value *scalar_from_plain(char *text_owned)
{
	struct yaml_value *y;

	if (!strcmp(text_owned, "true") || !strcmp(text_owned, "True") ||
	    !strcmp(text_owned, "TRUE")) {
		free(text_owned);
		return new_bool(1);
	}
	if (!strcmp(text_owned, "false") || !strcmp(text_owned, "False") ||
	    !strcmp(text_owned, "FALSE")) {
		free(text_owned);
		return new_bool(0);
	}
	if (!strcmp(text_owned, "null") || !strcmp(text_owned, "Null") ||
	    !strcmp(text_owned, "NULL") || !strcmp(text_owned, "~") ||
	    !*text_owned) {
		free(text_owned);
		return new_null();
	}
	y = new_string(text_owned);
	return y;
}

/* Forward decls. */
static struct yaml_value *parse_node(struct parser *ps, int min_indent);
static struct yaml_value *parse_flow_seq(struct parser *ps);

/* Parse a flow-sequence scalar item (quoted or plain).  Caller has
 * already skipped leading whitespace inside the `[...]`. */
static struct yaml_value *parse_flow_scalar(struct parser *ps)
{
	int c = peek(ps);
	char *text;

	if (c == '"') {
		text = parse_dquoted(ps);
		if (!text)
			return NULL;
		return new_string(text);
	}
	if (c == '\'') {
		text = parse_squoted(ps);
		if (!text)
			return NULL;
		return new_string(text);
	}
	if (c == '[') /* nested flow seq */
		return parse_flow_seq(ps);
	text = parse_plain(ps, 1);
	if (!text)
		return NULL;
	return scalar_from_plain(text);
}

static struct yaml_value *parse_flow_seq(struct parser *ps)
{
	struct yaml_value *seq = new_seq();

	advance(ps); /* opening [ */
	for (;;) {
		/* Skip whitespace, including newlines inside the flow. */
		while (peek(ps) == ' ' || peek(ps) == '\t' ||
		       peek(ps) == '\n' || peek(ps) == '\r')
			advance(ps);
		if (peek(ps) == ']') {
			advance(ps);
			return seq;
		}
		if (at_eof(ps))
			goto fail;
		{
			struct yaml_value *item = parse_flow_scalar(ps);
			if (!item)
				goto fail;
			seq_push(seq, item);
		}
		while (peek(ps) == ' ' || peek(ps) == '\t' ||
		       peek(ps) == '\n' || peek(ps) == '\r')
			advance(ps);
		if (peek(ps) == ',') {
			advance(ps);
			continue;
		}
		if (peek(ps) == ']') {
			advance(ps);
			return seq;
		}
		goto fail;
	}
fail:
	yaml_free(seq);
	ps->failed = 1;
	return NULL;
}

/* Parse an inline value starting at the current position (after a ':'
 * or '-' consumer).  Handles quoted scalars, flow sequences, and plain
 * scalars running to EOL.  Consumes the trailing newline. */
static struct yaml_value *parse_inline(struct parser *ps)
{
	int c = peek(ps);
	struct yaml_value *y;
	char *text;

	if (c == '"') {
		text = parse_dquoted(ps);
		if (!text)
			return NULL;
		y = new_string(text);
		if (end_of_line(ps) < 0) {
			yaml_free(y);
			ps->failed = 1;
			return NULL;
		}
		return y;
	}
	if (c == '\'') {
		text = parse_squoted(ps);
		if (!text)
			return NULL;
		y = new_string(text);
		if (end_of_line(ps) < 0) {
			yaml_free(y);
			ps->failed = 1;
			return NULL;
		}
		return y;
	}
	if (c == '[') {
		y = parse_flow_seq(ps);
		if (!y)
			return NULL;
		if (end_of_line(ps) < 0) {
			yaml_free(y);
			ps->failed = 1;
			return NULL;
		}
		return y;
	}
	text = parse_plain(ps, 0);
	if (!text)
		return NULL;
	y = scalar_from_plain(text);
	if (end_of_line(ps) < 0) {
		yaml_free(y);
		ps->failed = 1;
		return NULL;
	}
	return y;
}

/*
 * Does the remainder of the current line look like a block-mapping key?
 * Specifically, is there an unquoted ':' on this line followed by a
 * space or newline?  Walks a local copy of the cursor without consuming
 * input.
 */
static int line_is_map_key(const struct parser *ps)
{
	size_t i = ps->pos;
	int in_quote = 0;
	int qchar = 0;

	while (i < ps->len) {
		int c = (unsigned char)ps->buf[i];
		if (c == '\n' || c == '\r')
			return 0;
		if (in_quote) {
			if (c == '\\' && qchar == '"' && i + 1 < ps->len)
				i += 2;
			else if (c == qchar)
				in_quote = 0, i++;
			else
				i++;
			continue;
		}
		if (c == '"' || c == '\'') {
			in_quote = 1;
			qchar = c;
			i++;
			continue;
		}
		if (c == '#')
			return 0;
		if (c == ':') {
			int next = (i + 1 < ps->len)
				       ? (unsigned char)ps->buf[i + 1]
				       : -1;
			if (next == -1 || next == ' ' || next == '\t' ||
			    next == '\n' || next == '\r')
				return 1;
		}
		i++;
	}
	return 0;
}

/* Read a scalar token to use as a mapping key.  Consumes the token but
 * not the trailing ':'. */
static char *parse_map_key_token(struct parser *ps)
{
	int c = peek(ps);
	if (c == '"')
		return parse_dquoted(ps);
	if (c == '\'')
		return parse_squoted(ps);

	struct sbuf sb = SBUF_INIT;
	while (!at_eof(ps)) {
		int ch = peek(ps);
		if (ch == ':') {
			int next = peek_at(ps, 1);
			if (next == -1 || next == ' ' || next == '\t' ||
			    next == '\n' || next == '\r')
				break;
		}
		if (ch == '\n' || ch == '\r' || ch == '#')
			break;
		sbuf_addch(&sb, ch);
		advance(ps);
	}
	while (sb.len > 0 &&
	       (sb.buf[sb.len - 1] == ' ' || sb.buf[sb.len - 1] == '\t'))
		sb.buf[--sb.len] = '\0';
	if (sb.len == 0) {
		sbuf_release(&sb);
		ps->failed = 1;
		return NULL;
	}
	return sbuf_detach(&sb);
}

/* Parse a block mapping rooted at indent `indent`.  Caller guarantees
 * the first key is at column `indent`. */
static struct yaml_value *parse_block_map(struct parser *ps, int indent)
{
	struct yaml_value *map = new_map();

	for (;;) {
		skip_blank_lines(ps);
		if (at_eof(ps))
			return map;

		int cur = line_indent(ps);
		if (cur < indent)
			return map;
		if (cur > indent) {
			/* Inconsistent indent -- reject. */
			goto fail;
		}

		consume_spaces(ps, cur);

		/* Must look like a key:value line. */
		if (!line_is_map_key(ps))
			return map;

		char *key = parse_map_key_token(ps);
		if (!key)
			goto fail;

		if (peek(ps) != ':') {
			free(key);
			goto fail;
		}
		advance(ps); /* consume ':' */
		skip_inline_ws(ps);

		struct yaml_value *val;
		if (at_newline(ps) || peek(ps) == '#' || at_eof(ps)) {
			if (peek(ps) == '#')
				skip_to_eol(ps);
			skip_newline(ps);
			/* Value is on following lines at greater indent. */
			skip_blank_lines(ps);
			if (at_eof(ps)) {
				val = new_null();
			} else {
				int nxt = line_indent(ps);
				if (nxt <= indent)
					val = new_null();
				else
					val = parse_node(ps, nxt);
			}
		} else {
			val = parse_inline(ps);
		}

		if (!val) {
			free(key);
			goto fail;
		}
		map_set(map, key, val);
	}
fail:
	yaml_free(map);
	ps->failed = 1;
	return NULL;
}

/* Parse a block sequence rooted at indent `indent`.  Caller guarantees
 * the first '-' is at column `indent`. */
static struct yaml_value *parse_block_seq(struct parser *ps, int indent)
{
	struct yaml_value *seq = new_seq();

	for (;;) {
		skip_blank_lines(ps);
		if (at_eof(ps))
			return seq;

		int cur = line_indent(ps);
		if (cur < indent)
			return seq;
		if (cur > indent)
			goto fail;

		consume_spaces(ps, cur);
		if (peek(ps) != '-')
			return seq;
		{
			int next = peek_at(ps, 1);
			if (next != ' ' && next != '\t' && next != '\n' &&
			    next != '\r' && next != -1)
				return seq; /* '-foo' is a plain scalar */
		}
		advance(ps); /* consume '-' */
		skip_inline_ws(ps);

		struct yaml_value *val;
		if (at_newline(ps) || peek(ps) == '#' || at_eof(ps)) {
			if (peek(ps) == '#')
				skip_to_eol(ps);
			skip_newline(ps);
			skip_blank_lines(ps);
			if (at_eof(ps)) {
				val = new_null();
			} else {
				int nxt = line_indent(ps);
				if (nxt <= indent)
					val = new_null();
				else
					val = parse_node(ps, nxt);
			}
		} else {
			val = parse_inline(ps);
		}
		if (!val)
			goto fail;
		seq_push(seq, val);
	}
fail:
	yaml_free(seq);
	ps->failed = 1;
	return NULL;
}

/*
 * Parse a node at the current position, which must start a non-blank
 * line already indented by exactly @p min_indent spaces (indent-marker
 * spaces not yet consumed).  Dispatches on the first real character.
 */
static struct yaml_value *parse_node(struct parser *ps, int min_indent)
{
	skip_blank_lines(ps);
	if (at_eof(ps))
		return new_null();

	int cur = line_indent(ps);
	if (cur < min_indent) {
		ps->failed = 1;
		return NULL;
	}

	/* Peek past indent without consuming -- we pass cur into parse_block_*.
	 */
	consume_spaces(ps, cur);
	int c = peek(ps);

	if (c == '-') {
		int n = peek_at(ps, 1);
		if (n == ' ' || n == '\t' || n == '\n' || n == '\r' ||
		    n == -1) {
			/* Put back the indent -- parse_block_seq re-reads it.
			 */
			ps->pos -= cur;
			return parse_block_seq(ps, cur);
		}
	}
	if (c == '[') {
		struct yaml_value *y = parse_flow_seq(ps);
		if (!y)
			return NULL;
		if (end_of_line(ps) < 0) {
			yaml_free(y);
			ps->failed = 1;
			return NULL;
		}
		return y;
	}

	/* Peek for a mapping key without consuming. */
	if (line_is_map_key(ps)) {
		ps->pos -= cur;
		return parse_block_map(ps, cur);
	}

	/* Plain scalar on a line by itself. */
	char *text;
	if (c == '"') {
		text = parse_dquoted(ps);
		if (!text)
			return NULL;
		if (end_of_line(ps) < 0)
			goto fail_free_text;
		return new_string(text);
	}
	if (c == '\'') {
		text = parse_squoted(ps);
		if (!text)
			return NULL;
		if (end_of_line(ps) < 0)
			goto fail_free_text;
		return new_string(text);
	}
	text = parse_plain(ps, 0);
	if (!text)
		return NULL;
	if (end_of_line(ps) < 0)
		goto fail_free_text;
	return scalar_from_plain(text);

fail_free_text:
	free(text);
	ps->failed = 1;
	return NULL;
}

struct yaml_value *yaml_parse(const char *buf, size_t len)
{
	struct parser ps = {.buf = buf, .len = len, .pos = 0, .failed = 0};
	struct yaml_value *root;

	skip_blank_lines(&ps);
	if (at_eof(&ps))
		return new_null();

	root = parse_node(&ps, 0);
	if (!root || ps.failed) {
		yaml_free(root);
		return NULL;
	}

	skip_blank_lines(&ps);
	if (!at_eof(&ps)) {
		yaml_free(root);
		return NULL;
	}
	return root;
}
