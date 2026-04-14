/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file help.c
 * @brief Manual page renderer.
 */
#include "ice.h"

#ifndef _WIN32
#include <fcntl.h>
#endif

#define INDENT   "    "
#define INDENT_N 4
#define MIN_WIDTH 40

static int is_bool_opt(enum option_type t)
{
	return t == OPTION_BOOL || t == OPTION_CONFIG_BOOL;
}

static int has_options(const struct option *opts)
{
	return opts && opts->type != OPTION_END;
}

/*
 * Best-effort detection of the user's terminal width even when
 * stdout is redirected.  We try: each std* fd if it happens to be
 * a tty, then /dev/tty (POSIX), then $COLUMNS, and finally 80.
 */
static int detect_width(void)
{
	const int fds[] = { STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO };
	const char *cols;

	for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++)
		if (isatty(fds[i]))
			return term_width(fds[i]);

#ifndef _WIN32
	int fd = open("/dev/tty", O_RDONLY);
	if (fd >= 0) {
		int w = term_width(fd);
		close(fd);
		return w;
	}
#endif

	cols = getenv("COLUMNS");
	if (cols) {
		int w = atoi(cols);
		if (w > 0)
			return w;
	}

	return 80;
}

static int render_width(void)
{
	int w = detect_width();

	if (w < MIN_WIDTH)
		w = MIN_WIDTH;
	return w;
}

/*
 * Visible width (in terminal columns) of @p len bytes at @p s,
 * skipping @x{...} / @[...]{...} markup bytes and counting UTF-8
 * sequences as one column per code point (the fallback for our
 * purposes; full wcwidth is overkill here).
 */
static size_t visible_len(const char *s, size_t len)
{
	size_t vw = 0;
	size_t i = 0;
	size_t depth = 0;

	while (i < len) {
		if (s[i] == '@' && i + 1 < len && s[i + 1] == '@') {
			vw++;
			i += 2;
		} else if (s[i] == '@' && i + 2 < len && s[i + 2] == '{') {
			/* @x{ -- opening marker, 3 invisible bytes */
			i += 3;
			depth++;
		} else if (s[i] == '@' && i + 1 < len && s[i + 1] == '[') {
			/* @[spec]{ -- skip to the '{' after ']' */
			i += 2;
			while (i < len && s[i] != ']')
				i++;
			if (i < len)
				i++;
			if (i < len && s[i] == '{') {
				i++;
				depth++;
			}
		} else if (depth > 0 && s[i] == '}' && i + 1 < len &&
			   s[i + 1] == '}') {
			/* "}}" -- literal } inside a token */
			vw++;
			i += 2;
		} else if (depth > 0 && s[i] == '}') {
			depth--;
			i++;
		} else if (((unsigned char)s[i] & 0xc0) == 0x80) {
			/* UTF-8 continuation -- part of the previous code point */
			i++;
		} else {
			vw++;
			i++;
		}
	}
	return vw;
}

/*
 * Scan forward from @p p to the end of the next "word" -- a run of
 * non-whitespace bytes, where @x{...} / @[...]{...} tokens are
 * treated atomically so spaces inside them don't split the word.
 */
static const char *word_end(const char *p)
{
	while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
		if (*p == '@' && p[1] == '@') {
			p += 2;
		} else if (*p == '@' && p[1] && p[2] == '{') {
			p += 3;
			int depth = 1;
			while (*p && depth > 0) {
				if (*p == '}' && p[1] == '}')
					p += 2;
				else if (*p == '}') {
					depth--;
					p++;
				} else {
					p++;
				}
			}
		} else if (*p == '@' && p[1] == '[') {
			p += 2;
			while (*p && *p != ']')
				p++;
			if (*p == ']')
				p++;
			if (*p == '{')
				p++;
			int depth = 1;
			while (*p && depth > 0) {
				if (*p == '}' && p[1] == '}')
					p += 2;
				else if (*p == '}') {
					depth--;
					p++;
				} else {
					p++;
				}
			}
		} else {
			p++;
		}
	}
	return p;
}

static void emit_line(const struct sbuf *line)
{
	fputs(INDENT, stdout);
	fputs(line->buf, stdout);
	fputs("\n", stdout);
}

/*
 * Word-wrap @p text to the current terminal width, indenting every
 * output line by INDENT_N spaces.  Paragraph breaks (two consecutive
 * newlines in the source) are preserved as blank lines.  @x{...}
 * color tokens pass through to fputs and are expanded there.
 */
static void print_reflowed(const char *text)
{
	struct sbuf line = SBUF_INIT;
	size_t line_vw = 0;
	int width = render_width();
	int content_w = width - INDENT_N;
	int need_blank = 0;
	const char *p = text;

	while (*p) {
		int nls = 0;

		while (*p == ' ' || *p == '\t' || *p == '\n') {
			if (*p == '\n')
				nls++;
			p++;
		}
		if (!*p)
			break;

		if (nls >= 2 && line.len > 0) {
			emit_line(&line);
			sbuf_reset(&line);
			line_vw = 0;
			need_blank = 1;
		}

		if (need_blank && line.len == 0) {
			fputs("\n", stdout);
			need_blank = 0;
		}

		const char *ws = p;
		const char *we = word_end(p);
		size_t wlen = we - ws;
		size_t vw = visible_len(ws, wlen);
		int needs_space = line.len > 0;

		if (needs_space &&
		    (int)(line_vw + 1 + vw) > content_w) {
			emit_line(&line);
			sbuf_reset(&line);
			line_vw = 0;
			needs_space = 0;
		}

		if (needs_space) {
			sbuf_addch(&line, ' ');
			line_vw += 1;
		}
		sbuf_add(&line, ws, wlen);
		line_vw += vw;
		p = we;
	}

	if (line.len > 0)
		emit_line(&line);
	sbuf_release(&line);
}

/*
 * Ensure a section body emits a trailing blank line, so the next
 * heading is visually separated without us needing to micromanage
 * spacing in every H_* macro.
 */
static void print_body(const char *body)
{
	size_t len;

	if (!body || !*body)
		return;

	fputs(body, stdout);

	len = strlen(body);
	if (body[len - 1] != '\n')
		fputs("\n\n", stdout);
	else if (len < 2 || body[len - 2] != '\n')
		fputs("\n", stdout);
}

static void print_options_body(const struct option *opts)
{
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		fputs(INDENT, stdout);
		if (o->short_opt)
			printf("@b{-%c}", o->short_opt);
		if (o->short_opt && o->long_opt)
			fputs(", ", stdout);
		if (o->long_opt) {
			if (is_bool_opt(o->type))
				printf("@b{--%s}", o->long_opt);
			else
				printf("@b{--%s}=<%s>", o->long_opt,
				       o->argh ? o->argh : "...");
		}
		fputs("\n", stdout);
		if (o->help)
			printf(INDENT INDENT "%s\n", o->help);
		fputs("\n", stdout);
	}
}

static void print_commands_body(void)
{
	size_t maxlen = 0;

	for (const struct cmd_struct *c = ice_commands; c->name; c++) {
		size_t l = strlen(c->name);
		if (l > maxlen)
			maxlen = l;
	}

	for (const struct cmd_struct *c = ice_commands; c->name; c++) {
		printf(INDENT "@b{%-*s}   %s\n",
		       (int)maxlen, c->name,
		       c->summary ? c->summary : "");
	}
	fputs("\n", stdout);
}

/*
 * Walk the active config for "alias.<name>" entries and dedupe by
 * name (the same alias may be set in multiple scopes).  Populates
 * @p names with unique alias names in first-occurrence order.
 */
static void collect_aliases(struct svec *names)
{
	for (int i = 0; i < config.nr; i++) {
		const char *key = config.entries[i].key;
		const char *name;
		int dup = 0;

		if (strncmp(key, "alias.", 6) != 0)
			continue;
		name = key + 6;
		if (!*name)
			continue;

		for (size_t j = 0; j < names->nr; j++)
			if (!strcmp(names->v[j], name)) {
				dup = 1;
				break;
			}
		if (!dup)
			svec_push(names, name);
	}
}

static void print_aliases_body(const struct svec *names)
{
	size_t maxlen = 0;

	for (size_t i = 0; i < names->nr; i++)
		if (strlen(names->v[i]) > maxlen)
			maxlen = strlen(names->v[i]);

	for (size_t i = 0; i < names->nr; i++) {
		struct sbuf key = SBUF_INIT;
		const char *val;

		sbuf_addf(&key, "alias.%s", names->v[i]);
		val = config_get(key.buf);
		sbuf_release(&key);

		printf(INDENT "@b{%-*s}   %s\n",
		       (int)maxlen, names->v[i], val ? val : "");
	}
	fputs("\n", stdout);
}

/*
 * Strip any leading path components from argv[0] so the NAME line
 * shows "ice" / "ice-config" rather than "/usr/bin/ice".
 */
static const char *basename_of(const char *p)
{
	const char *last;

	if (!p)
		return NULL;
	last = p;
	for (; *p; p++)
		if (*p == '/' || *p == '\\')
			last = p + 1;
	return last;
}

void print_manual(const char *cmd_name,
		  const struct cmd_manual *m,
		  const struct option *opts,
		  const char **usage)
{
	const char *summary;

	cmd_name = basename_of(cmd_name);
	summary = m && m->summary
			  ? m->summary
			  : (cmd_name ? ice_cmd_summary(cmd_name) : NULL);

	pager_start();

	/* NAME */
	fputs("@b{NAME}\n" INDENT, stdout);
	if (cmd_name && strcmp(cmd_name, "ice") != 0)
		printf("ice-%s", cmd_name);
	else
		fputs("ice", stdout);
	if (summary)
		printf(" - %s", summary);
	fputs("\n\n", stdout);

	/* SYNOPSIS */
	if (usage && usage[0]) {
		fputs("@b{SYNOPSIS}\n", stdout);
		for (int i = 0; usage[i]; i++)
			printf(INDENT "%s\n", usage[i]);
		fputs("\n", stdout);
	}

	/* DESCRIPTION -- prose, reflowed to terminal width. */
	if (m && m->description) {
		fputs("@b{DESCRIPTION}\n", stdout);
		print_reflowed(m->description);
		fputs("\n", stdout);
	}

	/* OPTIONS -- auto-generated from the option table. */
	if (has_options(opts)) {
		fputs("@b{OPTIONS}\n", stdout);
		print_options_body(opts);
	}

	/* COMMANDS -- only for the top-level manual. */
	if (m && m->list_commands) {
		fputs("@b{COMMANDS}\n", stdout);
		print_commands_body();
	}

	/* ALIASES -- skipped entirely when no aliases are configured. */
	if (m && m->list_aliases) {
		struct svec names = SVEC_INIT;

		collect_aliases(&names);
		if (names.nr > 0) {
			fputs("@b{ALIASES}\n", stdout);
			print_aliases_body(&names);
		}
		svec_clear(&names);
	}

	/* EXAMPLES -- verbatim, indented. */
	if (m && m->examples) {
		fputs("@b{EXAMPLES}\n", stdout);
		print_body(m->examples);
	}

	/* EXTRAS -- verbatim, author-structured. */
	if (m && m->extras)
		print_body(m->extras);
}
