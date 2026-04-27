/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file vt100.c
 * @brief Paul Williams DEC-ANSI parser + cell grid mutation.
 *
 * Tracks the canonical state diagram from
 * https://vt100.net/emu/dec_ansi_parser.  Phase 2 wires real dispatch:
 * putc with deferred wrap, cursor movement (CUU/CUD/CUF/CUB, CUP/HVP,
 * CHA), erase (EL, ED), graphic rendition (SGR), DECSET/DECRST for
 * autowrap (?7) and cursor visibility (?25), and DECSC/DECRC.
 *
 * Implicit scroll past the bottom margin is phase 3 -- the cursor
 * clamps for now.  DSR synth lands in phase 4.
 *
 * Layout in this file:
 *   1. parser state enum
 *   2. struct vt100 definition
 *   3. lifecycle (new / free / resize)
 *   4. low-level helpers (cell access, erase, sgr)
 *   5. C0 / putc / cursor primitives
 *   6. CSI param collection + dispatch
 *   7. ESC dispatch
 *   8. parser state machine (vt100_input)
 *   9. observability accessors
 */
#include "vt100.h"

#include "ice.h"

#define VT100_MAX_PARAMS 16

/*
 * Hard cap on the reply sbuf.  Sized for many normal DSR / CPR
 * round-trips between caller drains; on overflow we drop the oldest
 * bytes (linenoise re-probes if its expected reply is missing, so
 * dropping is safe).  Keep private to vt100.c -- callers should treat
 * the buffer as bounded but not assume the exact value.
 */
#define VT100_REPLY_CAP 256

/* ------------------------------------------------------------------ */
/*  Parser state                                                      */
/* ------------------------------------------------------------------ */

enum vt100_state {
	VT100_GROUND,
	VT100_ESCAPE,
	VT100_ESCAPE_INTERMEDIATE,
	VT100_CSI_ENTRY,
	VT100_CSI_PARAM,
	VT100_CSI_INTERMEDIATE,
	VT100_CSI_IGNORE,
	VT100_DCS_ENTRY,
	VT100_DCS_PARAM,
	VT100_DCS_INTERMEDIATE,
	VT100_DCS_PASSTHROUGH,
	VT100_DCS_IGNORE,
	VT100_OSC_STRING,
	VT100_SOS_PM_APC_STRING,
	/*
	 * Pseudo-state for "saw ESC inside a string state".  ESC \\ is
	 * the canonical 7-bit form of ST (string terminator); anything
	 * else cancels the string and re-feeds the byte through ESCAPE
	 * so that ESC <X> starts a fresh sequence.
	 */
	VT100_STRING_ESC,
};

/* ------------------------------------------------------------------ */
/*  struct vt100                                                      */
/* ------------------------------------------------------------------ */

/*
 * One row of cells that has scrolled off the top of the live grid and
 * is queued for the caller (typically tui.c) to materialise into a
 * scrollback ring.  Width is stored per-row because vt100_resize
 * between scrolls can change @c cols.
 */
struct vt100_scrolled_row {
	struct vt100_cell *cells; /* malloc'd, length == cols */
	int cols;
};

struct vt100 {
	int rows;
	int cols;
	struct vt100_cell *grid; /* rows * cols flat array. */

	/* Live cursor + rendition. */
	int cur_row;
	int cur_col;
	struct vt100_sgr sgr;
	int pending_wrap;

	/* Modes. */
	int autowrap;
	int cursor_visible;
	int alt_screen;

	/* Scroll region (0-based, inclusive). */
	int scroll_top;
	int scroll_bottom;

	/* Saved cursor (DECSC / DECRC). */
	int saved_row;
	int saved_col;
	int saved_pending_wrap;
	struct vt100_sgr saved_sgr;

	/* Parser state. */
	enum vt100_state state;
	enum vt100_state string_origin;

	/* CSI param collection. */
	int params[VT100_MAX_PARAMS];
	int param_count;
	int param_in_progress;
	unsigned char csi_priv;

	struct sbuf reply;
	struct vt100_counters counters;

	/* Scrolled-off rows queue (drained by the caller). */
	struct vt100_scrolled_row *scrolled;
	int scrolled_count;
	int scrolled_alloc;
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static struct vt100_sgr sgr_default(void)
{
	struct vt100_sgr s = {.fg = VT100_DEFAULT_COLOR,
			      .bg = VT100_DEFAULT_COLOR,
			      .attrs = 0,
			      ._pad = 0};
	return s;
}

static struct vt100_cell *cell_at(struct vt100 *V, int row, int col)
{
	return &V->grid[(size_t)row * V->cols + col];
}

static void grid_blank(struct vt100 *V)
{
	struct vt100_sgr d = sgr_default();
	size_t n = (size_t)V->rows * V->cols;

	for (size_t i = 0; i < n; i++) {
		V->grid[i].cp = 0;
		V->grid[i].sgr = d;
	}
}

/* Erase cells in the inclusive rectangle [r0,c0]..[r1,c1] using the
 * current SGR, so the bg color paints through (xterm convention). */
static void cells_erase(struct vt100 *V, int r0, int c0, int r1, int c1)
{
	if (r0 < 0)
		r0 = 0;
	if (c0 < 0)
		c0 = 0;
	if (r1 >= V->rows)
		r1 = V->rows - 1;
	if (c1 >= V->cols)
		c1 = V->cols - 1;
	if (r0 > r1)
		return;

	for (int r = r0; r <= r1; r++) {
		int sc = (r == r0) ? c0 : 0;
		int ec = (r == r1) ? c1 : V->cols - 1;

		for (int c = sc; c <= ec; c++) {
			cell_at(V, r, c)->cp = 0;
			cell_at(V, r, c)->sgr = V->sgr;
		}
	}
}

static struct vt100_cell *grid_alloc(int rows, int cols)
{
	struct vt100_cell *g = calloc((size_t)rows * cols, sizeof(*g));

	if (!g)
		die_errno("calloc");
	return g;
}

/* ------------------------------------------------------------------ */
/*  Scrolled-off queue + scroll primitives                            */
/* ------------------------------------------------------------------ */

static int scroll_is_full_screen(const struct vt100 *V)
{
	return V->scroll_top == 0 && V->scroll_bottom == V->rows - 1;
}

static void scrolled_push(struct vt100 *V, int row)
{
	ALLOC_GROW(V->scrolled, V->scrolled_count + 1, V->scrolled_alloc);
	struct vt100_scrolled_row *r = &V->scrolled[V->scrolled_count++];

	r->cols = V->cols;
	r->cells = malloc((size_t)V->cols * sizeof(*r->cells));
	if (!r->cells)
		die_errno("malloc");
	memcpy(r->cells, &V->grid[(size_t)row * V->cols],
	       (size_t)V->cols * sizeof(struct vt100_cell));
}

static void scrolled_clear(struct vt100 *V)
{
	for (int i = 0; i < V->scrolled_count; i++)
		free(V->scrolled[i].cells);
	V->scrolled_count = 0;
}

/*
 * Scroll the active scroll region up by @p n rows.  Top @p n rows of
 * the region are dropped from the grid; rows below shift up; the
 * bottom @p n rows blank with current SGR.  When the scroll region
 * spans the full screen and alt-screen is off, dropped rows are
 * enqueued into the scrolled-off list (so tui.c can materialise them
 * into scrollback); otherwise they vanish.
 */
static void scroll_up(struct vt100 *V, int n)
{
	int top = V->scroll_top;
	int bot = V->scroll_bottom;
	int region_h = bot - top + 1;
	int do_enqueue = scroll_is_full_screen(V) && !V->alt_screen;

	if (n <= 0)
		return;
	if (n > region_h)
		n = region_h;

	if (do_enqueue) {
		for (int i = 0; i < n; i++)
			scrolled_push(V, top + i);
	}

	for (int r = top; r + n <= bot; r++)
		memcpy(&V->grid[(size_t)r * V->cols],
		       &V->grid[(size_t)(r + n) * V->cols],
		       (size_t)V->cols * sizeof(struct vt100_cell));

	for (int r = bot - n + 1; r <= bot; r++)
		cells_erase(V, r, 0, r, V->cols - 1);
}

static void scroll_down(struct vt100 *V, int n)
{
	int top = V->scroll_top;
	int bot = V->scroll_bottom;
	int region_h = bot - top + 1;

	if (n <= 0)
		return;
	if (n > region_h)
		n = region_h;

	for (int r = bot; r - n >= top; r--)
		memcpy(&V->grid[(size_t)r * V->cols],
		       &V->grid[(size_t)(r - n) * V->cols],
		       (size_t)V->cols * sizeof(struct vt100_cell));

	for (int r = top; r < top + n; r++)
		cells_erase(V, r, 0, r, V->cols - 1);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

struct vt100 *vt100_new(int rows, int cols)
{
	struct vt100 *V = calloc(1, sizeof(*V));

	if (!V)
		die_errno("calloc");

	if (rows < 1)
		rows = 1;
	if (cols < 1)
		cols = 1;
	V->rows = rows;
	V->cols = cols;
	V->grid = grid_alloc(rows, cols);

	V->sgr = sgr_default();
	V->saved_sgr = sgr_default();
	V->autowrap = 1;
	V->cursor_visible = 1;
	V->scroll_top = 0;
	V->scroll_bottom = rows - 1;
	V->state = VT100_GROUND;
	grid_blank(V);
	sbuf_init(&V->reply);
	return V;
}

void vt100_free(struct vt100 *V)
{
	if (!V)
		return;
	free(V->grid);
	scrolled_clear(V);
	free(V->scrolled);
	sbuf_release(&V->reply);
	free(V);
}

void vt100_resize(struct vt100 *V, int rows, int cols)
{
	if (rows < 1)
		rows = 1;
	if (cols < 1)
		cols = 1;
	if (rows == V->rows && cols == V->cols)
		return;

	free(V->grid);
	V->rows = rows;
	V->cols = cols;
	V->grid = grid_alloc(rows, cols);
	grid_blank(V);

	if (V->cur_row >= rows)
		V->cur_row = rows - 1;
	if (V->cur_col >= cols)
		V->cur_col = cols - 1;
	if (V->saved_row >= rows)
		V->saved_row = rows - 1;
	if (V->saved_col >= cols)
		V->saved_col = cols - 1;
	V->pending_wrap = 0;
	/* Reset margins to full screen.  Real terminals tend to clamp;
	 * a chip program that cares re-emits DECSTBM after a resize. */
	V->scroll_top = 0;
	V->scroll_bottom = rows - 1;
	/* Parser state, reply buffer, counters, SGR, scrolled queue
	 * (with their original cols) survive resize. */
}

/* ------------------------------------------------------------------ */
/*  Cursor primitives                                                 */
/* ------------------------------------------------------------------ */

static void cursor_clamp(struct vt100 *V)
{
	if (V->cur_row < 0)
		V->cur_row = 0;
	if (V->cur_row >= V->rows)
		V->cur_row = V->rows - 1;
	if (V->cur_col < 0)
		V->cur_col = 0;
	if (V->cur_col >= V->cols)
		V->cur_col = V->cols - 1;
}

static void cursor_move(struct vt100 *V, int row, int col)
{
	V->cur_row = row;
	V->cur_col = col;
	cursor_clamp(V);
	V->pending_wrap = 0;
}

/*
 * Move the cursor down one row.  At the bottom of the scroll region
 * this scrolls the region up by one (cursor stays at scroll_bottom);
 * outside the scroll region the cursor advances and clamps at the
 * last row.
 */
static void cursor_advance_row(struct vt100 *V)
{
	if (V->cur_row == V->scroll_bottom)
		scroll_up(V, 1);
	else if (V->cur_row + 1 < V->rows)
		V->cur_row++;
}

/* ------------------------------------------------------------------ */
/*  putc + C0                                                         */
/* ------------------------------------------------------------------ */

static void put_printable(struct vt100 *V, unsigned char c)
{
	if (V->pending_wrap) {
		if (V->autowrap) {
			V->cur_col = 0;
			cursor_advance_row(V);
		}
		V->pending_wrap = 0;
	}

	struct vt100_cell *cell = cell_at(V, V->cur_row, V->cur_col);

	cell->cp = c;
	cell->sgr = V->sgr;

	if (V->cur_col + 1 < V->cols)
		V->cur_col++;
	else
		V->pending_wrap = 1;

	V->counters.printable++;
}

static void c0_execute(struct vt100 *V, unsigned char c)
{
	V->counters.c0++;

	switch (c) {
	case '\n':
	case 0x0B: /* VT */
	case 0x0C: /* FF */
		V->pending_wrap = 0;
		cursor_advance_row(V);
		break;
	case '\r':
		V->pending_wrap = 0;
		V->cur_col = 0;
		break;
	case '\b':
		V->pending_wrap = 0;
		if (V->cur_col > 0)
			V->cur_col--;
		break;
	case '\t':
		V->pending_wrap = 0;
		do {
			V->cur_col++;
		} while (V->cur_col < V->cols - 1 && (V->cur_col & 7) != 0);
		if (V->cur_col >= V->cols)
			V->cur_col = V->cols - 1;
		break;
	default:
		/* BEL (0x07) and other C0 controls are no-ops in phase 2. */
		break;
	}
}

/* ------------------------------------------------------------------ */
/*  CSI param collection                                              */
/* ------------------------------------------------------------------ */

static void csi_clear(struct vt100 *V)
{
	V->param_count = 0;
	V->param_in_progress = 0;
	V->csi_priv = 0;
	for (int i = 0; i < VT100_MAX_PARAMS; i++)
		V->params[i] = 0;
}

static void csi_param_digit(struct vt100 *V, unsigned char c)
{
	if (V->param_count >= VT100_MAX_PARAMS)
		return;
	if (!V->param_in_progress) {
		V->params[V->param_count] = 0;
		V->param_in_progress = 1;
	}
	int v = V->params[V->param_count] * 10 + (c - '0');

	if (v > 99999)
		v = 99999;
	V->params[V->param_count] = v;
}

static void csi_param_separator(struct vt100 *V)
{
	if (V->param_count < VT100_MAX_PARAMS) {
		V->param_count++;
		V->param_in_progress = 0;
	}
}

static void csi_param_finalize(struct vt100 *V)
{
	if (V->param_in_progress && V->param_count < VT100_MAX_PARAMS) {
		V->param_count++;
		V->param_in_progress = 0;
	}
}

static int csi_param(const struct vt100 *V, int idx)
{
	return idx < V->param_count ? V->params[idx] : 0;
}

/* "0 or missing" → @p dflt; otherwise the literal value. */
static int csi_param_or(const struct vt100 *V, int idx, int dflt)
{
	int v = csi_param(V, idx);

	return v ? v : dflt;
}

/* ------------------------------------------------------------------ */
/*  Reply (DSR-style replies destined for the chip)                   */
/* ------------------------------------------------------------------ */

/*
 * Append @p n bytes from @p s to the reply sbuf, enforcing
 * VT100_REPLY_CAP by dropping the oldest bytes on overflow.  When @p n
 * already meets or exceeds the cap, the existing content is discarded
 * and only the trailing @c VT100_REPLY_CAP bytes of @p s are kept.
 */
static void reply_append(struct vt100 *V, const char *s, size_t n)
{
	struct sbuf *r = &V->reply;

	if (n == 0)
		return;
	if (n >= VT100_REPLY_CAP) {
		sbuf_reset(r);
		sbuf_add(r, s + (n - VT100_REPLY_CAP), VT100_REPLY_CAP);
		return;
	}
	if (r->len + n > VT100_REPLY_CAP) {
		size_t drop = r->len + n - VT100_REPLY_CAP;

		memmove(r->buf, r->buf + drop, r->len - drop);
		sbuf_setlen(r, r->len - drop);
	}
	sbuf_add(r, s, n);
}

/* ------------------------------------------------------------------ */
/*  SGR                                                               */
/* ------------------------------------------------------------------ */

static void sgr_apply(struct vt100 *V, int p)
{
	if (p == 0) {
		V->sgr = sgr_default();
	} else if (p == 1) {
		V->sgr.attrs |= VT100_ATTR_BOLD;
	} else if (p == 4) {
		V->sgr.attrs |= VT100_ATTR_UNDERLINE;
	} else if (p == 7) {
		V->sgr.attrs |= VT100_ATTR_REVERSE;
	} else if (p == 22) {
		V->sgr.attrs &= (uint8_t)~VT100_ATTR_BOLD;
	} else if (p == 24) {
		V->sgr.attrs &= (uint8_t)~VT100_ATTR_UNDERLINE;
	} else if (p == 27) {
		V->sgr.attrs &= (uint8_t)~VT100_ATTR_REVERSE;
	} else if (p >= 30 && p <= 37) {
		V->sgr.fg = (uint8_t)(p - 30);
	} else if (p == 39) {
		V->sgr.fg = VT100_DEFAULT_COLOR;
	} else if (p >= 40 && p <= 47) {
		V->sgr.bg = (uint8_t)(p - 40);
	} else if (p == 49) {
		V->sgr.bg = VT100_DEFAULT_COLOR;
	} else if (p >= 90 && p <= 97) {
		V->sgr.fg = (uint8_t)(8 + (p - 90));
	} else if (p >= 100 && p <= 107) {
		V->sgr.bg = (uint8_t)(8 + (p - 100));
	}
	/* 38 / 48 (extended fg/bg) and unknown params are silently ignored. */
}

/* ------------------------------------------------------------------ */
/*  CSI dispatch                                                      */
/* ------------------------------------------------------------------ */

static void csi_erase_line(struct vt100 *V, int p)
{
	switch (p) {
	case 0:
		cells_erase(V, V->cur_row, V->cur_col, V->cur_row, V->cols - 1);
		break;
	case 1:
		cells_erase(V, V->cur_row, 0, V->cur_row, V->cur_col);
		break;
	case 2:
		cells_erase(V, V->cur_row, 0, V->cur_row, V->cols - 1);
		break;
	default:
		break;
	}
}

static void csi_erase_display(struct vt100 *V, int p)
{
	switch (p) {
	case 0:
		cells_erase(V, V->cur_row, V->cur_col, V->cur_row, V->cols - 1);
		cells_erase(V, V->cur_row + 1, 0, V->rows - 1, V->cols - 1);
		break;
	case 1:
		cells_erase(V, 0, 0, V->cur_row - 1, V->cols - 1);
		cells_erase(V, V->cur_row, 0, V->cur_row, V->cur_col);
		break;
	case 2:
	case 3: /* xterm: also clear scrollback -- phase 5 wires that. */
		cells_erase(V, 0, 0, V->rows - 1, V->cols - 1);
		break;
	default:
		break;
	}
}

static void csi_decset(struct vt100 *V, int set)
{
	if (V->csi_priv != '?')
		return;
	for (int i = 0; i < V->param_count; i++) {
		switch (V->params[i]) {
		case 7:
			V->autowrap = set;
			break;
		case 25:
			V->cursor_visible = set;
			break;
		case 47:
		case 1047:
		case 1049:
			/*
			 * Track the alt-screen flag; phase 3 doesn't
			 * actually maintain a second grid.  Cursor
			 * save/restore semantics for ?1049 vs ?1047
			 * are deferred until a chip program needs it.
			 */
			V->alt_screen = set;
			break;
		default:
			break;
		}
	}
}

static void csi_sgr(struct vt100 *V)
{
	if (V->param_count == 0) {
		sgr_apply(V, 0);
		return;
	}
	for (int i = 0; i < V->param_count; i++)
		sgr_apply(V, V->params[i]);
}

static void csi_dispatch(struct vt100 *V, unsigned char final)
{
	csi_param_finalize(V);
	V->counters.csi++;

	switch (final) {
	case 'A': /* CUU */
		cursor_move(V, V->cur_row - csi_param_or(V, 0, 1), V->cur_col);
		break;
	case 'B': /* CUD */
		cursor_move(V, V->cur_row + csi_param_or(V, 0, 1), V->cur_col);
		break;
	case 'C': /* CUF */
		cursor_move(V, V->cur_row, V->cur_col + csi_param_or(V, 0, 1));
		break;
	case 'D': /* CUB */
		cursor_move(V, V->cur_row, V->cur_col - csi_param_or(V, 0, 1));
		break;
	case 'G': /* CHA */
		cursor_move(V, V->cur_row, csi_param_or(V, 0, 1) - 1);
		break;
	case 'H': /* CUP */
	case 'f': /* HVP */
		cursor_move(V, csi_param_or(V, 0, 1) - 1,
			    csi_param_or(V, 1, 1) - 1);
		break;
	case 'J':
		csi_erase_display(V, csi_param(V, 0));
		break;
	case 'K':
		csi_erase_line(V, csi_param(V, 0));
		break;
	case 'h':
		csi_decset(V, 1);
		break;
	case 'l':
		csi_decset(V, 0);
		break;
	case 'm':
		csi_sgr(V);
		break;
	case 'n': { /* DSR -- device status report */
		if (V->csi_priv)
			break; /* private DSR (?6n etc.) not supported */
		int kind = csi_param(V, 0);

		if (kind == 5) {
			reply_append(V, "\x1b[0n", 4);
		} else if (kind == 6) {
			char buf[24];
			int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dR",
					 V->cur_row + 1, V->cur_col + 1);

			if (n > 0)
				reply_append(V, buf, (size_t)n);
		}
		break;
	}
	case 'r': { /* DECSTBM */
		int top = csi_param_or(V, 0, 1) - 1;
		int bot = csi_param_or(V, 1, V->rows) - 1;

		if (top < 0)
			top = 0;
		if (bot >= V->rows)
			bot = V->rows - 1;
		if (top < bot) {
			V->scroll_top = top;
			V->scroll_bottom = bot;
			cursor_move(V, 0, 0);
		}
		break;
	}
	case 'S': /* SU -- scroll region up */
		scroll_up(V, csi_param_or(V, 0, 1));
		break;
	case 'T': /* SD -- scroll region down */
		scroll_down(V, csi_param_or(V, 0, 1));
		break;
	case 'L': { /* IL -- insert lines */
		if (V->cur_row < V->scroll_top || V->cur_row > V->scroll_bottom)
			break;
		int n = csi_param_or(V, 0, 1);
		int avail = V->scroll_bottom - V->cur_row + 1;

		if (n > avail)
			n = avail;
		for (int r = V->scroll_bottom; r - n >= V->cur_row; r--)
			memcpy(&V->grid[(size_t)r * V->cols],
			       &V->grid[(size_t)(r - n) * V->cols],
			       (size_t)V->cols * sizeof(struct vt100_cell));
		for (int r = V->cur_row; r < V->cur_row + n; r++)
			cells_erase(V, r, 0, r, V->cols - 1);
		V->cur_col = 0;
		V->pending_wrap = 0;
		break;
	}
	case 'M': { /* DL -- delete lines */
		if (V->cur_row < V->scroll_top || V->cur_row > V->scroll_bottom)
			break;
		int n = csi_param_or(V, 0, 1);
		int avail = V->scroll_bottom - V->cur_row + 1;

		if (n > avail)
			n = avail;
		for (int r = V->cur_row; r + n <= V->scroll_bottom; r++)
			memcpy(&V->grid[(size_t)r * V->cols],
			       &V->grid[(size_t)(r + n) * V->cols],
			       (size_t)V->cols * sizeof(struct vt100_cell));
		for (int r = V->scroll_bottom - n + 1; r <= V->scroll_bottom;
		     r++)
			cells_erase(V, r, 0, r, V->cols - 1);
		V->cur_col = 0;
		V->pending_wrap = 0;
		break;
	}
	case '@': { /* ICH -- insert characters */
		int n = csi_param_or(V, 0, 1);
		int avail = V->cols - V->cur_col;

		if (n > avail)
			n = avail;
		for (int co = V->cols - 1; co - n >= V->cur_col; co--)
			V->grid[(size_t)V->cur_row * V->cols + co] =
			    V->grid[(size_t)V->cur_row * V->cols + co - n];
		for (int co = V->cur_col; co < V->cur_col + n; co++) {
			V->grid[(size_t)V->cur_row * V->cols + co].cp = 0;
			V->grid[(size_t)V->cur_row * V->cols + co].sgr = V->sgr;
		}
		V->pending_wrap = 0;
		break;
	}
	case 'P': { /* DCH -- delete characters */
		int n = csi_param_or(V, 0, 1);
		int avail = V->cols - V->cur_col;

		if (n > avail)
			n = avail;
		for (int co = V->cur_col; co + n < V->cols; co++)
			V->grid[(size_t)V->cur_row * V->cols + co] =
			    V->grid[(size_t)V->cur_row * V->cols + co + n];
		for (int co = V->cols - n; co < V->cols; co++) {
			V->grid[(size_t)V->cur_row * V->cols + co].cp = 0;
			V->grid[(size_t)V->cur_row * V->cols + co].sgr = V->sgr;
		}
		V->pending_wrap = 0;
		break;
	}
	default:
		/* Other final bytes silently consumed; phase 4+ extends. */
		break;
	}
}

/* ------------------------------------------------------------------ */
/*  ESC dispatch                                                      */
/* ------------------------------------------------------------------ */

static void esc_dispatch(struct vt100 *V, unsigned char final)
{
	V->counters.esc++;

	switch (final) {
	case '7': /* DECSC */
		V->saved_row = V->cur_row;
		V->saved_col = V->cur_col;
		V->saved_sgr = V->sgr;
		V->saved_pending_wrap = V->pending_wrap;
		break;
	case '8': /* DECRC */
		V->cur_row = V->saved_row;
		V->cur_col = V->saved_col;
		V->sgr = V->saved_sgr;
		V->pending_wrap = V->saved_pending_wrap;
		cursor_clamp(V);
		break;
	default:
		/* Charset designation (ESC ( B), ESC = / >, etc. are
		 * no-ops in phase 2.  The parser still classifies them
		 * as esc dispatches via the counter bump above. */
		break;
	}
}

/* ------------------------------------------------------------------ */
/*  Parser state machine                                              */
/* ------------------------------------------------------------------ */

static int is_string_state(enum vt100_state s)
{
	return s == VT100_OSC_STRING || s == VT100_DCS_PASSTHROUGH ||
	       s == VT100_DCS_IGNORE || s == VT100_SOS_PM_APC_STRING;
}

void vt100_input(struct vt100 *V, const void *buf, size_t n)
{
	const unsigned char *bytes = buf;
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char c = bytes[i];

	reprocess:
		/*
		 * "Anywhere" transitions for non-string states.
		 *
		 * String states (OSC / DCS-passthrough / DCS-ignore /
		 * SOS-PM-APC) handle ESC themselves so an embedded ESC
		 * \\ can act as ST.  STRING_ESC is treated like a
		 * regular state here -- a fresh ESC just supersedes the
		 * pending one and a CAN/SUB cancels back to GROUND.
		 */
		if (!is_string_state(V->state)) {
			if (c == 0x18 || c == 0x1A) {
				V->state = VT100_GROUND;
				continue;
			}
			if (c == 0x1B) {
				V->state = VT100_ESCAPE;
				continue;
			}
		}

		switch (V->state) {
		case VT100_GROUND:
			if (c == 0x7F)
				break;
			if (c < 0x20)
				c0_execute(V, c);
			else
				put_printable(V, c);
			break;

		case VT100_ESCAPE:
			if (c == 0x7F)
				break;
			if (c < 0x20) {
				c0_execute(V, c);
				break;
			}
			if (c == 0x5B) { /* '[' */
				csi_clear(V);
				V->state = VT100_CSI_ENTRY;
			} else if (c == 0x5D) { /* ']' */
				V->state = VT100_OSC_STRING;
			} else if (c == 0x50) { /* 'P' */
				csi_clear(V);
				V->state = VT100_DCS_ENTRY;
			} else if (c == 0x58 || c == 0x5E ||
				   c == 0x5F) { /* X / ^ / _ */
				V->state = VT100_SOS_PM_APC_STRING;
			} else if (c >= 0x20 && c <= 0x2F) {
				V->state = VT100_ESCAPE_INTERMEDIATE;
			} else if (c >= 0x30 && c <= 0x7E) {
				esc_dispatch(V, c);
				V->state = VT100_GROUND;
			} else {
				V->state = VT100_GROUND;
			}
			break;

		case VT100_ESCAPE_INTERMEDIATE:
			if (c == 0x7F)
				break;
			if (c < 0x20) {
				c0_execute(V, c);
				break;
			}
			if (c >= 0x20 && c <= 0x2F) {
				/* collect another intermediate; stay */
			} else if (c >= 0x30 && c <= 0x7E) {
				esc_dispatch(V, c);
				V->state = VT100_GROUND;
			}
			break;

		case VT100_CSI_ENTRY:
			if (c == 0x7F)
				break;
			if (c < 0x20) {
				c0_execute(V, c);
				break;
			}
			if (c >= 0x20 && c <= 0x2F) {
				V->state = VT100_CSI_INTERMEDIATE;
			} else if (c == 0x3A) {
				V->state = VT100_CSI_IGNORE;
			} else if (c >= 0x30 && c <= 0x39) {
				csi_param_digit(V, c);
				V->state = VT100_CSI_PARAM;
			} else if (c == 0x3B) {
				csi_param_separator(V);
				V->state = VT100_CSI_PARAM;
			} else if (c >= 0x3C && c <= 0x3F) {
				V->csi_priv = c;
				V->state = VT100_CSI_PARAM;
			} else if (c >= 0x40 && c <= 0x7E) {
				csi_dispatch(V, c);
				V->state = VT100_GROUND;
			}
			break;

		case VT100_CSI_PARAM:
			if (c == 0x7F)
				break;
			if (c < 0x20) {
				c0_execute(V, c);
				break;
			}
			if (c >= 0x30 && c <= 0x39) {
				csi_param_digit(V, c);
			} else if (c == 0x3B) {
				csi_param_separator(V);
			} else if (c == 0x3A || (c >= 0x3C && c <= 0x3F)) {
				V->state = VT100_CSI_IGNORE;
			} else if (c >= 0x20 && c <= 0x2F) {
				V->state = VT100_CSI_INTERMEDIATE;
			} else if (c >= 0x40 && c <= 0x7E) {
				csi_dispatch(V, c);
				V->state = VT100_GROUND;
			}
			break;

		case VT100_CSI_INTERMEDIATE:
			if (c == 0x7F)
				break;
			if (c < 0x20) {
				c0_execute(V, c);
				break;
			}
			if (c >= 0x20 && c <= 0x2F) {
				/* collect another intermediate; stay */
			} else if (c >= 0x30 && c <= 0x3F) {
				V->state = VT100_CSI_IGNORE;
			} else if (c >= 0x40 && c <= 0x7E) {
				csi_dispatch(V, c);
				V->state = VT100_GROUND;
			}
			break;

		case VT100_CSI_IGNORE:
			if (c == 0x7F)
				break;
			if (c < 0x20) {
				c0_execute(V, c);
				break;
			}
			if (c >= 0x40 && c <= 0x7E)
				V->state = VT100_GROUND;
			/* else stay -- absorb garbage params/intermediates */
			break;

		case VT100_DCS_ENTRY:
			/* C0 is "ignore" in DCS_*; do not bump counters. */
			if (c == 0x7F)
				break;
			if (c < 0x20)
				break;
			if (c >= 0x20 && c <= 0x2F)
				V->state = VT100_DCS_INTERMEDIATE;
			else if ((c >= 0x30 && c <= 0x39) || c == 0x3B)
				V->state = VT100_DCS_PARAM;
			else if (c == 0x3A || (c >= 0x3C && c <= 0x3F))
				V->state = VT100_DCS_IGNORE;
			else if (c >= 0x40 && c <= 0x7E)
				V->state = VT100_DCS_PASSTHROUGH;
			break;

		case VT100_DCS_PARAM:
			if (c == 0x7F)
				break;
			if (c < 0x20)
				break;
			if ((c >= 0x30 && c <= 0x39) || c == 0x3B) {
				/* collect another param byte; stay */
			} else if (c == 0x3A || (c >= 0x3C && c <= 0x3F)) {
				V->state = VT100_DCS_IGNORE;
			} else if (c >= 0x20 && c <= 0x2F) {
				V->state = VT100_DCS_INTERMEDIATE;
			} else if (c >= 0x40 && c <= 0x7E) {
				V->state = VT100_DCS_PASSTHROUGH;
			}
			break;

		case VT100_DCS_INTERMEDIATE:
			if (c == 0x7F)
				break;
			if (c < 0x20)
				break;
			if (c >= 0x20 && c <= 0x2F) {
				/* collect another intermediate; stay */
			} else if (c >= 0x30 && c <= 0x3F) {
				V->state = VT100_DCS_IGNORE;
			} else if (c >= 0x40 && c <= 0x7E) {
				V->state = VT100_DCS_PASSTHROUGH;
			}
			break;

		case VT100_DCS_PASSTHROUGH:
			if (c == 0x1B) {
				V->state = VT100_STRING_ESC;
				V->string_origin = VT100_DCS_PASSTHROUGH;
			} else if (c == 0x18 || c == 0x1A) {
				V->state = VT100_GROUND;
			}
			/* else "put" -- collected; dropped in phase 2. */
			break;

		case VT100_DCS_IGNORE:
			if (c == 0x1B) {
				V->state = VT100_STRING_ESC;
				V->string_origin = VT100_DCS_IGNORE;
			} else if (c == 0x18 || c == 0x1A) {
				V->state = VT100_GROUND;
			}
			break;

		case VT100_OSC_STRING:
			/*
			 * BEL (0x07) is the xterm-style OSC terminator and
			 * widely used in real chip output.  ESC opens
			 * STRING_ESC awaiting \\ for the canonical ST.
			 */
			if (c == 0x07) {
				V->counters.osc++;
				V->state = VT100_GROUND;
			} else if (c == 0x1B) {
				V->state = VT100_STRING_ESC;
				V->string_origin = VT100_OSC_STRING;
			} else if (c == 0x18 || c == 0x1A) {
				V->state = VT100_GROUND;
			}
			/* else osc_put -- collected; dropped in phase 2. */
			break;

		case VT100_SOS_PM_APC_STRING:
			if (c == 0x1B) {
				V->state = VT100_STRING_ESC;
				V->string_origin = VT100_SOS_PM_APC_STRING;
			} else if (c == 0x18 || c == 0x1A) {
				V->state = VT100_GROUND;
			}
			break;

		case VT100_STRING_ESC:
			if (c == 0x5C) { /* ESC \\  =  ST */
				if (V->string_origin == VT100_OSC_STRING)
					V->counters.osc++;
				V->state = VT100_GROUND;
			} else {
				/*
				 * The ESC didn't introduce ST.  The pending
				 * string is abandoned; the new byte is the
				 * first one of a fresh ESC sequence.  Re-feed
				 * it through ESCAPE.
				 */
				V->state = VT100_ESCAPE;
				goto reprocess;
			}
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/*  Accessors                                                         */
/* ------------------------------------------------------------------ */

struct sbuf *vt100_reply(struct vt100 *V) { return &V->reply; }

int vt100_rows(const struct vt100 *V) { return V->rows; }

int vt100_cols(const struct vt100 *V) { return V->cols; }

const struct vt100_cell *vt100_cell(const struct vt100 *V, int row, int col)
{
	if (row < 0 || row >= V->rows)
		return NULL;
	if (col < 0 || col >= V->cols)
		return NULL;
	return &V->grid[(size_t)row * V->cols + col];
}

int vt100_cursor_row(const struct vt100 *V) { return V->cur_row; }

int vt100_cursor_col(const struct vt100 *V) { return V->cur_col; }

int vt100_cursor_visible(const struct vt100 *V) { return V->cursor_visible; }

int vt100_pending_wrap(const struct vt100 *V) { return V->pending_wrap; }

int vt100_idle(const struct vt100 *V) { return V->state == VT100_GROUND; }

const struct vt100_counters *vt100_counters(const struct vt100 *V)
{
	return &V->counters;
}

int vt100_scroll_top(const struct vt100 *V) { return V->scroll_top; }

int vt100_scroll_bottom(const struct vt100 *V) { return V->scroll_bottom; }

int vt100_alt_screen(const struct vt100 *V) { return V->alt_screen; }

int vt100_scrolled_count(const struct vt100 *V) { return V->scrolled_count; }

const struct vt100_cell *vt100_scrolled_row(const struct vt100 *V, int i,
					    int *cols)
{
	if (i < 0 || i >= V->scrolled_count)
		return NULL;
	if (cols)
		*cols = V->scrolled[i].cols;
	return V->scrolled[i].cells;
}

void vt100_drain_scrolled(struct vt100 *V) { scrolled_clear(V); }

/* ------------------------------------------------------------------ */
/*  Cell row → ANSI byte string                                       */
/* ------------------------------------------------------------------ */

static int sgr_eq(const struct vt100_sgr *a, const struct vt100_sgr *b)
{
	return a->fg == b->fg && a->bg == b->bg && a->attrs == b->attrs;
}

static int sgr_is_default(const struct vt100_sgr *s)
{
	return s->fg == VT100_DEFAULT_COLOR && s->bg == VT100_DEFAULT_COLOR &&
	       s->attrs == 0;
}

/* Emit a "set from scratch" SGR sequence: \x1b[0;<attrs>;<fg>;<bg>m.
 * Always starting from reset keeps the encoder simple; the renderer
 * collapses adjacent matching SGRs at paint time anyway. */
static void sgr_emit_ansi(struct sbuf *out, const struct vt100_sgr *s)
{
	sbuf_addstr(out, "\x1b[0");
	if (s->attrs & VT100_ATTR_BOLD)
		sbuf_addstr(out, ";1");
	if (s->attrs & VT100_ATTR_UNDERLINE)
		sbuf_addstr(out, ";4");
	if (s->attrs & VT100_ATTR_REVERSE)
		sbuf_addstr(out, ";7");
	if (s->fg != VT100_DEFAULT_COLOR) {
		if (s->fg < 8)
			sbuf_addf(out, ";%d", 30 + s->fg);
		else if (s->fg < 16)
			sbuf_addf(out, ";%d", 90 + (s->fg - 8));
	}
	if (s->bg != VT100_DEFAULT_COLOR) {
		if (s->bg < 8)
			sbuf_addf(out, ";%d", 40 + s->bg);
		else if (s->bg < 16)
			sbuf_addf(out, ";%d", 100 + (s->bg - 8));
	}
	sbuf_addch(out, 'm');
}

void vt100_serialize_row(struct sbuf *out, const struct vt100_cell *cells,
			 int cols)
{
	int end = cols;

	while (end > 0 && cells[end - 1].cp == 0)
		end--;

	struct vt100_sgr cur = sgr_default();
	int dirty = 0;

	for (int c = 0; c < end; c++) {
		if (!sgr_eq(&cur, &cells[c].sgr)) {
			sgr_emit_ansi(out, &cells[c].sgr);
			cur = cells[c].sgr;
			dirty = !sgr_is_default(&cur);
		}
		uint32_t cp = cells[c].cp ? cells[c].cp : (uint32_t)' ';

		if (cp < 0x80)
			sbuf_addch(out, (int)cp);
		else
			sbuf_addch(out, '?');
	}
	if (dirty)
		sbuf_addstr(out, "\x1b[0m");
}
