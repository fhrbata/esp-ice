/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file monitor.c
 * @brief Reusable chip-output monitor session.
 *
 * The state machine the standalone @c{ice target monitor} command
 * used to own end-to-end -- vt100 + tui_log + level decoration +
 * Ctrl-T prefix + inspect/search/help -- packaged as a passive
 * widget the way @ref vt100 and @ref tui_log are.  See @ref
 * monitor.h for the API contract; this file is the implementation
 * lifted from @c cmd/target/monitor/monitor.c with all I/O
 * (serial port, terminal, signal handling) lifted out.
 *
 * Three callers are expected:
 *   - @c{ice target monitor}: opens a serial port, owns the user's
 *     terminal, drives one session full-screen.
 *   - @c{ice qemu}: spawns qemu with @c{-serial stdio}, drives one
 *     session full-screen against qemu's stdout pipe.
 *   - @c{ice qemu --debug} / @c{ice debug}: dual-pane parent, drives
 *     one session embedded in a sub-rect alongside a gdb pty pane.
 *
 * The session never reads or writes a transport itself; @ref
 * monitor_feed_chip and @ref monitor_chip_tx are the chip-side
 * interface, @ref monitor_feed_event is the user-side interface,
 * @ref monitor_render is the screen-side interface.  Callers are
 * responsible for the actual reads, writes, and tui_flush.
 */
#include "monitor.h"

#include "sbuf.h"
#include "term.h"
#include "tui.h"
#include "vt100.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Mode flat enum                                                    */
/* ------------------------------------------------------------------ */

/*
 * Two user-visible modes (normal, inspect) plus a couple of internal
 * sub-states each (Ctrl-T prefix waiting, help modal, search prompt).
 */
enum mon_mode {
	MON_NORMAL = 0,
	MON_NORMAL_PREFIX, /* Ctrl-T pressed, waiting for next byte */
	MON_NORMAL_HELP,   /* tui_info modal showing normal-mode help */
	MON_INSPECT,
	MON_INSPECT_SEARCH, /* tui_prompt modal active */
	MON_INSPECT_HELP,   /* tui_info modal showing inspect-mode help */
};

static int mode_is_inspect(int mode)
{
	return mode == MON_INSPECT || mode == MON_INSPECT_SEARCH ||
	       mode == MON_INSPECT_HELP;
}

/* ------------------------------------------------------------------ */
/*  Session state                                                     */
/* ------------------------------------------------------------------ */

struct monitor_session {
	struct vt100 *V;
	struct tui_log L;

	struct sbuf status;
	struct sbuf chip_tx;

	struct tui_prompt search_prompt;
	struct tui_info help_info;

	char *source_label;

	void (*on_reset)(void *ctx);
	void (*on_bootloader)(void *ctx);
	void *cb_ctx;

	int origin_x, origin_y;
	int width, height;

	int mode;
	int dirty;
	int should_quit;
};

/* ------------------------------------------------------------------ */
/*  Help text                                                         */
/* ------------------------------------------------------------------ */

static const char NORMAL_HELP_TEXT[] =
    "Normal mode\n"
    "\n"
    "Every keystroke is forwarded to the connected target except\n"
    "the Ctrl-T prefix.  Type Ctrl-T followed by:\n"
    "\n"
    "  ?, h           Show this help.\n"
    "  i              Enter inspect mode (freeze the buffer +\n"
    "                 explore history with vim-style keys).\n"
    "  r              Reset the target.\n"
    "  p              Reset into the bootloader.\n"
    "  x              Exit the monitor.\n"
    "  Ctrl-T         Send a literal Ctrl-T to the target.\n"
    "\n"
    "Press any key to dismiss this help.";

static const char INSPECT_HELP_TEXT[] =
    "Inspect mode\n"
    "\n"
    "The buffer is frozen.  Bytes keep arriving in the background\n"
    "but stay invisible until you exit inspect.  Inside:\n"
    "\n"
    "  Up / Down, j / k    Scroll one line.\n"
    "  PgUp / PgDn         Scroll one page.\n"
    "  Home / End,\n"
    "  g / G               Jump to oldest / newest.\n"
    "  /                   Open search prompt (regex).\n"
    "  n / N               Jump to next / previous match.\n"
    "  q, Esc              Return to normal mode.\n"
    "  ?, h                Show this help.\n";

/* ------------------------------------------------------------------ */
/*  Status bar text                                                   */
/* ------------------------------------------------------------------ */

/*
 * Rebuild the title-bar status string.  Always called before render
 * so the badge, key hints, and search counter stay live as state
 * changes.  Cheap: a few @c sbuf appends and one pointer swap.
 */
static void update_status(struct monitor_session *m)
{
	sbuf_reset(&m->status);
	if (m->source_label && *m->source_label)
		sbuf_addstr(&m->status, m->source_label);
	if (mode_is_inspect(m->mode)) {
		sbuf_addstr(&m->status, "  \xe2\x80\xa2  [INSPECT]");
		sbuf_addstr(&m->status, "  \xe2\x80\xa2  Esc=back  ?=help");
		if (tui_log_search_active(&m->L)) {
			sbuf_addstr(&m->status, "  \xe2\x80\xa2  search: ");
			sbuf_addstr(&m->status, tui_log_search_pattern(&m->L));
			int total = tui_log_search_total(&m->L);
			int idx = tui_log_search_index(&m->L);
			if (total <= 0)
				sbuf_addstr(&m->status, " (no matches)");
			else if (idx > 0)
				sbuf_addf(&m->status, " (%d/%d)", idx, total);
			else
				sbuf_addf(&m->status, " (%d)", total);
		}
	} else {
		sbuf_addstr(&m->status, "  \xe2\x80\xa2  [NORMAL]");
		sbuf_addstr(
		    &m->status,
		    "  \xe2\x80\xa2  Ctrl-T:  ?=help  i=inspect  x=exit");
	}
	tui_log_set_status(&m->L, m->status.buf);
}

/* ------------------------------------------------------------------ */
/*  Per-line decorator                                                */
/* ------------------------------------------------------------------ */

static const struct {
	const char *kw;
	size_t klen;
	const char *sgr;
} mon_kw_rules[] = {
    {"fatal error:", 12, "1;31"},
    {"FAILED:", 7, "1;31"},
    {"error:", 6, "1;31"},
    {"warning:", 8, "1;33"},
    {"note:", 5, "36"},
    {"CMake Error", 11, "0;31"},
    {"CMake Warning", 13, "0;33"},
    {"undefined reference to", 22, "0;31"},
    {"multiple definition of", 22, "0;31"},
    {"In file included from", 21, "36"},
};

/*
 * Two layers, identical to the standalone @c{ice target monitor}:
 *
 *   1. ESP-IDF level prefix: @c E / @c W / @c I followed by a space
 *      gets a whole-line overlay; firmware-coloured lines (start
 *      with ESC) are passed through.
 *   2. Otherwise scan for the keyword rules above (error: / warning:
 *      / CMake Error / ...) and emit one overlay per match.
 *
 * Skipping the keyword scan when the level layer fires keeps
 * red-on-red regions from stacking.
 */
static int monitor_decorate(const char *line, size_t len, void *ctx,
			    struct tui_overlay *out, int max)
{
	(void)ctx;
	if (max < 1 || len == 0)
		return 0;

	if ((unsigned char)line[0] == 0x1b)
		return 0;

	if (len >= 2 && line[1] == ' ') {
		const char *sgr_open = NULL;
		switch (line[0]) {
		case 'E':
			sgr_open = "0;31";
			break;
		case 'W':
			sgr_open = "0;33";
			break;
		case 'I':
			sgr_open = "0;32";
			break;
		}
		if (sgr_open) {
			out[0].start = 0;
			out[0].end = len;
			out[0].sgr_open = sgr_open;
			out[0].sgr_close = "0";
			return 1;
		}
	}

	int n = 0;
	for (size_t r = 0;
	     r < sizeof(mon_kw_rules) / sizeof(mon_kw_rules[0]) && n < max;
	     r++) {
		size_t klen = mon_kw_rules[r].klen;
		if (klen == 0 || klen > len)
			continue;
		size_t end = len - klen;
		for (size_t i = 0; i <= end && n < max; i++) {
			if (memcmp(line + i, mon_kw_rules[r].kw, klen) != 0)
				continue;
			out[n].start = i;
			out[n].end = i + klen;
			out[n].sgr_open = mon_kw_rules[r].sgr;
			out[n].sgr_close = "0";
			n++;
			i += klen - 1; /* loop's i++ steps past the match */
		}
	}
	return n;
}

/* ------------------------------------------------------------------ */
/*  Layout helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Apply the session's rect to the inner widgets.  tui_log uses the
 * outer rect (status bar at the top eats one row internally); the
 * vt100 grid is sized to the body area (one row narrower for
 * status, one column narrower for the scrollbar) so the chip's
 * column probes (linenoise's @c{\x1b[999C\x1b[6n}) and autowrap
 * math match what gets painted.
 */
static void apply_rect(struct monitor_session *m)
{
	tui_log_set_origin(&m->L, m->origin_x, m->origin_y);
	tui_log_resize(&m->L, m->width, m->height);

	int inner_w = m->width > 1 ? m->width - 1 : m->width;
	int inner_h = m->height > 1 ? m->height - 1 : m->height;
	if (inner_h < 1)
		inner_h = 1;
	vt100_resize(m->V, inner_h, inner_w);

	if (m->mode == MON_INSPECT_SEARCH)
		tui_prompt_resize(&m->search_prompt, m->width, m->height);
	if (m->mode == MON_NORMAL_HELP || m->mode == MON_INSPECT_HELP)
		tui_info_resize(&m->help_info, m->width, m->height);
}

/* ------------------------------------------------------------------ */
/*  Construction / destruction                                        */
/* ------------------------------------------------------------------ */

struct monitor_session *monitor_new(const struct monitor_config *cfg)
{
	if (!cfg)
		return NULL;
	struct monitor_session *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;

	int scrollback = cfg->scrollback > 0 ? cfg->scrollback : 1;
	int w = cfg->width > 0 ? cfg->width : 80;
	int h = cfg->height > 0 ? cfg->height : 24;

	m->V = vt100_new(h > 1 ? h - 1 : h, w > 1 ? w - 1 : w);
	if (!m->V) {
		free(m);
		return NULL;
	}

	tui_log_init(&m->L, scrollback);
	tui_log_set_grid(&m->L, m->V);
	if (use_color)
		tui_log_set_decorator(&m->L, monitor_decorate, NULL);

	m->source_label =
	    cfg->source_label ? sbuf_strdup(cfg->source_label) : NULL;
	m->on_reset = cfg->on_reset;
	m->on_bootloader = cfg->on_bootloader;
	m->cb_ctx = cfg->cb_ctx;

	m->origin_x = cfg->origin_x > 0 ? cfg->origin_x : 1;
	m->origin_y = cfg->origin_y > 0 ? cfg->origin_y : 1;
	m->width = w;
	m->height = h;

	m->mode = MON_NORMAL;
	m->dirty = 1;
	m->should_quit = 0;

	apply_rect(m);
	update_status(m);
	return m;
}

void monitor_release(struct monitor_session *m)
{
	if (!m)
		return;
	if (m->mode == MON_NORMAL_HELP || m->mode == MON_INSPECT_HELP)
		tui_info_release(&m->help_info);
	/* tui_prompt is a flat struct with no heap ownership -- nothing
	 * to release for the search prompt. */
	tui_log_release(&m->L);
	vt100_free(m->V);
	sbuf_release(&m->status);
	sbuf_release(&m->chip_tx);
	free(m->source_label);
	free(m);
}

void monitor_set_rect(struct monitor_session *m, int origin_x, int origin_y,
		      int width, int height)
{
	if (!m)
		return;
	m->origin_x = origin_x > 0 ? origin_x : 1;
	m->origin_y = origin_y > 0 ? origin_y : 1;
	m->width = width > 0 ? width : 1;
	m->height = height > 0 ? height : 1;
	apply_rect(m);
	m->dirty = 1;
}

/* ------------------------------------------------------------------ */
/*  Chip-side feed                                                    */
/* ------------------------------------------------------------------ */

void monitor_feed_chip(struct monitor_session *m, const uint8_t *buf, size_t n)
{
	if (!m || n == 0)
		return;
	vt100_input(m->V, buf, n);

	struct sbuf *r = vt100_reply(m->V);
	if (r->len) {
		sbuf_add(&m->chip_tx, r->buf, r->len);
		sbuf_reset(r);
	}

	if (tui_log_is_frozen(&m->L)) {
		/* Modal up: drain V's scrolled-row queue without pushing
		 * it into the ring -- the frozen ceiling is the user's
		 * reading point and we don't want it to drift. */
		vt100_drain_scrolled(m->V);
	} else {
		tui_log_pull_from_vt100(&m->L, m->V);
		m->dirty = 1;
	}
}

struct sbuf *monitor_chip_tx(struct monitor_session *m)
{
	return m ? &m->chip_tx : NULL;
}

/* ------------------------------------------------------------------ */
/*  Mode transitions                                                  */
/* ------------------------------------------------------------------ */

static void enter_help(struct monitor_session *m)
{
	int target_mode =
	    mode_is_inspect(m->mode) ? MON_INSPECT_HELP : MON_NORMAL_HELP;
	const char *title = target_mode == MON_INSPECT_HELP
				? "ice monitor - inspect mode keys"
				: "ice monitor - normal mode keys";
	const char *body = target_mode == MON_INSPECT_HELP ? INSPECT_HELP_TEXT
							   : NORMAL_HELP_TEXT;

	tui_info_init(&m->help_info, title, body);
	tui_info_resize(&m->help_info, m->width, m->height);
	tui_log_freeze(&m->L);
	m->mode = target_mode;
}

static void enter_inspect(struct monitor_session *m)
{
	tui_log_freeze(&m->L);
	m->mode = MON_INSPECT;
}

static void leave_modal_and_unfreeze(struct monitor_session *m, int next_mode)
{
	if (m->mode == MON_NORMAL_HELP || m->mode == MON_INSPECT_HELP)
		tui_info_release(&m->help_info);
	/* tui_prompt is a flat struct -- nothing to release. */
	if (next_mode == MON_NORMAL)
		tui_log_unfreeze(&m->L);
	m->mode = next_mode;
}

/* ------------------------------------------------------------------ */
/*  Ctrl-T prefix dispatch                                            */
/* ------------------------------------------------------------------ */

/*
 * Resolve one byte arriving after a @c Ctrl-T prefix.  Mirrors the
 * standalone monitor's dispatch -- modes / actions / literal forward
 * are unchanged; the difference is that target-side actions go
 * through caller-provided callbacks (NULL = action unavailable on
 * this transport, dispatch becomes a silent prefix-cancel) and
 * literal @c Ctrl-T accumulates in the chip-TX buffer instead of
 * being written through to a serial port.
 */
static void dispatch_normal_prefix(struct monitor_session *m, int key)
{
	switch (key) {
	case 'x':
	case 'X':
		m->should_quit = 1;
		break;
	case 0x12: /* Ctrl-R */
	case 'r':
	case 'R':
		if (m->on_reset)
			m->on_reset(m->cb_ctx);
		break;
	case 0x10: /* Ctrl-P */
	case 'p':
	case 'P':
		if (m->on_bootloader)
			m->on_bootloader(m->cb_ctx);
		break;
	case 0x08: /* Ctrl-H */
	case 'h':
	case 'H':
	case '?':
		enter_help(m);
		break;
	case 'i':
	case 'I':
		enter_inspect(m);
		break;
	case 0x14: /* literal Ctrl-T */
		sbuf_addch(&m->chip_tx, 0x14);
		break;
	default:
		/* Esc / any other byte cancels the prefix without firing.
		 * Including unknown keys in the catch-all keeps mistakes
		 * cheap -- the user just tries again. */
		break;
	}
}

/* ------------------------------------------------------------------ */
/*  Inspect-mode dispatch                                             */
/* ------------------------------------------------------------------ */

static void dispatch_inspect(struct monitor_session *m,
			     const struct term_event *ev)
{
	if (ev->key == 0x1d) { /* Ctrl-]: panic eject */
		m->should_quit = 1;
		return;
	}

	int key = ev->key;
	if (key == 'j')
		key = TK_DOWN;
	else if (key == 'k')
		key = TK_UP;
	else if (key == 'g')
		key = TK_HOME;
	else if (key == 'G')
		key = TK_END;

	switch (key) {
	case TK_UP:
	case TK_DOWN:
	case TK_PGUP:
	case TK_PGDN:
	case TK_HOME:
	case TK_END: {
		struct term_event nav = {.key = key};
		tui_log_on_event(&m->L, &nav);
		return;
	}
	case '/':
		tui_prompt_init(&m->search_prompt, "search:",
				tui_log_search_pattern(&m->L)
				    ? tui_log_search_pattern(&m->L)
				    : "");
		tui_prompt_resize(&m->search_prompt, m->width, m->height);
		m->mode = MON_INSPECT_SEARCH;
		return;
	case 'n':
		tui_log_search_next(&m->L);
		return;
	case 'N':
		tui_log_search_prev(&m->L);
		return;
	case 'q':
	case TK_ESC:
		leave_modal_and_unfreeze(m, MON_NORMAL);
		return;
	case '?':
	case 'h':
	case 'H':
	case 0x08:
		enter_help(m);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------------------ */
/*  Top-level event feed                                              */
/* ------------------------------------------------------------------ */

void monitor_feed_event(struct monitor_session *m, const struct term_event *ev)
{
	if (!m || !ev || ev->key == TK_NONE)
		return;
	if (ev->key == TK_RESIZE) {
		monitor_set_rect(m, m->origin_x, m->origin_y, ev->cols,
				 ev->rows);
		return;
	}
	if (ev->key == 0x1d) { /* Ctrl-]: undocumented panic eject */
		m->should_quit = 1;
		m->dirty = 1;
		return;
	}

	switch (m->mode) {
	case MON_NORMAL:
		if (ev->key == 0x14) { /* Ctrl-T: enter prefix mode */
			m->mode = MON_NORMAL_PREFIX;
			m->dirty = 1;
		} else if (ev->key < 0x100) {
			sbuf_addch(&m->chip_tx, (int)ev->key);
		}
		/* TK_UP / TK_PGUP / etc. in NORMAL: not forwarded to chip
		 * (matches the dual-pane behaviour the standalone monitor
		 * inherits from term_read's raw bytes -- they'd survive as
		 * the encoded escape sequence there but we'd have to
		 * re-encode here, and chip-side consumers of arrow keys
		 * are rare enough to not justify the table). */
		break;

	case MON_NORMAL_PREFIX:
		dispatch_normal_prefix(m, ev->key);
		if (m->mode == MON_NORMAL_PREFIX)
			m->mode = MON_NORMAL;
		m->dirty = 1;
		break;

	case MON_NORMAL_HELP:
		if (tui_info_on_event(&m->help_info, ev))
			leave_modal_and_unfreeze(m, MON_NORMAL);
		m->dirty = 1;
		break;

	case MON_INSPECT:
		dispatch_inspect(m, ev);
		m->dirty = 1;
		break;

	case MON_INSPECT_SEARCH: {
		int r = tui_prompt_on_event(&m->search_prompt, ev);
		if (r == 1) {
			int rc =
			    tui_log_search_set(&m->L, m->search_prompt.buf);
			if (rc < 0)
				m->search_prompt.title =
				    "invalid regex - search:";
			else
				leave_modal_and_unfreeze(m, MON_INSPECT);
		} else if (r == -1) {
			leave_modal_and_unfreeze(m, MON_INSPECT);
		}
		m->dirty = 1;
		break;
	}

	case MON_INSPECT_HELP:
		if (tui_info_on_event(&m->help_info, ev))
			leave_modal_and_unfreeze(m, MON_INSPECT);
		m->dirty = 1;
		break;
	}
}

/* ------------------------------------------------------------------ */
/*  Render                                                            */
/* ------------------------------------------------------------------ */

void monitor_render(struct sbuf *out, struct monitor_session *m)
{
	if (!out || !m)
		return;
	update_status(m);
	tui_log_render(out, &m->L);
	if (m->mode == MON_INSPECT_SEARCH)
		tui_prompt_render(out, &m->search_prompt);
	else if (m->mode == MON_NORMAL_HELP || m->mode == MON_INSPECT_HELP)
		tui_info_render(out, &m->help_info);
}

/* ------------------------------------------------------------------ */
/*  Trivial getters                                                   */
/* ------------------------------------------------------------------ */

int monitor_should_quit(const struct monitor_session *m)
{
	return m ? m->should_quit : 1;
}

int monitor_dirty(const struct monitor_session *m) { return m ? m->dirty : 0; }

void monitor_clear_dirty(struct monitor_session *m)
{
	if (m)
		m->dirty = 0;
}

int monitor_cursor(const struct monitor_session *m, int *row, int *col)
{
	if (!m || !row || !col)
		return 0;
	/* Modal up: cursor lives inside the modal (or hidden); leave
	 * placement to whoever drew the modal last. */
	if (m->mode != MON_NORMAL && m->mode != MON_NORMAL_PREFIX &&
	    m->mode != MON_INSPECT)
		return 0;
	if (!tui_log_is_tailing(&m->L))
		return 0;
	if (!vt100_cursor_visible(m->V))
		return 0;
	/* Body sits one row below the status bar; vt100 cursor is 0-based. */
	*row = m->origin_y + 1 + vt100_cursor_row(m->V);
	*col = m->origin_x + vt100_cursor_col(m->V);
	return 1;
}
