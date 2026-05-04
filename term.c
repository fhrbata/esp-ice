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

const char *term_key_to_xterm_seq(int key)
{
	/* Sequences mirror the @c vk_to_seq table in
	 * @c platform/win/term.c -- xterm / VT220 conventions that
	 * every common terminal recognises.  Returning NULL keeps the
	 * caller's defaults intact for keys without a canonical
	 * escape (printable bytes, mouse wheel, @c TK_RESIZE, ...). */
	switch (key) {
	case TK_UP:
		return "\x1b[A";
	case TK_DOWN:
		return "\x1b[B";
	case TK_RIGHT:
		return "\x1b[C";
	case TK_LEFT:
		return "\x1b[D";
	case TK_HOME:
		return "\x1b[H";
	case TK_END:
		return "\x1b[F";
	case TK_INS:
		return "\x1b[2~";
	case TK_DEL:
		return "\x1b[3~";
	case TK_F1:
		return "\x1bOP";
	case TK_F2:
		return "\x1bOQ";
	case TK_F3:
		return "\x1bOR";
	case TK_F4:
		return "\x1bOS";
	case TK_F5:
		return "\x1b[15~";
	case TK_F6:
		return "\x1b[17~";
	case TK_F7:
		return "\x1b[18~";
	case TK_F8:
		return "\x1b[19~";
	case TK_F9:
		return "\x1b[20~";
	case TK_F10:
		return "\x1b[21~";
	case TK_F11:
		return "\x1b[23~";
	case TK_F12:
		return "\x1b[24~";
	default:
		return NULL;
	}
}

/* ================================================================== */
/*  Input event decoder                                               */
/* ================================================================== */

/*
 * Timeout (ms) used to drain the tail of an escape sequence after the
 * initial @c ESC byte has been observed.  Modern terminals flush the
 * full CSI / SS3 sequence in well under a millisecond; 50 ms is the
 * classical value used by termbox / ncurses to distinguish a bare ESC
 * press from a prefix and leaves plenty of slack for laggy SSH links.
 */
#define TERM_ESC_TIMEOUT_MS 50

static const struct {
	const char *seq; /* bytes AFTER the initial ESC */
	int key;
} term_keyseq[] = {
    /* CSI (ESC [ ...) sequences. */
    {"[A", TK_UP},
    {"[B", TK_DOWN},
    {"[C", TK_RIGHT},
    {"[D", TK_LEFT},
    {"[H", TK_HOME},
    {"[F", TK_END},
    {"[1~", TK_HOME},
    {"[4~", TK_END},
    {"[5~", TK_PGUP},
    {"[6~", TK_PGDN},
    {"[2~", TK_INS},
    {"[3~", TK_DEL},
    {"[11~", TK_F1},
    {"[12~", TK_F2},
    {"[13~", TK_F3},
    {"[14~", TK_F4},
    {"[15~", TK_F5},
    {"[17~", TK_F6},
    {"[18~", TK_F7},
    {"[19~", TK_F8},
    {"[20~", TK_F9},
    {"[21~", TK_F10},
    {"[23~", TK_F11},
    {"[24~", TK_F12},
    /* Bracketed-paste markers (DECSET 2004). */
    {"[200~", TK_PASTE_BEGIN},
    {"[201~", TK_PASTE_END},
    /* SS3 (ESC O ...) sequences used by xterm application-keypad
     * mode and by rxvt for the arrow keys. */
    {"OA", TK_UP},
    {"OB", TK_DOWN},
    {"OC", TK_RIGHT},
    {"OD", TK_LEFT},
    {"OH", TK_HOME},
    {"OF", TK_END},
    {"OP", TK_F1},
    {"OQ", TK_F2},
    {"OR", TK_F3},
    {"OS", TK_F4},
};

/*
 * Pending-byte buffer for the event decoder.
 *
 * A single kernel read can return multiple escape sequences when the
 * user holds an arrow key (terminal repeats `ESC[B ESC[B ESC[B ...`
 * faster than we consume them).  The previous implementation fed the
 * whole blob to one table lookup and fell through to a bare TK_ESC
 * on no match -- which, at the root menu, quits the TUI.  Buffer the
 * bytes here and parse exactly one sequence per call instead, so the
 * event stream mirrors keystrokes 1:1 regardless of how the terminal
 * chunked its output.
 */
#define TERM_PBUF_CAP 64
static unsigned char term_pbuf[TERM_PBUF_CAP];
static size_t term_phead;
static size_t term_ptail;

/*
 * Bracketed-paste mode latch.  Set when the parser delivers
 * @c TK_PASTE_BEGIN, cleared when the matching @c TK_PASTE_END
 * lands.  While set, every byte is delivered verbatim (no escape
 * decoding) except the literal @c \\e[201~ end marker -- so a paste
 * containing arrow keys, Ctrl-T, mouse-style escapes etc. ships as
 * raw bytes the host can forward unchanged.
 */
static int term_in_paste;

/*
 * Ensure at least one byte is queued, reading from term_read if the
 * current buffer is empty.  @p timeout_ms applies only on the read --
 * draining already-buffered bytes is instant.
 */
static ssize_t term_fill(unsigned timeout_ms)
{
	if (term_phead < term_ptail)
		return (ssize_t)(term_ptail - term_phead);
	term_phead = term_ptail = 0;
	ssize_t n = term_read(term_pbuf, sizeof(term_pbuf), timeout_ms);
	if (n < 0)
		return -1;
	term_ptail = (size_t)n;
	return n;
}

/*
 * Match the bytes @p buf[0..len) (the payload after ESC) against the
 * keyseq table.  Returns the mapped @ref term_key value or 0 when
 * nothing matches.
 */
static int match_keyseq(const unsigned char *buf, size_t len)
{
	for (size_t i = 0; i < sizeof(term_keyseq) / sizeof(term_keyseq[0]);
	     i++) {
		size_t tlen = strlen(term_keyseq[i].seq);
		if (tlen == len && memcmp(buf, term_keyseq[i].seq, tlen) == 0)
			return term_keyseq[i].key;
	}
	return 0;
}

int term_read_event(struct term_event *ev, unsigned timeout_ms)
{
	ev->key = TK_NONE;
	ev->cols = 0;
	ev->rows = 0;

	if (term_resize_pending()) {
		ev->key = TK_RESIZE;
		term_size(&ev->cols, &ev->rows);
		return 1;
	}

	ssize_t n = term_fill(timeout_ms);
	if (n < 0)
		return -1;
	if (n == 0)
		return 0;

	/* Inside a bracketed paste: deliver bytes verbatim, watching
	 * only for the closing @c \\e[201~ marker so escape sequences
	 * inside the payload don't get mis-interpreted as keypresses. */
	if (term_in_paste) {
		if (term_pbuf[term_phead] == 0x1b) {
			/* Try to match a 6-byte close marker; pull more
			 * bytes if the paste-end straddles a read. */
			if (term_phead + 6 > term_ptail)
				term_fill(TERM_ESC_TIMEOUT_MS);
			if (term_phead + 6 <= term_ptail &&
			    term_pbuf[term_phead + 1] == '[' &&
			    term_pbuf[term_phead + 2] == '2' &&
			    term_pbuf[term_phead + 3] == '0' &&
			    term_pbuf[term_phead + 4] == '1' &&
			    term_pbuf[term_phead + 5] == '~') {
				term_phead += 6;
				term_in_paste = 0;
				ev->key = TK_PASTE_END;
				return 1;
			}
			/* Lone ESC inside the payload -- deliver as-is. */
		}
		ev->key = term_pbuf[term_phead++];
		return 1;
	}

	unsigned char first = term_pbuf[term_phead++];
	if (first != 0x1b) {
		ev->key = first;
		return 1;
	}

	/*
	 * ESC seen.  Peek at the next byte to classify the sequence; if
	 * the buffer is empty, wait a short window for the terminal to
	 * deliver the tail.  No tail -> bare ESC (e.g. user closed a
	 * modal).
	 */
	if (term_phead >= term_ptail) {
		ssize_t m = term_fill(TERM_ESC_TIMEOUT_MS);
		if (m <= 0) {
			ev->key = TK_ESC;
			return m < 0 ? -1 : 1;
		}
	}

	unsigned char second = term_pbuf[term_phead];

	/*
	 * CSI: ESC [ [params] final-byte.  Final is any byte in
	 * 0x40..0x7E.  Scan one byte at a time so a partial sequence
	 * that straddles a read boundary resumes cleanly via term_fill.
	 *
	 * SS3: ESC O final-byte -- exactly three bytes total.
	 */
	if (second == '[' || second == 'O') {
		size_t seq_start = term_phead;
		term_phead++; /* consume '[' / 'O' */
		int got_final = 0;
		if (second == 'O') {
			if (term_phead >= term_ptail)
				term_fill(TERM_ESC_TIMEOUT_MS);
			if (term_phead < term_ptail) {
				term_phead++; /* final byte */
				got_final = 1;
			}
		} else {
			for (;;) {
				if (term_phead >= term_ptail) {
					ssize_t m =
					    term_fill(TERM_ESC_TIMEOUT_MS);
					if (m <= 0)
						break;
				}
				unsigned char c = term_pbuf[term_phead++];
				if (c >= 0x40 && c <= 0x7E) {
					got_final = 1;
					break;
				}
			}
		}

		if (got_final) {
			size_t seq_len = term_phead - seq_start;
			/* SGR mouse: CSI < button ; col ; row (M|m).  The
			 * private-CSI '<' marks the SGR encoding (terminal
			 * mode 1006).  @c seq_start points at the leading
			 * '['; the '<' marker (when present) sits one byte
			 * later.  Parse the three integer fields and
			 * synthesise a wheel event for buttons 64/65;
			 * silently swallow other mouse events for now. */
			if (second == '[' && seq_len >= 3 &&
			    term_pbuf[seq_start + 1] == '<') {
				unsigned char final = term_pbuf[term_phead - 1];
				if (final == 'M' || final == 'm') {
					int btn = 0, col = 0, row = 0;
					int field = 0;
					for (size_t i = seq_start + 2;
					     i < term_phead - 1; i++) {
						unsigned char c = term_pbuf[i];
						if (c == ';') {
							field++;
							continue;
						}
						if (c < '0' || c > '9')
							continue;
						int *dst = field == 0	? &btn
							   : field == 1 ? &col
									: &row;
						*dst = *dst * 10 + (c - '0');
					}
					/* Ignore button modifier bits
					 * (shift/meta/ ctrl 0x04/08/10 plus the
					 * motion flag 0x20) when classifying
					 * the button. */
					int base = btn & ~0x3c;
					if (final == 'M' && base == 64) {
						ev->key = TK_WHEEL_UP;
						ev->cols = col;
						ev->rows = row;
						return 1;
					}
					if (final == 'M' && base == 65) {
						ev->key = TK_WHEEL_DOWN;
						ev->cols = col;
						ev->rows = row;
						return 1;
					}
					if (final == 'M' && base == 0) {
						ev->key = TK_MOUSE_PRESS;
						ev->cols = col;
						ev->rows = row;
						return 1;
					}
					/* Other mouse events (middle / right
					 * clicks, motion, release) -- consume
					 * silently. */
					ev->key = TK_NONE;
					return 1;
				}
			}
			int key = match_keyseq(&term_pbuf[seq_start], seq_len);
			if (key) {
				if (key == TK_PASTE_BEGIN)
					term_in_paste = 1;
				ev->key = key;
				return 1;
			}
		}
		/* Unrecognised or truncated -- deliver bare ESC so callers
		 * can at least close a modal.  Bytes already consumed stay
		 * consumed; the next event starts after them. */
		ev->key = TK_ESC;
		return 1;
	}

	/*
	 * Alt-key (ESC + char) or unknown prefix.  Menuconfig doesn't
	 * use Alt combos; drop the follow-up and deliver bare ESC.
	 */
	term_phead++;
	ev->key = TK_ESC;
	return 1;
}

/* ================================================================== */
/*  Raw-mode output helpers                                           */
/* ================================================================== */

/*
 * Every helper writes via stdio so the Windows output shim in
 * platform/win/io.c can intercept the byte stream when VT processing
 * is unavailable and dispatch to Console API calls.  fflush keeps the
 * TUI redraw order tight against stdin -- without it a buffered
 * escape could leave the cursor in the wrong place between keystrokes.
 */

void term_move(int row, int col)
{
	printf("\x1b[%d;%dH", row, col);
	fflush(stdout);
}

void term_clear_to_eol(void)
{
	fputs("\x1b[K", stdout);
	fflush(stdout);
}

void term_clear_line(void)
{
	fputs("\x1b[2K", stdout);
	fflush(stdout);
}

void term_clear_screen(void)
{
	/* Clear the screen and home the cursor in one fflush -- the two
	 * together are the canonical "fresh frame" prologue. */
	fputs("\x1b[2J\x1b[H", stdout);
	fflush(stdout);
}

void term_sgr(const char *codes)
{
	printf("\x1b[%sm", codes && *codes ? codes : "0");
	fflush(stdout);
}

void term_sgr_reset(void) { term_sgr(NULL); }
