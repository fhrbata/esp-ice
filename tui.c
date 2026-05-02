/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tui.c
 * @brief Scrollable list, modal prompt, info modal, and scrolling log
 *        pane widgets built on term_*.
 *
 * Render functions append into a caller-owned @c sbuf so multiple
 * widgets (log + help modal, list + prompt) can be composed into one
 * frame; @ref tui_flush wraps that frame in DEC mode 2026
 * (synchronized output) so the terminal applies it atomically and
 * background log churn doesn't cause overlays to flicker.
 */
#include "tui.h"
#include "ice.h"
#include "vt100.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

void tui_flush(struct sbuf *out)
{
	/*
	 * Wrap the frame in DEC private mode 2026 (synchronized output):
	 * the terminal buffers everything between BSU and ESU, then
	 * applies it as one atomic update.  A plain single @c fputs is
	 * not enough -- a multi-KB frame is split across multiple
	 * @c write(2) calls by stdio and across multiple terminal
	 * refresh cycles by the renderer, so the user can briefly see
	 * the log paint where the help modal is about to land before
	 * the modal repaints on top.  Mode 2026 closes that window.
	 *
	 * Modern terminals (kitty, alacritty, foot, wezterm, iTerm2,
	 * mintty, contour, vte, konsole) honour the sequence; older
	 * terminals ignore the unknown private mode -- worst case the
	 * residual flicker comes back, never anything visibly broken.
	 */
	fputs("\x1b[?2026h", stdout);
	fputs(out->buf, stdout);
	fputs("\x1b[?2026l", stdout);
	fflush(stdout);
	sbuf_release(out);
}

/* ================================================================== */
/*  Rectangles + layout helpers                                       */
/* ================================================================== */

void tui_rect_split_v(const struct tui_rect *parent, struct tui_rect *left,
		      struct tui_rect *right, int left_w)
{
	if (left_w < 0)
		left_w = 0;
	if (left_w > parent->w)
		left_w = parent->w;
	left->x = parent->x;
	left->y = parent->y;
	left->w = left_w;
	left->h = parent->h;
	right->x = parent->x + left_w;
	right->y = parent->y;
	right->w = parent->w - left_w;
	right->h = parent->h;
}

void tui_rect_split_h(const struct tui_rect *parent, struct tui_rect *top,
		      struct tui_rect *bottom, int top_h)
{
	if (top_h < 0)
		top_h = 0;
	if (top_h > parent->h)
		top_h = parent->h;
	top->x = parent->x;
	top->y = parent->y;
	top->w = parent->w;
	top->h = top_h;
	bottom->x = parent->x;
	bottom->y = parent->y + top_h;
	bottom->w = parent->w;
	bottom->h = parent->h - top_h;
}

struct tui_rect tui_rect_inset(struct tui_rect r, int top, int right,
			       int bottom, int left)
{
	int hcut = left + right;
	int vcut = top + bottom;
	r.x += left;
	r.y += top;
	r.w = r.w > hcut ? r.w - hcut : 0;
	r.h = r.h > vcut ? r.h - vcut : 0;
	return r;
}

/*
 * Active search state, lazily allocated when a pattern is set and
 * freed by @ref tui_log_search_clear.  Kept opaque in @ref tui.h so
 * consumers don't pull in the PCRE2 headers.
 *
 * @c current_line / @c current_start / @c current_end mark the match
 * the user is "on" right now (after the most recent set / next / prev
 * call).  The renderer paints that single match with a distinct SGR
 * so it stands out against the regular highlighted matches.  @c
 * current_line uses the global line counter so the marker survives
 * eviction the same way @c L->anchor does.
 */
struct tui_search {
	pcre2_code *re;
	pcre2_match_data *md;
	char *pattern; /**< Heap-duplicated raw pattern, for status display. */

	long long current_line; /**< Global idx of the current match's line;
				 *   -1 when no match is currently selected. */
	size_t current_start;	/**< Byte offset of the current match. */
	size_t current_end;	/**< Byte offset (exclusive) of the current. */

	/* Match counter.  Maintained incrementally by @ref log_push_line
	 * (scan new line on append, scan evicted line on eviction) so the
	 * status bar can show "i/N" without re-scanning the whole buffer
	 * every render.  Only completed lines are counted -- pending
	 * content gets highlighted but not indexed because the user can't
	 * navigate into a half-built line. */
	int total_matches;
	int current_index; /**< 1-based; 0 when no match is selected. */
};

/* ================================================================== */
/*  Scrollable list                                                   */
/* ================================================================== */

static int item_is_selectable(const struct tui_list_item *it)
{
	return !(it->flags & (TUI_ITEM_DISABLED | TUI_ITEM_HEADING));
}

/*
 * Scan from @p start in @p dir (+1 forward, -1 backward) for the
 * first selectable item.  Returns the found index, or the starting
 * index unchanged if nothing selectable lies that way.
 */
static int find_selectable(const struct tui_list *L, int start, int dir)
{
	int i = start;
	while (i >= 0 && i < L->n_items) {
		if (item_is_selectable(&L->items[i]))
			return i;
		i += dir;
	}
	return start;
}

/*
 * Move the cursor by @p delta, clamping to the list bounds and
 * skipping past non-selectable items in the direction of travel.
 * A PageUp / PageDown that lands on a heading-only region falls
 * back to the nearest selectable neighbour so the caret never
 * gets stuck on an inert row.
 */
static void move_cursor(struct tui_list *L, int delta)
{
	int dir = delta >= 0 ? 1 : -1;
	int target = L->cursor + delta;
	if (target < 0)
		target = 0;
	if (target >= L->n_items)
		target = L->n_items - 1;
	int found = find_selectable(L, target, dir);
	if (!item_is_selectable(&L->items[found]))
		found = find_selectable(L, target, -dir);
	L->cursor = found;
}

static int body_rows(const struct tui_list *L)
{
	int rows = L->height - 2; /* reserve title + footer rows */
	return rows > 0 ? rows : 0;
}

/*
 * After any cursor / size change, ensure the cursor is within the
 * visible band.  Keeps the scroll offset stable when the cursor
 * moves inside the visible range, scrolls by exactly enough to bring
 * it back into view otherwise.
 */
static void clamp_scroll(struct tui_list *L)
{
	int rows = body_rows(L);
	if (rows <= 0 || L->n_items <= 0) {
		L->top = 0;
		return;
	}
	if (L->top > L->n_items - rows)
		L->top = L->n_items - rows;
	if (L->top < 0)
		L->top = 0;
	if (L->cursor < L->top)
		L->top = L->cursor;
	else if (L->cursor >= L->top + rows)
		L->top = L->cursor - rows + 1;
}

void tui_list_init(struct tui_list *L)
{
	memset(L, 0, sizeof(*L));
	L->origin_x = 1;
	L->origin_y = 1;
}

void tui_list_set_origin(struct tui_list *L, int x, int y)
{
	L->origin_x = x;
	L->origin_y = y;
}

void tui_list_set_items(struct tui_list *L, const struct tui_list_item *items,
			int n)
{
	L->items = items;
	L->n_items = n;
	L->cursor = 0;
	L->top = 0;
	/* Land on the first selectable entry even if the list opens with
	 * a heading -- otherwise the cursor draws against an inert row
	 * and callers see a heading as the "current" item. */
	if (n > 0 && !item_is_selectable(&items[0]))
		L->cursor = find_selectable(L, 0, 1);
	clamp_scroll(L);
}

void tui_list_set_title(struct tui_list *L, const char *title)
{
	L->title = title;
}

void tui_list_set_footer(struct tui_list *L, const char *footer)
{
	L->footer = footer;
}

void tui_list_resize(struct tui_list *L, int width, int height)
{
	L->width = width;
	L->height = height;
	clamp_scroll(L);
}

int tui_list_on_event(struct tui_list *L, const struct term_event *ev)
{
	int rows = body_rows(L);
	int moved = 0;
	switch (ev->key) {
	case TK_UP:
		move_cursor(L, -1);
		moved = 1;
		break;
	case TK_DOWN:
		move_cursor(L, 1);
		moved = 1;
		break;
	case TK_PGUP:
		move_cursor(L, -rows);
		moved = 1;
		break;
	case TK_PGDN:
		move_cursor(L, rows);
		moved = 1;
		break;
	case TK_HOME:
		L->cursor = find_selectable(L, 0, 1);
		moved = 1;
		break;
	case TK_END:
		L->cursor = find_selectable(L, L->n_items - 1, -1);
		moved = 1;
		break;
	default:
		return 0;
	}
	if (moved)
		clamp_scroll(L);
	return 1;
}

const struct tui_list_item *tui_list_current(const struct tui_list *L)
{
	if (L->n_items <= 0)
		return NULL;
	return &L->items[L->cursor];
}

/*
 * Append @p s truncated (or padded with spaces) to exactly @p width
 * terminal columns.  ASCII-only -- byte count == column count --
 * which is what menuconfig prompts use.  A NULL @p s renders as the
 * empty string (all padding).
 */
static void sbuf_pad(struct sbuf *sb, const char *s, int width)
{
	int len = s ? (int)strlen(s) : 0;
	if (len > width)
		len = width;
	if (s)
		sbuf_add(sb, s, (size_t)len);
	for (int i = len; i < width; i++)
		sbuf_addch(sb, ' ');
}

/*
 * Row layout (1-based columns, W = L->width):
 *
 *   col 1..2     "> " for cursor, "  " otherwise   (prefix_w = 2)
 *   col 3..M     text, padded with spaces          (text_w)
 *   col M+1..W   " VALUE " or blank filler         (value block)
 *
 * Totals always add to exactly W so the reverse-video stripe on a
 * cursor row reaches the right edge without leaking the previous
 * frame's trailing text.
 */
static void render_item_row(struct sbuf *sb, const struct tui_list *L, int idx)
{
	int item_idx = L->top + idx;
	sbuf_addf(sb, "\x1b[%d;%dH", L->origin_y + 1 + idx, L->origin_x);
	if (item_idx >= L->n_items) {
		/* Blank filler so the previous frame's row is cleared. */
		for (int i = 0; i < L->width; i++)
			sbuf_addch(sb, ' ');
		return;
	}
	const struct tui_list_item *it = &L->items[item_idx];
	int is_cursor = item_idx == L->cursor;
	int is_heading = (it->flags & TUI_ITEM_HEADING) != 0;
	int is_disabled = (it->flags & TUI_ITEM_DISABLED) != 0;

	/*
	 * Row-level style.  Only one of reverse / bold / dim applies at a
	 * time -- cursor dominates (so the stripe reads clearly), then
	 * headings, then disabled.  Emitted as a single SGR sequence and
	 * reset at row end.
	 */
	const char *row_sgr = NULL;
	if (is_cursor)
		row_sgr = "\x1b[7m"; /* reverse video */
	else if (is_heading)
		row_sgr = "\x1b[1;33m"; /* bold yellow: "group label" */
	else if (is_disabled)
		row_sgr = "\x1b[2m"; /* dim */
	if (row_sgr)
		sbuf_addstr(sb, row_sgr);

	sbuf_addch(sb, is_cursor ? '>' : ' ');
	sbuf_addch(sb, ' ');

	int value_w = it->value ? (int)strlen(it->value) : 0;
	/*
	 * " VALUE " (surrounding spaces) takes value_w + 2 columns.  Text
	 * gets whatever's left after the 2-column prefix.  If the
	 * terminal is too narrow for both, drop the value so the text
	 * stays legible.
	 */
	int value_block_w = value_w > 0 ? value_w + 2 : 0;
	int text_w = L->width - 2 - value_block_w;
	if (text_w < 0) {
		/* Terminal too narrow for both text and value -- drop the
		 * value so the label stays legible.  value_block_w drops to
		 * zero implicitly via the value_w==0 path below. */
		text_w = L->width - 2;
		value_w = 0;
	}
	sbuf_pad(sb, it->text, text_w);
	if (value_w > 0) {
		sbuf_addch(sb, ' ');
		/*
		 * Apply per-item value_sgr only when the row-level style
		 * would not conflict with it.  On a cursor row the reverse-
		 * video already carries the whole line, so keep the value
		 * under that style.  Disabled rows also override (dimmed
		 * values read consistently with their dim label).
		 */
		int apply_value_sgr =
		    it->value_sgr && !is_cursor && !is_disabled;
		if (apply_value_sgr)
			sbuf_addf(sb, "\x1b[%sm", it->value_sgr);
		sbuf_add(sb, it->value, (size_t)value_w);
		if (apply_value_sgr) {
			/* Re-assert the row-level style (if any) after the
			 * item-level one.  An empty string-reset here would
			 * clear the reverse / dim row stripe entirely. */
			sbuf_addstr(sb, "\x1b[0m");
			if (row_sgr)
				sbuf_addstr(sb, row_sgr);
		}
		sbuf_addch(sb, ' ');
	}

	sbuf_addstr(sb, "\x1b[0m");
}

void tui_list_render(struct sbuf *out, const struct tui_list *L)
{
	/*
	 * Full repaint.  Every row rewrites its full width, so we don't
	 * need a screen clear -- which was the source of the visible
	 * flash users saw on each redraw.  Just hide the cursor for the
	 * duration (so ESC[r;cH moves don't flick a visible caret across
	 * the screen); a modal prompt layered on top re-enables it at
	 * its input position.
	 */
	sbuf_addstr(out, "\x1b[?25l");

	/* Title bar: bold white on blue (classic menuconfig palette).
	 * Includes a one-space left margin so the text doesn't crowd
	 * the terminal edge. */
	sbuf_addf(out, "\x1b[%d;%dH\x1b[1;37;44m", L->origin_y, L->origin_x);
	sbuf_addch(out, ' ');
	sbuf_pad(out, L->title, L->width - 1);
	sbuf_addstr(out, "\x1b[0m");

	/* Body rows. */
	int rows = body_rows(L);
	for (int i = 0; i < rows; i++)
		render_item_row(out, L, i);

	/* Footer: white on blue (no bold) -- matches the title palette
	 * but reads as a secondary band. */
	if (L->height >= 2) {
		sbuf_addf(out, "\x1b[%d;%dH\x1b[37;44m",
			  L->origin_y + L->height - 1, L->origin_x);
		sbuf_addch(out, ' ');
		sbuf_pad(out, L->footer, L->width - 1);
		sbuf_addstr(out, "\x1b[0m");
	}
}

/* ================================================================== */
/*  Modal input prompt                                                */
/* ================================================================== */

void tui_prompt_init(struct tui_prompt *P, const char *title,
		     const char *initial)
{
	memset(P, 0, sizeof(*P));
	P->origin_x = 1;
	P->origin_y = 1;
	P->title = title;
	if (initial) {
		int n = (int)strlen(initial);
		if (n > TUI_PROMPT_CAP)
			n = TUI_PROMPT_CAP;
		memcpy(P->buf, initial, (size_t)n);
		P->len = n;
		P->cursor = n;
	}
}

void tui_prompt_resize(struct tui_prompt *P, int width, int height)
{
	P->width = width;
	P->height = height;
}

void tui_prompt_set_origin(struct tui_prompt *P, int x, int y)
{
	P->origin_x = x;
	P->origin_y = y;
}

static void prompt_insert(struct tui_prompt *P, int c)
{
	if (P->len >= TUI_PROMPT_CAP)
		return;
	/* Shift tail right, drop byte in. */
	memmove(P->buf + P->cursor + 1, P->buf + P->cursor,
		(size_t)(P->len - P->cursor));
	P->buf[P->cursor++] = (char)c;
	P->len++;
	P->buf[P->len] = '\0';
}

static void prompt_backspace(struct tui_prompt *P)
{
	if (P->cursor <= 0)
		return;
	memmove(P->buf + P->cursor - 1, P->buf + P->cursor,
		(size_t)(P->len - P->cursor));
	P->cursor--;
	P->len--;
	P->buf[P->len] = '\0';
}

static void prompt_delete(struct tui_prompt *P)
{
	if (P->cursor >= P->len)
		return;
	memmove(P->buf + P->cursor, P->buf + P->cursor + 1,
		(size_t)(P->len - P->cursor - 1));
	P->len--;
	P->buf[P->len] = '\0';
}

int tui_prompt_on_event(struct tui_prompt *P, const struct term_event *ev)
{
	switch (ev->key) {
	case TK_ENTER:
		return 1;
	case TK_ESC:
		return -1;
	case TK_BACKSPACE:
	case 0x08: /* Ctrl-H / BS */
		prompt_backspace(P);
		return 0;
	case TK_DEL:
		prompt_delete(P);
		return 0;
	case TK_LEFT:
		if (P->cursor > 0)
			P->cursor--;
		return 0;
	case TK_RIGHT:
		if (P->cursor < P->len)
			P->cursor++;
		return 0;
	case TK_HOME:
		P->cursor = 0;
		return 0;
	case TK_END:
		P->cursor = P->len;
		return 0;
	default:
		/* Printable ASCII only -- menuconfig prompts don't accept
		 * control chars or high bytes.  This filters out F-keys
		 * (sentinel values > 0xff) and C0 controls. */
		if (ev->key >= 0x20 && ev->key <= 0x7e)
			prompt_insert(P, ev->key);
		return 0;
	}
}

/*
 * Box geometry: we render a single-line-border box of fixed height 5
 * (top, title, separator, input, bottom) at ~60% screen width, centred.
 * Minimum width 30 so prompts remain legible; if the terminal is
 * smaller than that we use whatever is available.
 */
#define PROMPT_BOX_ROWS 5

static int prompt_box_cols(const struct tui_prompt *P)
{
	int cols = P->width * 6 / 10;
	if (cols < 30)
		cols = P->width > 30 ? 30 : P->width - 2;
	if (cols > P->width - 2)
		cols = P->width - 2;
	return cols > 4 ? cols : 4;
}

void tui_prompt_render(struct sbuf *out, const struct tui_prompt *P)
{
	int cols = prompt_box_cols(P);
	/* Centre the box within the widget's @c width x @c height area
	 * and shift by the origin so it lands at the right place on the
	 * actual terminal grid. */
	int left = P->origin_x + (P->width - cols) / 2;
	int top = P->origin_y + (P->height - PROMPT_BOX_ROWS) / 2;
	int inner = cols - 2; /* between the two vertical borders */

	/*
	 * Hide the cursor for the duration of the frame.  Without this,
	 * each ESC[r;cH between box-draw commands briefly positions a
	 * visible cursor at the border cells, producing the "cursor jumps
	 * around the screen" flicker users see when the modal pops up.
	 * Re-enabled at the final cursor placement below.
	 */
	sbuf_addstr(out, "\x1b[?25l");

	/*
	 * Modal palette.  Cyan borders frame the dialog so it reads as
	 * "separate from the list behind it" without the heaviness of a
	 * filled background.  Bold title text carries the emphasis.
	 * Input area stays in the default colour so the cursor is
	 * clearly visible.  A dim hint line below the box advertises
	 * the key bindings.
	 */
#define MODAL_BORDER "\x1b[36m" /* cyan */
#define MODAL_TITLE "\x1b[0;1m" /* bold, default fg */
#define MODAL_INPUT "\x1b[0m"	/* default */
#define MODAL_HINT "\x1b[0;2m"	/* dim */
#define MODAL_RESET "\x1b[0m"

	/* Top border. */
	sbuf_addf(out, "\x1b[%d;%dH%s", top, left, MODAL_BORDER);
	sbuf_addstr(out, "\xe2\x94\x8c"); /* U+250C ┌ */
	for (int i = 0; i < inner; i++)
		sbuf_addstr(out, HL);
	sbuf_addstr(out, "\xe2\x94\x90"); /* U+2510 ┐ */

	/* Title row. */
	sbuf_addf(out, "\x1b[%d;%dH%s", top + 1, left, MODAL_BORDER);
	sbuf_addstr(out, VL);
	sbuf_addstr(out, MODAL_TITLE);
	sbuf_addch(out, ' ');
	sbuf_pad(out, P->title, inner - 1);
	sbuf_addstr(out, MODAL_BORDER);
	sbuf_addstr(out, VL);

	/* Separator. */
	sbuf_addf(out, "\x1b[%d;%dH%s", top + 2, left, MODAL_BORDER);
	sbuf_addstr(out, "\xe2\x94\x9c"); /* U+251C ├ */
	for (int i = 0; i < inner; i++)
		sbuf_addstr(out, HL);
	sbuf_addstr(out, "\xe2\x94\xa4"); /* U+2524 ┤ */

	/* Input row.  Scroll the visible window so the cursor stays
	 * inside the inner area even for long buffers. */
	sbuf_addf(out, "\x1b[%d;%dH%s", top + 3, left, MODAL_BORDER);
	sbuf_addstr(out, VL);
	sbuf_addstr(out, MODAL_INPUT);
	sbuf_addch(out, ' ');
	int view_w = inner - 1;
	int view_start = P->cursor > view_w - 1 ? P->cursor - (view_w - 1) : 0;
	int view_len = P->len - view_start;
	if (view_len > view_w)
		view_len = view_w;
	if (view_len > 0)
		sbuf_add(out, P->buf + view_start, (size_t)view_len);
	for (int i = view_len; i < view_w; i++)
		sbuf_addch(out, ' ');
	sbuf_addstr(out, MODAL_BORDER);
	sbuf_addstr(out, VL);

	/* Bottom border. */
	sbuf_addf(out, "\x1b[%d;%dH%s", top + 4, left, MODAL_BORDER);
	sbuf_addstr(out, "\xe2\x94\x94"); /* U+2514 └ */
	for (int i = 0; i < inner; i++)
		sbuf_addstr(out, HL);
	sbuf_addstr(out, "\xe2\x94\x98"); /* U+2518 ┘ */
	sbuf_addstr(out, MODAL_RESET);

	/*
	 * Dim hint line just below the box advertising the key bindings.
	 * Fits within the same horizontal footprint so the eye groups it
	 * with the dialog.  Rendered only when there's a row to spare
	 * (on extremely short terminals, skip).
	 */
	if (top + PROMPT_BOX_ROWS < P->origin_y + P->height) {
		const char *hint = "Enter accept  \xe2\x80\xa2  Esc cancel";
		sbuf_addf(out, "\x1b[%d;%dH%s", top + PROMPT_BOX_ROWS, left,
			  MODAL_HINT);
		/* Left-pad one space so the hint lines up with the box
		 * content rather than the border. */
		sbuf_addch(out, ' ');
		sbuf_pad(out, hint, cols - 1);
		sbuf_addstr(out, MODAL_RESET);
	}

	/* Put the cursor at the edit position.  Column: left border + 1
	 * gap + (cursor - view_start).  The +1 for 1-based columns is
	 * already in @c left. */
	sbuf_addf(out, "\x1b[%d;%dH", top + 3,
		  left + 2 + (P->cursor - view_start));
	/*
	 * Re-show the cursor at its final edit position.  The alt-screen
	 * enter hid it, and tui_list_render also hides it at the top of
	 * every frame, so this is the one place a visible cursor is
	 * wanted while the TUI is active.
	 */
	sbuf_addstr(out, "\x1b[?25h");

#undef MODAL_BORDER
#undef MODAL_TITLE
#undef MODAL_INPUT
#undef MODAL_HINT
#undef MODAL_RESET
}

/* ================================================================== */
/*  Read-only scrollable info box                                     */
/* ================================================================== */

/*
 * Split @p body into lines on @c \n and populate I->lines / line_lens
 * / n_lines.  Lines point into @p body without copying; callers keep
 * the buffer alive for the widget's lifetime.  Trailing @c \n on the
 * last line is fine -- it just yields an empty final line, rendered
 * as blank, which keeps paragraph spacing the caller wrote.
 */
static void info_split_lines(struct tui_info *I)
{
	size_t cap = 0;
	I->n_lines = 0;
	I->lines = NULL;
	I->line_lens = NULL;

	const char *p = I->body;
	while (*p || I->n_lines == 0) {
		const char *eol = strchr(p, '\n');
		int len = eol ? (int)(eol - p) : (int)strlen(p);
		ALLOC_GROW(I->lines, (size_t)I->n_lines + 1, cap);
		/* line_lens grows in lockstep with lines; cap is tracked
		 * once via the ALLOC_GROW above. */
		REALLOC_ARRAY(I->line_lens, cap);
		I->lines[I->n_lines] = p;
		I->line_lens[I->n_lines] = len;
		I->n_lines++;
		if (!eol)
			break;
		p = eol + 1;
	}
}

void tui_info_init(struct tui_info *I, const char *title, const char *body)
{
	memset(I, 0, sizeof(*I));
	I->origin_x = 1;
	I->origin_y = 1;
	I->title = title;
	I->body = body ? body : "";
	info_split_lines(I);
}

void tui_info_set_origin(struct tui_info *I, int x, int y)
{
	I->origin_x = x;
	I->origin_y = y;
}

void tui_info_release(struct tui_info *I)
{
	free(I->lines);
	free(I->line_lens);
	I->lines = NULL;
	I->line_lens = NULL;
	I->n_lines = 0;
}

void tui_info_resize(struct tui_info *I, int width, int height)
{
	I->width = width;
	I->height = height;
}

/*
 * Body rows visible inside the modal box, given the box geometry below
 * (top + title + separator + bottom = 4 rows of chrome; a 1-row hint
 * below the box is outside it and doesn't subtract).
 */
static int info_body_rows(const struct tui_info *I)
{
	/* Box takes ~85% of terminal height; reserve 4 rows of chrome. */
	int box_rows = I->height - (I->height / 8) - 1;
	if (box_rows < 7)
		box_rows = I->height > 7 ? I->height - 2 : I->height;
	int rows = box_rows - 4;
	return rows > 0 ? rows : 0;
}

static void info_clamp_scroll(struct tui_info *I)
{
	int rows = info_body_rows(I);
	if (I->top_line < 0)
		I->top_line = 0;
	if (I->n_lines > rows && I->top_line > I->n_lines - rows)
		I->top_line = I->n_lines - rows;
	if (I->n_lines <= rows)
		I->top_line = 0;
}

int tui_info_on_event(struct tui_info *I, const struct term_event *ev)
{
	int rows = info_body_rows(I);
	switch (ev->key) {
	case TK_ESC:
	case TK_ENTER:
	case 'q':
	case 'Q':
		return 1;
	case TK_UP:
		I->top_line--;
		info_clamp_scroll(I);
		return 0;
	case TK_DOWN:
		I->top_line++;
		info_clamp_scroll(I);
		return 0;
	case TK_PGUP:
		I->top_line -= rows;
		info_clamp_scroll(I);
		return 0;
	case TK_PGDN:
		I->top_line += rows;
		info_clamp_scroll(I);
		return 0;
	case TK_HOME:
		I->top_line = 0;
		return 0;
	case TK_END:
		I->top_line = I->n_lines;
		info_clamp_scroll(I);
		return 0;
	default:
		return 0;
	}
}

void tui_info_render(struct sbuf *out, const struct tui_info *I)
{
	/*
	 * Modal palette re-used from tui_prompt so help boxes look like
	 * peers of the input prompt.
	 */
#define INFO_BORDER "\x1b[36m"
#define INFO_TITLE "\x1b[0;1m"
#define INFO_BODY "\x1b[0m"
#define INFO_HINT "\x1b[0;2m"
#define INFO_RESET "\x1b[0m"

	/* Box geometry: 90% width, 85% height, centred, min 40x10. */
	int cols = I->width * 9 / 10;
	if (cols < 40)
		cols = I->width > 40 ? 40 : I->width - 2;
	if (cols > I->width - 2)
		cols = I->width - 2;
	int box_rows = I->height - (I->height / 8) - 1;
	if (box_rows < 10)
		box_rows = I->height > 10 ? 10 : I->height - 2;
	if (box_rows > I->height - 1)
		box_rows = I->height - 1;
	int left = I->origin_x + (I->width - cols) / 2;
	int top = I->origin_y + (I->height - box_rows) / 2;
	int inner = cols - 2;
	int body_rows = box_rows - 4;

	sbuf_addstr(out, "\x1b[?25l");

	/* Top border. */
	sbuf_addf(out, "\x1b[%d;%dH%s", top, left, INFO_BORDER);
	sbuf_addstr(out, "\xe2\x94\x8c");
	for (int i = 0; i < inner; i++)
		sbuf_addstr(out, HL);
	sbuf_addstr(out, "\xe2\x94\x90");

	/* Title row. */
	sbuf_addf(out, "\x1b[%d;%dH%s", top + 1, left, INFO_BORDER);
	sbuf_addstr(out, VL);
	sbuf_addstr(out, INFO_TITLE);
	sbuf_addch(out, ' ');
	sbuf_pad(out, I->title, inner - 1);
	sbuf_addstr(out, INFO_BORDER);
	sbuf_addstr(out, VL);

	/* Separator. */
	sbuf_addf(out, "\x1b[%d;%dH%s", top + 2, left, INFO_BORDER);
	sbuf_addstr(out, "\xe2\x94\x9c");
	for (int i = 0; i < inner; i++)
		sbuf_addstr(out, HL);
	sbuf_addstr(out, "\xe2\x94\xa4");

	/* Body rows. */
	for (int r = 0; r < body_rows; r++) {
		int line_idx = I->top_line + r;
		sbuf_addf(out, "\x1b[%d;%dH%s", top + 3 + r, left, INFO_BORDER);
		sbuf_addstr(out, VL);
		sbuf_addstr(out, INFO_BODY);
		sbuf_addch(out, ' ');
		if (line_idx < I->n_lines) {
			int len = I->line_lens[line_idx];
			if (len > inner - 1)
				len = inner - 1;
			sbuf_add(out, I->lines[line_idx], (size_t)len);
			for (int i = len; i < inner - 1; i++)
				sbuf_addch(out, ' ');
		} else {
			for (int i = 0; i < inner - 1; i++)
				sbuf_addch(out, ' ');
		}
		sbuf_addstr(out, INFO_BORDER);
		sbuf_addstr(out, VL);
	}

	/* Bottom border. */
	sbuf_addf(out, "\x1b[%d;%dH%s", top + box_rows - 1, left, INFO_BORDER);
	sbuf_addstr(out, "\xe2\x94\x94");
	for (int i = 0; i < inner; i++)
		sbuf_addstr(out, HL);
	sbuf_addstr(out, "\xe2\x94\x98");
	sbuf_addstr(out, INFO_RESET);

	/* Hint line below the box.  Scroll indicator when there's more
	 * content than fits. */
	if (top + box_rows < I->origin_y + I->height) {
		const char *more = "";
		if (I->n_lines > body_rows) {
			if (I->top_line == 0)
				more = "  \xe2\x96\xbc more below";
			else if (I->top_line + body_rows >= I->n_lines)
				more = "  \xe2\x96\xb2 more above";
			else
				more = "  \xe2\x96\xb2\xe2\x96\xbc more "
				       "above/below";
		}
		sbuf_addf(out, "\x1b[%d;%dH%s", top + box_rows, left,
			  INFO_HINT);
		sbuf_addch(out, ' ');
		struct sbuf hint = SBUF_INIT;
		sbuf_addstr(&hint,
			    "\xe2\x86\x91\xe2\x86\x93 scroll  \xe2\x80\xa2"
			    "  Esc / q / Enter close");
		sbuf_addstr(&hint, more);
		sbuf_pad(out, hint.buf, cols - 1);
		sbuf_release(&hint);
		sbuf_addstr(out, INFO_RESET);
	}

#undef INFO_BORDER
#undef INFO_TITLE
#undef INFO_BODY
#undef INFO_HINT
#undef INFO_RESET
}

/* ================================================================== */
/*  Scrolling log pane                                                */
/* ================================================================== */

void tui_log_init(struct tui_log *L, int max_lines)
{
	memset(L, 0, sizeof(*L));
	L->origin_x = 1;
	L->origin_y = 1;
	L->cap_lines = max_lines > 0 ? max_lines : 1;
	L->lines = calloc((size_t)L->cap_lines, sizeof(*L->lines));
	if (!L->lines)
		die_errno("calloc");
	sbuf_init(&L->pending);
	L->anchor = -1;	 /* live-tail by default */
	L->ceiling = -1; /* no snapshot */
}

void tui_log_set_origin(struct tui_log *L, int x, int y)
{
	L->origin_x = x;
	L->origin_y = y;
}

void tui_log_release(struct tui_log *L)
{
	if (L->lines) {
		for (int i = 0; i < L->n_lines; i++) {
			int idx = (L->start + i) % L->cap_lines;
			free(L->lines[idx]);
		}
		free(L->lines);
		L->lines = NULL;
	}
	L->n_lines = 0;
	L->start = 0;
	free(L->snapshot);
	L->snapshot = NULL;
	L->snapshot_rows = 0;
	L->snapshot_cols = 0;
	sbuf_release(&L->pending);
	tui_log_search_clear(L);
}

void tui_log_resize(struct tui_log *L, int width, int height)
{
	L->width = width;
	L->height = height;
}

void tui_log_freeze(struct tui_log *L)
{
	L->ceiling = L->total_pushed;

	/*
	 * Snapshot the live grid so inspect-mode scroll and search hit
	 * stable content while the chip keeps writing.  Snapshot rows
	 * occupy globals [ceiling, ceiling + snapshot_rows) -- past the
	 * ring, but within the same monotonic counter so the existing
	 * global ↔ user mapping keeps working.
	 */
	free(L->snapshot);
	L->snapshot = NULL;
	L->snapshot_rows = 0;
	L->snapshot_cols = 0;
	if (L->vt100) {
		int rows = vt100_rows(L->vt100);
		int cols = vt100_cols(L->vt100);

		if (rows > 0 && cols > 0) {
			L->snapshot =
			    malloc((size_t)rows * cols * sizeof(*L->snapshot));
			if (!L->snapshot)
				die_errno("malloc");
			for (int r = 0; r < rows; r++) {
				const struct vt100_cell *src =
				    vt100_cell(L->vt100, r, 0);
				memcpy(&L->snapshot[(size_t)r * cols], src,
				       (size_t)cols *
					   sizeof(struct vt100_cell));
			}
			L->snapshot_rows = rows;
			L->snapshot_cols = cols;
		}
	}

	/*
	 * Anchor at the bottom of frozen content -- the last snapshot
	 * row when a grid was attached, otherwise the last ring line.
	 * If there's nothing to anchor on, live-tail (-1) is the safe
	 * default and log_bottom_idx returns total - 1 anyway.
	 */
	if (L->snapshot)
		L->anchor = L->ceiling + L->snapshot_rows - 1;
	else if (L->ceiling > 0)
		L->anchor = L->ceiling - 1;
	else
		L->anchor = -1;
}

void tui_log_unfreeze(struct tui_log *L)
{
	L->ceiling = -1;
	L->anchor = -1;
	free(L->snapshot);
	L->snapshot = NULL;
	L->snapshot_rows = 0;
	L->snapshot_cols = 0;
}

int tui_log_is_frozen(const struct tui_log *L) { return L->ceiling >= 0; }

void tui_log_set_status(struct tui_log *L, const char *status)
{
	L->status = status;
}

void tui_log_set_footer(struct tui_log *L, const char *footer)
{
	L->footer = footer;
}

void tui_log_set_grid(struct tui_log *L, struct vt100 *V) { L->vt100 = V; }

void tui_log_set_decorator(struct tui_log *L, tui_log_decorate_fn fn, void *ctx)
{
	L->decorate_fn = fn;
	L->decorate_ctx = ctx;
}

/*
 * Number of matches the current pattern produces against @p line.
 * Zero-width hits step one byte forward to avoid an infinite loop --
 * matches the convention @ref tui_log_render uses for highlight
 * iteration so the status counter and the rendered overlays agree on
 * what counts as "a match."
 */
static int count_line_matches(struct tui_search *sr, const char *line,
			      size_t len)
{
	if (!sr || !sr->re)
		return 0;
	int count = 0;
	size_t off = 0;
	while (off <= len) {
		int rc = pcre2_match(sr->re, (PCRE2_SPTR)line, len, off, 0,
				     sr->md, NULL);
		if (rc < 0)
			break;
		PCRE2_SIZE *vec = pcre2_get_ovector_pointer(sr->md);
		size_t ms = (size_t)vec[0];
		size_t me = (size_t)vec[1];
		count++;
		off = me > ms ? me : ms + 1;
	}
	return count;
}

/*
 * Push a NUL-free byte range as one completed line into the ring.  A
 * trailing @c '\r' is stripped so CRLF input renders as plain lines.
 * When the ring is full the oldest line is evicted to make room.
 *
 * Bumps @c total_pushed unconditionally; the global counter pairs with
 * @c anchor so frozen views stay pinned to the right content even when
 * eviction has shifted the user-visible indexing.  When a search is
 * active, the match counter is updated incrementally here -- evicted
 * matches are subtracted before the slot is overwritten, new matches
 * in @p s are added afterwards.  Doing this in @ref log_push_line
 * keeps the bookkeeping co-located with the only place lines actually
 * appear or disappear.
 */
static void log_push_line(struct tui_log *L, const char *s, size_t len)
{
	if (len > 0 && s[len - 1] == '\r')
		len--;
	char *line = malloc(len + 1);
	if (!line)
		die_errno("malloc");
	memcpy(line, s, len);
	line[len] = '\0';

	int slot;
	int evicted = 0;
	long long evicted_global = -1;
	int evicted_match_count = 0;
	if (L->n_lines < L->cap_lines) {
		slot = (L->start + L->n_lines) % L->cap_lines;
		L->n_lines++;
	} else {
		slot = L->start;
		evicted = 1;
		evicted_global = L->total_pushed - L->n_lines;
		if (L->search && L->search->re) {
			const char *old = L->lines[slot];
			evicted_match_count =
			    count_line_matches(L->search, old, strlen(old));
		}
		free(L->lines[slot]);
		L->start = (L->start + 1) % L->cap_lines;
	}
	L->lines[slot] = line;
	L->total_pushed++;

	if (L->search && L->search->re) {
		if (evicted) {
			L->search->total_matches -= evicted_match_count;
			if (L->search->current_line >= 0 &&
			    L->search->current_line == evicted_global) {
				/* The match the user was sitting on just
				 * fell off the bottom of the ring; clear
				 * the cursor so the next n/N starts fresh
				 * instead of pointing at deleted bytes. */
				L->search->current_line = -1;
				L->search->current_index = 0;
			} else if (L->search->current_index > 0 &&
				   evicted_match_count > 0) {
				/* All evicted matches were strictly older
				 * than the current one, so they used to
				 * occupy positions 1..k of the ordering;
				 * the current's index drops by exactly k. */
				L->search->current_index -= evicted_match_count;
				if (L->search->current_index < 0)
					L->search->current_index = 0;
			}
		}
		/* Lines pushed past the snapshot ceiling don't count toward
		 * the visible match total -- the user is inspecting a frozen
		 * view; matches arriving in the meantime sit invisibly in
		 * the ring until the next freeze. */
		long long new_global = L->total_pushed - 1;
		if (L->ceiling < 0 || new_global < L->ceiling)
			L->search->total_matches +=
			    count_line_matches(L->search, line, len);
	}
}

void tui_log_append(struct tui_log *L, const void *data, size_t n)
{
	const char *p = data;
	const char *end = p + n;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		if (!nl) {
			sbuf_add(&L->pending, p, (size_t)(end - p));
			return;
		}
		sbuf_add(&L->pending, p, (size_t)(nl - p));
		log_push_line(L, L->pending.buf, L->pending.len);
		sbuf_reset(&L->pending);
		p = nl + 1;
	}
}

void tui_log_pull_from_vt100(struct tui_log *L, struct vt100 *V)
{
	int n = vt100_scrolled_count(V);

	if (n == 0)
		return;

	struct sbuf row = SBUF_INIT;

	for (int i = 0; i < n; i++) {
		int cols;
		const struct vt100_cell *cells =
		    vt100_scrolled_row(V, i, &cols);

		sbuf_reset(&row);
		vt100_serialize_row(&row, cells, cols);
		log_push_line(L, row.buf, row.len);
	}
	sbuf_release(&row);
	vt100_drain_scrolled(V);
}

/*
 * Skip past one ANSI CSI sequence that starts at @p s[*i].  The caller
 * has already verified @c s[*i]==ESC and (when at least one trailing
 * byte is available) @c s[*i+1]=='['.  Advances @p *i past the final
 * byte of the sequence (any byte in 0x40..0x7e), or past whatever ESC-
 * prefixed pair is present when the second byte is not @c '['.
 *
 * Returns the byte range to emit verbatim via @p *out / @p *out_len.
 */
static void log_skip_csi(const char *s, size_t len, size_t *i, const char **out,
			 size_t *out_len)
{
	size_t start = *i;
	if (start + 1 < len && s[start + 1] == '[') {
		size_t j = start + 2;
		while (j < len) {
			unsigned char cj = (unsigned char)s[j];
			j++;
			if (cj >= 0x40 && cj <= 0x7e)
				break;
		}
		*out = &s[start];
		*out_len = j - start;
		*i = j;
	} else {
		/* Bare ESC, or ESC + a non-CSI letter -- emit ESC and the
		 * following byte (if present) verbatim, with no column cost. */
		size_t span = (start + 1 < len) ? 2 : 1;
		*out = &s[start];
		*out_len = span;
		*i = start + span;
	}
}

/*
 * Number of visual rows that @p line occupies at width @p width when
 * hard-wrapped on column overflow.  ANSI CSI sequences and UTF-8
 * continuation bytes do not count toward visible columns.  Returns at
 * least 1 -- an empty line still fills one row.
 */
static int log_visual_rows(const char *line, size_t len, int width)
{
	if (width <= 0)
		return 1;
	int col = 0;
	int rows = 1;
	size_t i = 0;
	while (i < len) {
		unsigned char c = (unsigned char)line[i];
		if (c == 0x1b) {
			const char *unused;
			size_t unused_len;
			log_skip_csi(line, len, &i, &unused, &unused_len);
			continue;
		}
		if (c == '\r' || c == '\b' || c == 0x7f) {
			i++;
			continue;
		}
		if ((c & 0xc0) == 0x80) {
			/* UTF-8 continuation: emitted but no column cost. */
			i++;
			continue;
		}
		if (col == width) {
			rows++;
			col = 0;
		}
		col++;
		i++;
	}
	return rows;
}

/*
 * Render @p line as up to @p max_rows visual rows starting at terminal
 * row @p term_row_start.  The first @p skip_rows visual rows of the
 * line are not drawn (caller uses this when only the tail of a long
 * line is meant to be visible).  Each emitted row positions the cursor
 * at column 1, writes its bytes (CSI sequences passed through), then
 * resets SGR and clears to end-of-line so the previous frame's content
 * is wiped without leaving stale colour state.
 *
 * @p overlays / @p n_overlays describe per-byte-range SGR decoration
 * applied on top of the raw line bytes.  Opens fire immediately
 * before the first visible byte at @c overlays[k].start; closes fire
 * immediately after the last byte at @c overlays[k].end-1.  Overlay
 * events fire only on non-CSI bytes, so target-side ANSI sequences
 * pass through untouched and overlay starts that land inside a CSI
 * are deferred to the next plain byte.  When a hard wrap mid-overlay
 * forces a per-row SGR reset, the renderer re-issues the active
 * overlay's @c sgr_open at the new row's start so the colour stripe
 * is unbroken across visual rows.
 *
 * Returns the number of visual rows actually emitted.
 */
/*
 * Sort overlays by start ascending; on ties, larger end first so the
 * outer overlay opens before any nested overlay sharing its start --
 * matches the LIFO close discipline @ref render_log_line relies on.
 */
static int overlay_cmp(const void *a, const void *b)
{
	const struct tui_overlay *oa = a;
	const struct tui_overlay *ob = b;
	if (oa->start != ob->start)
		return (oa->start < ob->start) ? -1 : 1;
	if (oa->end != ob->end)
		return (oa->end > ob->end) ? -1 : 1;
	return 0;
}

/* Hard cap on simultaneously-active overlays at any byte offset.
 * Realistic peak is small: outer level overlay + one or two nested
 * highlights from search / keyword rules.  Hitting the cap drops the
 * excess silently rather than crashing, which keeps malformed input
 * from taking the renderer down. */
#define RENDER_OVERLAY_STACK 16

static int render_log_line(struct sbuf *sb, const char *line, size_t len,
			   int width, int origin_col, int term_row_start,
			   int skip_rows, int max_rows,
			   const struct tui_overlay *overlays, int n_overlays)
{
	if (max_rows <= 0 || width <= 0)
		return 0;

	int col = 0;
	int vrow = 0;
	int emitted = 0;
	/*
	 * @c ov_next advances past every overlay whose start has fired.
	 * @c active is a LIFO stack of indices into @p overlays whose
	 * opens have fired but whose closes have not -- the topmost
	 * entry is the innermost overlay.  When two nested overlays are
	 * both active, the outer one's @c sgr_open was emitted first and
	 * the inner one's second; closing in stack order re-asserts the
	 * outer style after the inner ends.  Wrap path re-issues every
	 * active overlay's @c sgr_open in stack order so all enclosing
	 * styles carry over to the next visual row.
	 */
	int ov_next = 0;
	int active[RENDER_OVERLAY_STACK];
	int n_active = 0;

	if (skip_rows == 0)
		sbuf_addf(sb, "\x1b[%d;%dH", term_row_start, origin_col);

	size_t i = 0;
	while (i < len && emitted < max_rows) {
		unsigned char c = (unsigned char)line[i];
		const char *emit_ptr = NULL;
		size_t emit_len = 0;
		int col_advance = 0;
		int is_csi = 0;
		size_t offset = i; /* offset of the byte we're about to
				    * process; captured before the per-
				    * byte branches advance @c i. */

		if (c == 0x1b) {
			is_csi = 1;
			log_skip_csi(line, len, &i, &emit_ptr, &emit_len);
		} else if (c == '\r' || c == '\b' || c == 0x7f) {
			i++;
			continue;
		} else if ((c & 0xc0) == 0x80) {
			emit_ptr = &line[i];
			emit_len = 1;
			i++;
		} else {
			emit_ptr = &line[i];
			emit_len = 1;
			col_advance = 1;
			i++;
		}

		if (col_advance && col == width) {
			/* Close the row we just filled and step to the next.
			 * No erase needed -- col == width means we filled
			 * every cell of the pane width exactly. */
			if (vrow >= skip_rows) {
				sbuf_addstr(sb, "\x1b[0m");
				emitted++;
				if (emitted >= max_rows)
					return emitted;
			}
			vrow++;
			col = 0;
			if (vrow >= skip_rows) {
				sbuf_addf(sb, "\x1b[%d;%dH",
					  term_row_start + emitted, origin_col);
				/* Per-row reset wiped SGR state.  Re-issue
				 * every active overlay's open so all
				 * enclosing styles carry over -- order
				 * matches the original opening order, so
				 * inner overlays re-layer on top of outer
				 * ones the same way they did initially. */
				for (int a = 0; a < n_active; a++)
					sbuf_addf(sb, "\x1b[%sm",
						  overlays[active[a]].sgr_open);
			}
		}

		/* Fire overlay opens that land at or before this offset.
		 * Skipped during CSI passthrough: CSI segments have no
		 * caller-meaningful byte offsets, and an overlay whose
		 * start sits inside a CSI is deferred to the next plain
		 * byte (which is the first visible byte it can actually
		 * decorate). */
		if (!is_csi) {
			while (ov_next < n_overlays &&
			       overlays[ov_next].start <= offset) {
				if (n_active < RENDER_OVERLAY_STACK) {
					active[n_active++] = ov_next;
					if (vrow >= skip_rows)
						sbuf_addf(
						    sb, "\x1b[%sm",
						    overlays[ov_next].sgr_open);
				}
				ov_next++;
			}
		}

		if (vrow >= skip_rows && emit_ptr)
			sbuf_add(sb, emit_ptr, emit_len);
		col += col_advance;

		/* Fire overlay closes in LIFO order: as long as the
		 * topmost (innermost) overlay's @c end has been reached,
		 * pop it.  After each close, re-issue every still-active
		 * overlay's @c sgr_open so even a destructive close (e.g.
		 * @c "0" full reset) doesn't strip outer styles. */
		if (!is_csi) {
			while (n_active > 0 &&
			       overlays[active[n_active - 1]].end <=
				   offset + 1) {
				int idx = active[--n_active];
				if (vrow >= skip_rows) {
					sbuf_addf(sb, "\x1b[%sm",
						  overlays[idx].sgr_close);
					for (int a = 0; a < n_active; a++)
						sbuf_addf(sb, "\x1b[%sm",
							  overlays[active[a]]
							      .sgr_open);
				}
			}
		}
	}

	/* Close the final partial row, if we drew into it at all.  The
	 * \x1b[0m fully resets SGR so any still-active overlay is
	 * implicitly cleared -- callers that need clean SGR state at
	 * end-of-line get it for free.  Erase the unfilled cells with
	 * ECH (\x1b[<n>X) so neighbouring panes' content stays intact;
	 * \x1b[K would clear past the pane's right edge. */
	if (vrow >= skip_rows && emitted < max_rows) {
		int remaining = width - col;
		sbuf_addstr(sb, "\x1b[0m");
		if (remaining > 0)
			sbuf_addf(sb, "\x1b[%dX", remaining);
		emitted++;
	}
	return emitted;
}

/*
 * Look up the user-visible line at logical index @p idx.  0 is the
 * oldest surviving line, @c L->n_lines is the (sole) pending line if
 * any.  The caller must have already bounded @p idx.
 */
static int log_effective_n_lines(const struct tui_log *L);

/*
 * Scratch buffer for serializing grid / snapshot rows on demand inside
 * @ref log_get_line.  File-static so callers don't need to thread an
 * sbuf parameter through every helper; safe because tui is single-
 * threaded and every caller processes the returned pointer (or copies
 * what it needs) before the next @ref log_get_line call.
 */
static struct sbuf log_grid_scratch = SBUF_INIT;

static void log_get_line(const struct tui_log *L, int idx, const char **out,
			 size_t *out_len)
{
	int n_eff = log_effective_n_lines(L);

	if (idx < n_eff) {
		const char *s = L->lines[(L->start + idx) % L->cap_lines];

		*out = s;
		*out_len = strlen(s);
		return;
	}

	int grid_idx = idx - n_eff;

	/*
	 * Frozen + has snapshot: stable cell array taken at freeze time.
	 * Searches and scroll inside inspect see consistent content even
	 * as the chip keeps emitting bytes into the live grid.
	 */
	if (L->ceiling >= 0 && L->snapshot && grid_idx >= 0 &&
	    grid_idx < L->snapshot_rows) {
		sbuf_reset(&log_grid_scratch);
		vt100_serialize_row(
		    &log_grid_scratch,
		    &L->snapshot[(size_t)grid_idx * L->snapshot_cols],
		    L->snapshot_cols);
		*out = log_grid_scratch.buf;
		*out_len = log_grid_scratch.len;
		return;
	}

	/*
	 * Not frozen + grid attached: serialize the live grid row.  The
	 * pointer is valid until the next log_get_line call.
	 */
	if (L->ceiling < 0 && L->vt100 && grid_idx >= 0 &&
	    grid_idx < vt100_rows(L->vt100)) {
		sbuf_reset(&log_grid_scratch);
		vt100_serialize_row(&log_grid_scratch,
				    vt100_cell(L->vt100, grid_idx, 0),
				    vt100_cols(L->vt100));
		*out = log_grid_scratch.buf;
		*out_len = log_grid_scratch.len;
		return;
	}

	/*
	 * Pending in-progress line, only when no grid is attached -- the
	 * grid IS the in-progress edit when monitor.c is wired up.
	 */
	if (L->ceiling < 0 && !L->vt100 && L->pending.len > 0 &&
	    grid_idx == 0) {
		*out = L->pending.buf;
		*out_len = L->pending.len;
		return;
	}

	*out = "";
	*out_len = 0;
}

/*
 * Number of completed lines currently visible to the user.  When a
 * snapshot ceiling is set the count is clipped: only lines whose
 * @c global_idx is strictly less than @c L->ceiling appear.  Lines
 * that fell within the ceiling but have since been evicted from the
 * ring are simply gone -- the ceiling clips, it doesn't pin them.
 */
static int log_effective_n_lines(const struct tui_log *L)
{
	if (L->ceiling < 0)
		return L->n_lines;
	long long oldest = L->total_pushed - L->n_lines;
	long long visible = L->ceiling - oldest;
	if (visible < 0)
		return 0;
	if (visible > L->n_lines)
		return L->n_lines;
	return (int)visible;
}

/*
 * Total number of user-visible logical lines.  Includes the live
 * grid when attached (so inspect-mode scroll and search reach grid
 * content) and the snapshot when frozen.  Pending only counts when
 * no grid is attached and no ceiling is set -- a frozen view
 * excludes the half-built line because its bytes change under the
 * user, and the grid takes over the in-progress role when wired.
 */
static int log_total(const struct tui_log *L)
{
	int n = log_effective_n_lines(L);

	if (L->ceiling >= 0 && L->snapshot)
		n += L->snapshot_rows;
	else if (L->ceiling < 0 && L->vt100)
		n += vt100_rows(L->vt100);
	else if (L->ceiling < 0 && L->pending.len > 0)
		n++;
	return n;
}

/*
 * Walk the logical lines from @p bottom_idx backward and pick the
 * contiguous tail whose visual rows (computed at width @p body_w) just
 * fill @p body_rows.  Sets @p *first_line to the logical index of the
 * topmost line drawn, and @p *first_skip to the number of leading
 * visual rows to skip on that line (>0 when it spills above the body).
 *
 * Returns the number of body rows that remain blank above the first
 * line when history is shorter than @p body_rows; otherwise 0.
 */
static int log_pick_window_at(const struct tui_log *L, int body_rows,
			      int body_w, int bottom_idx, int *first_line,
			      int *first_skip)
{
	int rows_remaining = body_rows;
	*first_line = bottom_idx + 1; /* exclusive marker */
	*first_skip = 0;

	if (bottom_idx < 0 || body_rows <= 0)
		return body_rows > 0 ? body_rows : 0;

	for (int i = bottom_idx; i >= 0 && rows_remaining > 0; i--) {
		const char *s;
		size_t slen;
		log_get_line(L, i, &s, &slen);
		int vr = log_visual_rows(s, slen, body_w);
		if (vr >= rows_remaining) {
			*first_line = i;
			*first_skip = vr - rows_remaining;
			rows_remaining = 0;
			break;
		}
		*first_line = i;
		rows_remaining -= vr;
	}
	return rows_remaining;
}

/*
 * Body rows match render's geometry: row 1 holds the status when set,
 * row @c height holds the footer when set, the rest belongs to the
 * scrolling log body.  @ref tui_log_on_event uses this to size paged
 * scrolls (PgUp / PgDn move by exactly one body height so the next
 * page abuts cleanly with the previous one).
 */
static int log_body_rows(const struct tui_log *L)
{
	int body_top = (L->status && L->height >= 1) ? 2 : 1;
	int body_bottom = L->height;
	if (L->footer && L->height >= body_top + 1)
		body_bottom = L->height - 1;
	int rows = body_bottom - body_top + 1;
	return rows > 0 ? rows : 0;
}

/*
 * Map a global line index to the current user-visible index.  Returns
 * a value < 0 when the line has been evicted (caller should clamp to
 * the oldest surviving line) and a value >= n_lines when the index
 * sits past the newest pushed line (caller should snap to live-tail).
 */
static long long log_global_to_user(const struct tui_log *L, long long g)
{
	long long oldest = L->total_pushed - L->n_lines;
	return g - oldest;
}

/*
 * Inverse of @ref log_global_to_user.  Defined alongside its peer so
 * both directions are obvious; the search and render paths both need
 * the global<->user mapping.
 */
static long long log_user_to_global(const struct tui_log *L, int u)
{
	return (L->total_pushed - L->n_lines) + u;
}

/*
 * Resolve @c L->anchor (or the live-tail sentinel -1) to the bottom-
 * most user-visible logical index that should appear in the body.  In
 * tail mode: pending if any, else the newest completed line.  In
 * frozen mode: the anchored line, clamped to the surviving history.
 */
static int log_bottom_idx(const struct tui_log *L)
{
	int total = log_total(L);
	if (total == 0)
		return -1;
	if (L->anchor < 0)
		return total - 1;
	long long u = log_global_to_user(L, L->anchor);
	if (u < 0)
		u = 0;
	if (u >= total)
		u = total - 1;
	return (int)u;
}

int tui_log_is_tailing(const struct tui_log *L) { return L->anchor < 0; }

int tui_log_on_event(struct tui_log *L, const struct term_event *ev)
{
	int total = log_total(L);
	if (total == 0)
		return 0;
	int body_rows = log_body_rows(L);
	if (body_rows < 1)
		body_rows = 1;

	long long oldest = L->total_pushed - L->n_lines;
	/*
	 * "newest_completed" is the upper-bound global index for
	 * navigation.  Includes the snapshot when frozen and the live
	 * grid otherwise -- both occupy globals past total_pushed (the
	 * snapshot at [ceiling, ceiling+snapshot_rows) and the live
	 * grid at [total_pushed, total_pushed+grid_rows)) so the
	 * existing arithmetic walks them as virtual lines.
	 */
	long long newest_completed;
	if (L->ceiling >= 0 && L->snapshot)
		newest_completed = L->ceiling + L->snapshot_rows - 1;
	else if (L->ceiling >= 0)
		newest_completed = L->ceiling > 0 ? L->ceiling - 1 : -1;
	else if (L->vt100)
		newest_completed = L->total_pushed + vt100_rows(L->vt100) - 1;
	else
		newest_completed =
		    L->total_pushed > 0 ? L->total_pushed - 1 : -1;

	long long current;
	if (L->anchor < 0) {
		current = (!L->vt100 && L->ceiling < 0 && L->pending.len > 0)
			      ? L->total_pushed
			      : newest_completed;
	} else {
		current = L->anchor;
	}

	switch (ev->key) {
	case TK_UP: {
		long long target = current - 1;
		if (target < oldest)
			target = oldest;
		L->anchor = target;
		return 1;
	}
	case TK_DOWN: {
		long long target = current + 1;
		if (target > newest_completed)
			L->anchor = -1; /* snap back to live-tail */
		else
			L->anchor = target;
		return 1;
	}
	case TK_PGUP: {
		long long target = current - body_rows;
		if (target < oldest)
			target = oldest;
		L->anchor = target;
		return 1;
	}
	case TK_PGDN: {
		long long target = current + body_rows;
		if (target > newest_completed)
			L->anchor = -1; /* snap back to live-tail */
		else
			L->anchor = target;
		return 1;
	}
	case TK_HOME:
		L->anchor = oldest;
		return 1;
	case TK_END:
		L->anchor = -1;
		return 1;
	default:
		return 0;
	}
}

/*
 * Render the scrollbar track + thumb in the rightmost column of the
 * body region.  The thumb height/position approximate the visible
 * window's slice of the total scrollable buffer; we measure in logical
 * lines (cheaper than re-walking visual rows for thousands of lines)
 * which is precise enough for a one-column indicator.  When everything
 * fits on screen the bar shows a full thumb so callers always have a
 * visible "you are here" cue.
 */
static void render_scrollbar(struct sbuf *sb, const struct tui_log *L,
			     int body_top, int body_rows, int bottom_idx)
{
	if (body_rows <= 0 || L->width <= 0)
		return;

	int total = log_total(L);
	int sbar_col = L->origin_x + L->width - 1;

	int top_idx = bottom_idx - body_rows + 1;
	if (top_idx < 0)
		top_idx = 0;
	if (total <= 0)
		total = 1;

	int thumb_top = (int)((long long)top_idx * body_rows / total);
	int thumb_h =
	    (int)(((long long)body_rows * body_rows + total - 1) / total);
	if (thumb_h < 1)
		thumb_h = 1;
	if (thumb_h > body_rows)
		thumb_h = body_rows;
	if (thumb_top < 0)
		thumb_top = 0;
	if (thumb_top + thumb_h > body_rows)
		thumb_top = body_rows - thumb_h;

	for (int r = 0; r < body_rows; r++) {
		int term_row = body_top + r;
		int is_thumb = r >= thumb_top && r < thumb_top + thumb_h;
		sbuf_addf(sb, "\x1b[%d;%dH", term_row, sbar_col);
		if (is_thumb) {
			/* Heavy block in default fg -- reads as a clear
			 * "you are here" stripe against the dim track. */
			sbuf_addstr(sb, "\x1b[0m\xe2\x96\x88\x1b[0m");
		} else {
			/* Light vertical, dim, for the track. */
			sbuf_addstr(sb, "\x1b[2m" VL "\x1b[0m");
		}
	}
}

void tui_log_render(struct sbuf *out, const struct tui_log *L)
{
	sbuf_addstr(out, "\x1b[?25l");

	/* All row indices below are 1-based screen rows: starts at the
	 * widget's @c origin_y, ends one row before @c origin_y + height. */
	int body_top = L->origin_y;
	int body_bottom = L->origin_y + L->height - 1;

	/* Status bar at the top row of the widget (matches tui_list
	 * title palette). */
	if (L->status && L->height >= 1) {
		sbuf_addf(out, "\x1b[%d;%dH\x1b[1;37;44m", L->origin_y,
			  L->origin_x);
		sbuf_addch(out, ' ');
		sbuf_pad(out, L->status, L->width - 1);
		sbuf_addstr(out, "\x1b[0m");
		body_top = L->origin_y + 1;
	}

	/* Footer at the last row of the widget (matches tui_list footer
	 * palette).  Reserve only when we have at least one body row left
	 * for it. */
	if (L->footer && L->height >= (body_top - L->origin_y) + 2) {
		body_bottom = L->origin_y + L->height - 2;
	}

	int body_rows = body_bottom - body_top + 1;

	if (body_rows < 0)
		body_rows = 0;

	/* Reserve the rightmost column for the scrollbar.  Using a fixed
	 * reservation keeps wrap geometry stable: the bar is always
	 * visible (track when there's nothing to scroll, thumb when there
	 * is) so content never reflows when the buffer first overflows
	 * the body. */
	int body_w = L->width > 1 ? L->width - 1 : L->width;

	int bottom_idx = log_bottom_idx(L);

	/*
	 * Cursor tracking: when tailing with a grid attached and the
	 * chip has the cursor visible, find which user-idx the cursor
	 * lives on (n_eff + cur_row in the unified addressable space)
	 * and remember the terminal row when we render that line.  The
	 * cursor positioning emit at the bottom of this function then
	 * just looks up the saved row.
	 */
	int cursor_user_idx = -1;
	int cursor_term_row = -1;
	int cursor_term_col = 0;

	if (L->vt100 && tui_log_is_tailing(L) &&
	    vt100_cursor_visible(L->vt100)) {
		cursor_user_idx =
		    log_effective_n_lines(L) + vt100_cursor_row(L->vt100);
		cursor_term_col = L->origin_x + vt100_cursor_col(L->vt100);
	}

	if (body_rows > 0 && body_w > 0 && bottom_idx >= 0) {
		int first_line, first_skip;
		int leading_blanks = log_pick_window_at(
		    L, body_rows, body_w, bottom_idx, &first_line, &first_skip);

		/* Blank rows above the first rendered line (history shorter
		 * than the body, or scroll has cleared past the top). */
		for (int r = 0; r < leading_blanks; r++)
			sbuf_addf(out, "\x1b[%d;%dH\x1b[0m\x1b[%dX",
				  body_top + r, L->origin_x, body_w);

		int term_row = body_top + leading_blanks;
		for (int i = first_line;
		     i <= bottom_idx && term_row <= body_bottom; i++) {
			if (i == cursor_user_idx)
				cursor_term_row = term_row;
			const char *s;
			size_t slen;
			log_get_line(L, i, &s, &slen);
			int skip = (i == first_line) ? first_skip : 0;
			int avail = body_bottom - term_row + 1;

			/* Stack-allocated overlay buffer keeps decoration
			 * cheap.  32 slots cover the realistic peak (level
			 * color + a handful of keyword highlights + a
			 * search match per line); producers that would
			 * exceed it must clamp themselves. */
			struct tui_overlay ov[32];
			int n_ov = 0;
			int max_ov = (int)(sizeof(ov) / sizeof(ov[0]));
			if (L->decorate_fn)
				n_ov = L->decorate_fn(s, slen, L->decorate_ctx,
						      ov, max_ov);

			/* Append search highlights as nested overlays.
			 * The "current" match (the one the user just
			 * navigated to) gets a high-contrast bold yellow
			 * background; every other match uses plain reverse
			 * video.  Both styles compose with the outer level
			 * overlay because the renderer re-issues active
			 * opens after every close. */
			if (L->search && L->search->re) {
				/*
				 * Globals beyond total_pushed encode grid
				 * rows (frozen → snapshot at ceiling+r,
				 * live → total_pushed+r).  log_user_to_global
				 * yields those naturally, so search current-
				 * tracking works for grid rows too.
				 */
				long long line_g = log_user_to_global(L, i);
				int line_is_current =
				    line_g >= 0 &&
				    L->search->current_line == line_g;
				size_t off = 0;
				while (n_ov < max_ov && off <= slen) {
					int rc = pcre2_match(
					    L->search->re, (PCRE2_SPTR)s, slen,
					    off, 0, L->search->md, NULL);
					if (rc < 0)
						break;
					PCRE2_SIZE *vec =
					    pcre2_get_ovector_pointer(
						L->search->md);
					size_t mstart = (size_t)vec[0];
					size_t mend = (size_t)vec[1];
					if (mstart >= slen)
						break;
					int is_current =
					    line_is_current &&
					    mstart ==
						L->search->current_start &&
					    mend == L->search->current_end;
					ov[n_ov].start = mstart;
					ov[n_ov].end =
					    mend > slen ? slen : mend;
					if (is_current) {
						/* Bold black on yellow --
						 * pops out against the rest
						 * of the highlights and is
						 * legible regardless of the
						 * underlying level color. */
						ov[n_ov].sgr_open = "1;30;43";
						ov[n_ov].sgr_close = "0";
					} else {
						ov[n_ov].sgr_open = "7";
						ov[n_ov].sgr_close = "27";
					}
					n_ov++;
					/* Advance past this match; for
					 * zero-width hits step one byte
					 * forward so we don't loop. */
					off =
					    (mend > mstart) ? mend : mstart + 1;
				}
			}

			if (n_ov > 1)
				qsort(ov, n_ov, sizeof(ov[0]), overlay_cmp);

			int drew =
			    render_log_line(out, s, slen, body_w, L->origin_x,
					    term_row, skip, avail, ov, n_ov);
			term_row += drew;
		}
	} else if (body_rows > 0) {
		/* Empty buffer: still wipe stale body content. */
		for (int r = 0; r < body_rows; r++)
			sbuf_addf(out, "\x1b[%d;%dH\x1b[0m\x1b[%dX",
				  body_top + r, L->origin_x, body_w);
	}

	if (body_rows > 0 && L->width >= 1)
		render_scrollbar(out, L, body_top, body_rows, bottom_idx);

	if (L->footer && body_bottom < L->origin_y + L->height - 1) {
		sbuf_addf(out, "\x1b[%d;%dH\x1b[37;44m",
			  L->origin_y + L->height - 1, L->origin_x);
		sbuf_addch(out, ' ');
		sbuf_pad(out, L->footer, L->width - 1);
		sbuf_addstr(out, "\x1b[0m");
	}

	/*
	 * Position the cursor at the live grid's coordinates iff the
	 * grid row that holds it actually rendered into the body.  When
	 * scrolled, cursor_term_row stays -1 (cursor's row is past the
	 * visible window) and the leading "\x1b[?25l" keeps the cursor
	 * hidden.
	 */
	if (cursor_term_row > 0 && cursor_term_col > 0)
		sbuf_addf(out, "\x1b[%d;%dH\x1b[?25h", cursor_term_row,
			  cursor_term_col);
}

/* ================================================================== */
/*  Search                                                            */
/* ================================================================== */

int tui_log_search_active(const struct tui_log *L)
{
	return L->search && L->search->re;
}

const char *tui_log_search_pattern(const struct tui_log *L)
{
	return (L->search && L->search->pattern) ? L->search->pattern : NULL;
}

int tui_log_search_total(const struct tui_log *L)
{
	return L->search ? L->search->total_matches : 0;
}

int tui_log_search_index(const struct tui_log *L)
{
	return L->search ? L->search->current_index : 0;
}

void tui_log_search_clear(struct tui_log *L)
{
	if (!L->search)
		return;
	if (L->search->md)
		pcre2_match_data_free(L->search->md);
	if (L->search->re)
		pcre2_code_free(L->search->re);
	free(L->search->pattern);
	free(L->search);
	L->search = NULL;
}

/*
 * Find the first match in line at user-idx @p u whose start is at or
 * after @p min_start.  On hit, returns 1 and writes the match's byte
 * range to @p *start_out / @p *end_out (the @c end is bumped past
 * @c start by one byte for zero-width hits so navigation never gets
 * stuck).  On miss, returns 0 with @p *start_out / @p *end_out
 * untouched.
 */
static int line_match_after(const struct tui_log *L, int u, size_t min_start,
			    size_t *start_out, size_t *end_out)
{
	if (u < 0 || u >= log_total(L))
		return 0;
	const char *s;
	size_t slen;
	log_get_line(L, u, &s, &slen);
	if (min_start > slen)
		return 0;
	int rc = pcre2_match(L->search->re, (PCRE2_SPTR)s, slen, min_start, 0,
			     L->search->md, NULL);
	if (rc < 0)
		return 0;
	PCRE2_SIZE *vec = pcre2_get_ovector_pointer(L->search->md);
	size_t mstart = (size_t)vec[0];
	size_t mend = (size_t)vec[1];
	*start_out = mstart;
	*end_out = mend > mstart ? mend : mstart + 1;
	return 1;
}

/*
 * Find the last match in line at user-idx @p u whose start is strictly
 * less than @p max_start.  Walks all matches forward and keeps the
 * last qualifying one.  Returns 1 on hit, 0 otherwise.
 */
static int line_match_before(const struct tui_log *L, int u, size_t max_start,
			     size_t *start_out, size_t *end_out)
{
	if (u < 0 || u >= log_total(L))
		return 0;
	const char *s;
	size_t slen;
	log_get_line(L, u, &s, &slen);

	size_t off = 0;
	int found = 0;
	while (off <= slen) {
		int rc = pcre2_match(L->search->re, (PCRE2_SPTR)s, slen, off, 0,
				     L->search->md, NULL);
		if (rc < 0)
			break;
		PCRE2_SIZE *vec = pcre2_get_ovector_pointer(L->search->md);
		size_t ms = (size_t)vec[0];
		size_t me = (size_t)vec[1];
		if (ms >= max_start)
			break;
		*start_out = ms;
		*end_out = me > ms ? me : ms + 1;
		found = 1;
		off = me > ms ? me : ms + 1;
	}
	return found;
}

/*
 * Recompute @c current_index from scratch.  Used as a fallback when
 * the incremental delta (++ on next, -- on prev) lost track -- e.g.
 * the current's line was evicted earlier.  Walks every match earlier
 * than (or co-located with) the current cursor in line + byte order
 * and returns the 1-based position of the current match in that
 * ordering, or 0 when no current is set.
 */
static int search_compute_index(const struct tui_log *L)
{
	if (!L->search || !L->search->re || L->search->current_line < 0)
		return 0;
	long long u_ll = log_global_to_user(L, L->search->current_line);
	if (u_ll < 0 || u_ll >= log_total(L))
		return 0;
	int u = (int)u_ll;
	int idx = 0;
	for (int i = 0; i < u; i++) {
		const char *s;
		size_t slen;
		log_get_line(L, i, &s, &slen);
		idx += count_line_matches(L->search, s, slen);
	}
	const char *s;
	size_t slen;
	log_get_line(L, u, &s, &slen);
	size_t off = 0;
	while (off <= slen) {
		int rc = pcre2_match(L->search->re, (PCRE2_SPTR)s, slen, off, 0,
				     L->search->md, NULL);
		if (rc < 0)
			break;
		PCRE2_SIZE *vec = pcre2_get_ovector_pointer(L->search->md);
		size_t ms = (size_t)vec[0];
		size_t me = (size_t)vec[1];
		idx++;
		if (ms == L->search->current_start &&
		    (me > ms ? me : ms + 1) == L->search->current_end)
			return idx;
		off = me > ms ? me : ms + 1;
	}
	return idx;
}

int tui_log_search_set(struct tui_log *L, const char *pattern)
{
	if (!pattern || !*pattern) {
		tui_log_search_clear(L);
		return 0;
	}

	int errcode;
	PCRE2_SIZE erroffset;
	pcre2_code *re = pcre2_compile(
	    (PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
	    PCRE2_DOTALL | PCRE2_MULTILINE, &errcode, &erroffset, NULL);
	if (!re)
		return -1;

	pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
	if (!md) {
		pcre2_code_free(re);
		return -1;
	}

	/* Replace any prior search only after the new one is fully built;
	 * an early failure leaves the previous pattern intact. */
	if (!L->search) {
		L->search = calloc(1, sizeof(*L->search));
		if (!L->search)
			die_errno("calloc");
	} else {
		if (L->search->md)
			pcre2_match_data_free(L->search->md);
		if (L->search->re)
			pcre2_code_free(L->search->re);
		free(L->search->pattern);
	}
	L->search->re = re;
	L->search->md = md;
	L->search->pattern = sbuf_strdup(pattern);
	L->search->current_line = -1;
	L->search->current_index = 0;

	/* Count every match in the visible range up front.  This is the
	 * only O(n) walk -- subsequent appends and evictions update
	 * @c total_matches incrementally inside @ref log_push_line.
	 * When a snapshot ceiling is active we only walk the lines
	 * inside the snapshot; matches in lines that arrived past the
	 * ceiling don't count as visible. */
	int total = log_total(L);
	L->search->total_matches = 0;
	for (int i = 0; i < total; i++) {
		const char *s;
		size_t slen;
		log_get_line(L, i, &s, &slen);
		L->search->total_matches +=
		    count_line_matches(L->search, s, slen);
	}

	/* Position on the newest match: the user typed @c / followed by
	 * a pattern, so they expect to land somewhere visible right
	 * away.  Walking backward from the end of the visible range
	 * picks the last match; @c current_index = @c total_matches
	 * because the last match is the highest-numbered one. */
	if (L->ceiling < 0)
		L->anchor = -1;
	if (L->search->total_matches > 0) {
		for (int i = total - 1; i >= 0; i--) {
			size_t ms, me;
			if (line_match_before(L, i, SIZE_MAX, &ms, &me)) {
				L->search->current_line =
				    log_user_to_global(L, i);
				L->search->current_start = ms;
				L->search->current_end = me;
				L->search->current_index =
				    L->search->total_matches;
				L->anchor = L->search->current_line;
				break;
			}
		}
	}
	return 0;
}

int tui_log_search_next(struct tui_log *L)
{
	if (!tui_log_search_active(L))
		return 0;

	/* Determine where to start.  When there's a current match, step
	 * past it (within-line first: there might be more matches on
	 * the same line); otherwise start from the line currently at
	 * the bottom of the body.  When the current match's line has
	 * been evicted, fall back to the oldest surviving line so
	 * navigation doesn't strand at "nothing found". */
	int total = log_total(L);
	int from_line;
	size_t from_offset;
	if (L->search->current_line >= 0) {
		long long u = log_global_to_user(L, L->search->current_line);
		if (u < 0 || u >= total) {
			from_line = 0;
			from_offset = 0;
		} else {
			from_line = (int)u;
			from_offset = L->search->current_end;
		}
	} else {
		from_line = log_bottom_idx(L);
		if (from_line < 0 || from_line >= total)
			from_line = 0;
		from_offset = 0;
	}

	size_t ms, me;
	int found_at = -1;
	if (line_match_after(L, from_line, from_offset, &ms, &me)) {
		found_at = from_line;
	} else {
		for (int i = from_line + 1; i < total; i++) {
			if (line_match_after(L, i, 0, &ms, &me)) {
				found_at = i;
				break;
			}
		}
	}
	if (found_at < 0)
		return 0;

	int prev_index = L->search->current_index;
	L->search->current_line = log_user_to_global(L, found_at);
	L->search->current_start = ms;
	L->search->current_end = me;
	L->anchor = L->search->current_line;
	if (prev_index > 0)
		L->search->current_index = prev_index + 1;
	else
		L->search->current_index = search_compute_index(L);
	return 1;
}

int tui_log_search_prev(struct tui_log *L)
{
	if (!tui_log_search_active(L))
		return 0;

	int total = log_total(L);
	int from_line;
	size_t upper_bound;
	if (L->search->current_line >= 0) {
		long long u = log_global_to_user(L, L->search->current_line);
		if (u < 0 || u >= total) {
			from_line = total - 1;
			upper_bound = SIZE_MAX;
		} else {
			from_line = (int)u;
			upper_bound = L->search->current_start;
		}
	} else {
		/* No current match yet: walk from the line at the bottom
		 * of the body backward, scanning each line in full. */
		from_line = log_bottom_idx(L);
		if (from_line < 0 || from_line >= total)
			from_line = total - 1;
		upper_bound = SIZE_MAX;
	}
	if (from_line < 0)
		return 0;

	size_t ms, me;
	int found_at = -1;
	if (upper_bound != 0 &&
	    line_match_before(L, from_line, upper_bound, &ms, &me)) {
		found_at = from_line;
	} else {
		for (int i = from_line - 1; i >= 0; i--) {
			if (line_match_before(L, i, SIZE_MAX, &ms, &me)) {
				found_at = i;
				break;
			}
		}
	}
	if (found_at < 0)
		return 0;

	int prev_index = L->search->current_index;
	L->search->current_line = log_user_to_global(L, found_at);
	L->search->current_start = ms;
	L->search->current_end = me;
	L->anchor = L->search->current_line;
	if (prev_index > 1)
		L->search->current_index = prev_index - 1;
	else
		L->search->current_index = search_compute_index(L);
	return 1;
}
