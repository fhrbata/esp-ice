/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file term.c
 * @brief Terminal presentation support -- shared logic.
 *
 * Provides the color token expander (expand_colors) used by the
 * platform fprintf overrides, and the global color/VT flags.
 */
#include "ice.h"

int use_color;
int use_vt;

void color_init(int fd)
{
	use_color = isatty(fd);
	use_vt = PLATFORM_ANSI_VT_DEFAULT;
}

int use_color_for(FILE *stream)
{
	if (!use_color)
		return 0;
	/*
	 * When the pager is active, stdout's underlying fd is a pipe
	 * to the pager -- not a tty -- but the pager (e.g. `less -R`)
	 * renders the ANSI sequences we emit, so we still want colour.
	 */
	if (stream == stdout && pager_active())
		return 1;
	return isatty(fileno(stream));
}

/* Named color lookup for @[COLOR_RED]{...} syntax. */
static const struct {
	const char *name;
	const char *code;
} color_names[] = {
    {"COLOR_RESET", "\033[0m"},
    {"COLOR_BOLD", "\033[1m"},
    {"COLOR_UNDERLINE", "\033[4m"},
    {"COLOR_REVERSE", "\033[7m"},
    {"COLOR_BLACK", "\033[30m"},
    {"COLOR_RED", "\033[31m"},
    {"COLOR_GREEN", "\033[32m"},
    {"COLOR_YELLOW", "\033[33m"},
    {"COLOR_BLUE", "\033[34m"},
    {"COLOR_MAGENTA", "\033[35m"},
    {"COLOR_CYAN", "\033[36m"},
    {"COLOR_WHITE", "\033[37m"},
    {"COLOR_BOLD_RED", "\033[1;31m"},
    {"COLOR_BOLD_GREEN", "\033[1;32m"},
    {"COLOR_BOLD_YELLOW", "\033[1;33m"},
    {"COLOR_BOLD_BLUE", "\033[1;34m"},
    {"COLOR_BOLD_MAGENTA", "\033[1;35m"},
    {"COLOR_BOLD_CYAN", "\033[1;36m"},
    {"COLOR_BOLD_WHITE", "\033[1;37m"},
    {"COLOR_BG_RED", "\033[41m"},
    {"COLOR_BG_GREEN", "\033[42m"},
    {"COLOR_BG_YELLOW", "\033[43m"},
    {"COLOR_BG_BLUE", "\033[44m"},
    {"COLOR_BG_MAGENTA", "\033[45m"},
    {"COLOR_BG_CYAN", "\033[46m"},
    {"COLOR_BG_WHITE", "\033[47m"},
    {NULL},
};

static const char *find_color_name(const char *name, size_t len)
{
	size_t i;
	for (i = 0; color_names[i].name; i++) {
		if (strlen(color_names[i].name) == len &&
		    !memcmp(color_names[i].name, name, len))
			return color_names[i].code;
	}
	return NULL;
}

void color_text(struct sbuf *out, const char *text, size_t len,
		const struct color_rule *rules)
{
	const char *p = text;
	const char *end = text + len;

	while (p < end) {
		/* Check caller-supplied keyword rules. */
		int matched = 0;
		for (size_t i = 0; rules && rules[i].keyword; i++) {
			if (end - p >= rules[i].len &&
			    !memcmp(p, rules[i].keyword, rules[i].len)) {
				sbuf_addf(out, "@[%s]{", rules[i].color);
				sbuf_add(out, p, rules[i].len);
				sbuf_addch(out, '}');
				p += rules[i].len;
				matched = 1;
				break;
			}
		}
		if (matched)
			continue;

		/* Quoted strings: 'x', `x`, "x" → bold (skip \" etc.) */
		if ((*p == '\'' || *p == '`' || *p == '"') && p + 1 < end) {
			char q = *p;
			const char *s = p + 1;
			const char *close = NULL;
			while (s < end) {
				if (*s == '\\' && s + 1 < end) {
					s += 2;
					continue;
				}
				if (*s == q) {
					close = s;
					break;
				}
				s++;
			}
			if (close && close - p < 80) {
				sbuf_addf(out, "@b{%c", q);
				sbuf_add(out, p + 1, close - p - 1);
				sbuf_addf(out, "%c}", q);
				p = close + 1;
				continue;
			}
		}

		/* GCC caret+range: ^~~~~~~~~ → red */
		if (*p == '^' && p + 1 < end && *(p + 1) == '~') {
			const char *s = p;
			p++;
			while (p < end && *p == '~')
				p++;
			sbuf_addstr(out, "@r{");
			sbuf_add(out, s, p - s);
			sbuf_addch(out, '}');
			continue;
		}

		/* Numbers: 0x1a2b, 0777, 42 → cyan (only whitespace-bounded) */
		if (*p >= '0' && *p <= '9' &&
		    (p == text || isspace((unsigned char)*(p - 1)))) {
			const char *s = p;
			if (*p == '0' && p + 1 < end &&
			    (*(p + 1) == 'x' || *(p + 1) == 'X')) {
				p += 2;
				while (p < end && ((*p >= '0' && *p <= '9') ||
						   (*p >= 'a' && *p <= 'f') ||
						   (*p >= 'A' && *p <= 'F')))
					p++;
			} else {
				while (p < end && *p >= '0' && *p <= '9')
					p++;
			}
			if (p == end || isspace((unsigned char)*p)) {
				sbuf_addstr(out, "@c{");
				sbuf_add(out, s, p - s);
				sbuf_addch(out, '}');
				continue;
			}
			p = s;
		}

		/* Escape @ and } for color token safety. */
		if (*p == '@') {
			sbuf_addstr(out, "@@");
			p++;
			continue;
		}
		if (*p == '}') {
			sbuf_addstr(out, "@}");
			p++;
			continue;
		}

		sbuf_addch(out, *p++);
	}
}

/**
 * @brief Expand @x{...} color tokens in a format string.
 *
 * When use_color is set, tokens are replaced with ANSI escape codes.
 * When unset, tokens are stripped and only the text content remains.
 * The expanded result is a valid printf format string.
 *
 * Escaping: @@ -> literal @, @} -> literal } inside a color block.
 *
 * Nested tokens (e.g. @r{fatal: @b{hint} tail}) work: the stack tracks
 * each pushed code so that closing an inner block emits a reset and
 * re-emits the outer codes, restoring the parent style.  Depth beyond
 * STACK_MAX still parses correctly but doesn't restyle -- only visual
 * fidelity is lost, not content.
 */
#define EXPAND_STACK_MAX 8
#define EXPAND_CODE_MAX 32

void expand_colors(struct sbuf *out, const char *fmt, int colorize)
{
	char stack[EXPAND_STACK_MAX][EXPAND_CODE_MAX];
	int depth = 0;

	while (*fmt) {
		/* @@ -> literal @ */
		if (*fmt == '@' && fmt[1] == '@') {
			sbuf_addch(out, '@');
			fmt += 2;
			continue;
		}

		/*
		 * @} -> literal } (escape needed inside color blocks, where
		 * a bare } would close the current block).  The previous
		 * "}}" form was ambiguous with two stacked closes, e.g.
		 * @r{fatal @b{foo}} -- there the trailing }} is two closes,
		 * not an escape.  @} has no such ambiguity.
		 */
		if (*fmt == '@' && fmt[1] == '}') {
			sbuf_addch(out, '}');
			fmt += 2;
			continue;
		}

		/*
		 * @[spec]{ -> named color or numeric SGR:
		 *   @[COLOR_RED]{...}    named lookup
		 *   @[38;5;208]{...}     raw SGR parameters
		 */
		if (*fmt == '@' && fmt[1] == '[') {
			const char *end = strchr(fmt + 2, ']');
			if (end && end[1] == '{') {
				const char *s = fmt + 2;
				size_t len = (size_t)(end - s);
				const char *named = find_color_name(s, len);
				char code[EXPAND_CODE_MAX];

				if (named) {
					size_t clen = strlen(named);
					if (clen >= sizeof(code))
						clen = sizeof(code) - 1;
					memcpy(code, named, clen);
					code[clen] = '\0';
				} else {
					int n =
					    snprintf(code, sizeof(code),
						     "\033[%.*sm", (int)len, s);
					if (n < 0 || n >= (int)sizeof(code))
						code[sizeof(code) - 1] = '\0';
				}

				if (colorize)
					sbuf_addstr(out, code);
				if (depth < EXPAND_STACK_MAX)
					memcpy(stack[depth], code,
					       sizeof(code));
				depth++;
				fmt = end + 2;
				continue;
			}
		}

		/* @x{ -> start color (only for recognized letters) */
		if (*fmt == '@' && fmt[1] && fmt[2] == '{') {
			const char *code = NULL;

			switch (fmt[1]) {
			case 'r':
				code = "\033[31m";
				break;
			case 'g':
				code = "\033[32m";
				break;
			case 'y':
				code = "\033[33m";
				break;
			case 'b':
				code = "\033[1m";
				break;
			case 'c':
				code = "\033[36m";
				break;
			case 'R':
				code = "\033[1;31m";
				break;
			case 'G':
				code = "\033[1;32m";
				break;
			case 'Y':
				code = "\033[1;33m";
				break;
			}

			if (code) {
				if (colorize)
					sbuf_addstr(out, code);
				if (depth < EXPAND_STACK_MAX) {
					size_t clen = strlen(code);
					memcpy(stack[depth], code, clen + 1);
				}
				depth++;
				fmt += 3;
				continue;
			}
			/* Unrecognized: fall through, emit '@' as literal */
		}

		/* } -> close color block: reset, restore outer codes */
		if (*fmt == '}' && depth > 0) {
			depth--;
			if (colorize) {
				sbuf_addstr(out, "\033[0m");
				for (int i = 0;
				     i < depth && i < EXPAND_STACK_MAX; i++)
					sbuf_addstr(out, stack[i]);
			}
			fmt++;
			continue;
		}

		sbuf_addch(out, *fmt++);
	}
}
