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

#define INDENT "    "
#define MIN_WIDTH 40

static int is_bool_opt(enum option_type t) { return t == OPTION_BOOL; }

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
	const int fds[] = {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO};
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
		/* "@@" (anywhere) or "}}" (inside a token) -- doubled char
		 * that stands for one literal, visible character. */
		if ((s[i] == '@' && i + 1 < len && s[i + 1] == '@') ||
		    (depth > 0 && s[i] == '}' && i + 1 < len &&
		     s[i + 1] == '}')) {
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
		} else if (depth > 0 && s[i] == '}') {
			depth--;
			i++;
		} else if (((unsigned char)s[i] & 0xc0) == 0x80) {
			/* UTF-8 continuation -- part of the previous code point
			 */
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
	while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\x02' &&
	       *p != '\x03') {
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

/*
 * Word-wrap the bytes between H_R_BEG...H_R_END into @p out, at the
 * indent encoded right after H_R_BEG.  Advances @p *pp past H_R_END
 * (or to the terminating NUL if the region is unterminated).
 */
static void reflow_region(struct sbuf *out, const char **pp, int width)
{
	const char *p = *pp;
	int indent = (unsigned char)*p;
	int content_w;
	int col = 0;
	int first = 1;

	if (!indent) {
		*pp = p;
		return;
	}
	p++; /* skip the indent byte */

	content_w = width - indent;
	if (content_w < 10)
		content_w = 10;

	while (*p && *p != '\x03') {
		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		if (!*p || *p == '\x03')
			break;

		const char *ws = p;
		const char *we = word_end(p);
		size_t wlen = we - ws;
		size_t vw = visible_len(ws, wlen);

		if (first) {
			for (int i = 0; i < indent; i++)
				sbuf_addch(out, ' ');
			sbuf_add(out, ws, wlen);
			col = vw;
			first = 0;
		} else if (col + 1 + (int)vw > content_w) {
			sbuf_addch(out, '\n');
			for (int i = 0; i < indent; i++)
				sbuf_addch(out, ' ');
			sbuf_add(out, ws, wlen);
			col = vw;
		} else {
			sbuf_addch(out, ' ');
			sbuf_add(out, ws, wlen);
			col += 1 + (int)vw;
		}

		p = we;
	}

	if (*p == '\x03')
		p++;
	*pp = p;
}

/*
 * Render a body section to stdout.  Bytes outside the H_R_BEG..H_R_END
 * markers are copied verbatim; bytes inside are word-wrapped to the
 * terminal width at the indent carried by the marker.  Both modes run
 * through fputs so @x{...} color tokens are expanded normally.
 *
 * One renderer covers .description, .examples, and .extras -- authors
 * opt into wrapping via H_PARA / H_ITEM and get verbatim layout from
 * H_LINE / H_RAW / H_EXAMPLE.
 */
static void print_text(const char *body)
{
	struct sbuf out = SBUF_INIT;
	const char *p = body;
	int width;

	if (!body || !*body)
		return;

	width = render_width();

	while (*p) {
		if (*p == '\x02') {
			p++;
			reflow_region(&out, &p, width);
		} else {
			sbuf_addch(&out, *p);
			p++;
		}
	}

	/*
	 * Guarantee one blank line after the section so the next heading
	 * is visually separated without every macro managing its own
	 * trailing newlines.
	 */
	if (out.len == 0 || out.buf[out.len - 1] != '\n')
		sbuf_add(&out, "\n\n", 2);
	else if (out.len < 2 || out.buf[out.len - 2] != '\n')
		sbuf_addch(&out, '\n');

	fputs(out.buf, stdout);
	sbuf_release(&out);
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
		if (o->config_key || o->env_var) {
			fputs(INDENT INDENT, stdout);
			if (o->config_key)
				printf("[config: @b{%s}]", o->config_key);
			if (o->config_key && o->env_var)
				fputs(" ", stdout);
			if (o->env_var)
				printf("[env: @b{%s}]", o->env_var);
			fputs("\n", stdout);
		}
		fputs("\n", stdout);
	}
}

static int has_config_sources(const struct option *opts)
{
	if (!opts)
		return 0;
	for (const struct option *o = opts; o->type != OPTION_END; o++)
		if (o->config_key)
			return 1;
	return 0;
}

static int has_env_sources(const struct option *opts)
{
	if (!opts)
		return 0;
	for (const struct option *o = opts; o->type != OPTION_END; o++)
		if (o->env_var)
			return 1;
	return 0;
}

/*
 * Emit a definition list of <source name> -> description pairs for
 * each option that has the requested source set.  The @p key_of
 * callback picks between config_key and env_var so one body routine
 * serves both CONFIG and ENVIRONMENT sections.
 */
static void print_source_body(const struct option *opts,
			      const char *(*key_of)(const struct option *))
{
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		const char *key = key_of(o);
		const char *desc;

		if (!key)
			continue;

		desc = o->config_help ? o->config_help : o->help;
		printf(INDENT "@b{%s}\n", key);
		if (desc) {
			fputs(INDENT INDENT, stdout);
			fputs(desc, stdout);
			fputs("\n", stdout);
		}
		fputs("\n", stdout);
	}
}

static const char *opt_config_key(const struct option *o)
{
	return o->config_key;
}

static const char *opt_env_var(const struct option *o) { return o->env_var; }

static void print_commands_body(const struct cmd_desc *const *subs)
{
	size_t maxlen = 0;

	for (const struct cmd_desc *const *p = subs; *p; p++) {
		size_t l;
		/* Skip internal commands (leading underscore). */
		if ((*p)->name[0] == '_')
			continue;
		l = strlen((*p)->name);
		if (l > maxlen)
			maxlen = l;
	}

	for (const struct cmd_desc *const *p = subs; *p; p++) {
		const char *summary;
		if ((*p)->name[0] == '_')
			continue;
		summary = (*p)->manual ? (*p)->manual->summary : NULL;
		printf(INDENT "@b{%-*s}   %s\n", (int)maxlen, (*p)->name,
		       summary ? summary : "");
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

		printf(INDENT "@b{%-*s}   %s\n", (int)maxlen, names->v[i],
		       val ? val : "");
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

void print_manual(const char *cmd_name, const struct cmd_desc *desc)
{
	const struct cmd_manual *m = desc ? desc->manual : NULL;
	const struct option *opts = desc ? desc->opts : NULL;
	const char *summary;
	int has_flags = 0;
	int has_subcmds = 0;
	const char *positional = NULL;

	cmd_name = basename_of(cmd_name);
	summary = m ? m->summary : NULL;

	if (opts) {
		if (opts->type != OPTION_END)
			has_flags = 1;
		{
			const struct option *end = opts;
			while (end->type != OPTION_END)
				end++;
			positional = end->argh;
		}
	}
	if (desc && desc->subcommands && *desc->subcommands)
		has_subcmds = 1;

	pager_start();

	/* NAME */
	fputs("@b{NAME}\n" INDENT, stdout);
	fputs(cmd_name ? cmd_name : "ice", stdout);
	if (summary)
		printf(" - %s", summary);
	fputs("\n\n", stdout);

	/* SYNOPSIS */
	fputs("@b{SYNOPSIS}\n" INDENT, stdout);
	printf("%s", cmd_name ? cmd_name : "ice");
	if (has_flags)
		printf(" [<options>]");
	if (has_subcmds)
		printf(" <subcommand> [<args>]");
	else if (positional) {
		/*
		 * Mirrors print_usage: argh with '<' or '[' is a
		 * pre-formatted fragment (multi-positional commands),
		 * a bare word gets wrapped in <>.
		 */
		if (strchr(positional, '<') || strchr(positional, '['))
			printf(" %s", positional);
		else
			printf(" <%s>", positional);
	}
	fputs("\n\n", stdout);

	/* DESCRIPTION */
	if (m && m->description) {
		fputs("@b{DESCRIPTION}\n", stdout);
		print_text(m->description);
	}

	/* OPTIONS -- auto-generated from the option table. */
	if (has_options(opts)) {
		fputs("@b{OPTIONS}\n", stdout);
		print_options_body(opts);
	}

	/* CONFIG / ENVIRONMENT -- auto-generated from option source fields. */
	if (has_config_sources(opts)) {
		fputs("@b{CONFIG}\n", stdout);
		print_source_body(opts, opt_config_key);
	}
	if (has_env_sources(opts)) {
		fputs("@b{ENVIRONMENT}\n", stdout);
		print_source_body(opts, opt_env_var);
	}

	/* COMMANDS -- auto-emitted from desc->subcommands. */
	if (desc && desc->subcommands && *desc->subcommands) {
		fputs("@b{COMMANDS}\n", stdout);
		print_commands_body(desc->subcommands);
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

	/* EXAMPLES */
	if (m && m->examples) {
		fputs("@b{EXAMPLES}\n", stdout);
		print_text(m->examples);
	}

	/* EXTRAS -- author-structured sections. */
	if (m && m->extras)
		print_text(m->extras);
}
