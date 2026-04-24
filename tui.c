/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tui.c
 * @brief Scrollable list + modal prompt widgets built on term_*.
 *
 * Both widgets build their frame into an @c sbuf and write it to
 * stdout in one go, so a redraw is a single syscall regardless of
 * how many cells change.
 */
#include "tui.h"
#include "ice.h"

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

void tui_list_init(struct tui_list *L) { memset(L, 0, sizeof(*L)); }

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
	sbuf_addf(sb, "\x1b[%d;1H", 2 + idx);
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

void tui_list_render(const struct tui_list *L)
{
	struct sbuf sb = SBUF_INIT;

	/*
	 * Full repaint.  Every row rewrites its full width, so we don't
	 * need a screen clear -- which was the source of the visible
	 * flash users saw on each redraw.  Just hide the cursor for the
	 * duration (so ESC[r;cH moves don't flick a visible caret across
	 * the screen); a modal prompt layered on top re-enables it at
	 * its input position.
	 */
	sbuf_addstr(&sb, "\x1b[?25l");

	/* Title bar: bold white on blue (classic menuconfig palette).
	 * Includes a one-space left margin so the text doesn't crowd
	 * the terminal edge. */
	sbuf_addstr(&sb, "\x1b[1;1H\x1b[1;37;44m");
	sbuf_addch(&sb, ' ');
	sbuf_pad(&sb, L->title, L->width - 1);
	sbuf_addstr(&sb, "\x1b[0m");

	/* Body rows. */
	int rows = body_rows(L);
	for (int i = 0; i < rows; i++)
		render_item_row(&sb, L, i);

	/* Footer: white on blue (no bold) -- matches the title palette
	 * but reads as a secondary band. */
	if (L->height >= 2) {
		sbuf_addf(&sb, "\x1b[%d;1H\x1b[37;44m", L->height);
		sbuf_addch(&sb, ' ');
		sbuf_pad(&sb, L->footer, L->width - 1);
		sbuf_addstr(&sb, "\x1b[0m");
	}

	fputs(sb.buf, stdout);
	fflush(stdout);
	sbuf_release(&sb);
}

/* ================================================================== */
/*  Modal input prompt                                                */
/* ================================================================== */

void tui_prompt_init(struct tui_prompt *P, const char *title,
		     const char *initial)
{
	memset(P, 0, sizeof(*P));
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

void tui_prompt_render(const struct tui_prompt *P)
{
	struct sbuf sb = SBUF_INIT;
	int cols = prompt_box_cols(P);
	int left = (P->width - cols) / 2 + 1; /* 1-based column */
	int top = (P->height - PROMPT_BOX_ROWS) / 2 + 1;
	int inner = cols - 2; /* between the two vertical borders */

	/*
	 * Hide the cursor for the duration of the frame.  Without this,
	 * each ESC[r;cH between box-draw commands briefly positions a
	 * visible cursor at the border cells, producing the "cursor jumps
	 * around the screen" flicker users see when the modal pops up.
	 * Re-enabled at the final cursor placement below.
	 */
	sbuf_addstr(&sb, "\x1b[?25l");

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
	sbuf_addf(&sb, "\x1b[%d;%dH%s", top, left, MODAL_BORDER);
	sbuf_addstr(&sb, "\xe2\x94\x8c"); /* U+250C ┌ */
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\x90"); /* U+2510 ┐ */

	/* Title row. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", top + 1, left, MODAL_BORDER);
	sbuf_addstr(&sb, VL);
	sbuf_addstr(&sb, MODAL_TITLE);
	sbuf_addch(&sb, ' ');
	sbuf_pad(&sb, P->title, inner - 1);
	sbuf_addstr(&sb, MODAL_BORDER);
	sbuf_addstr(&sb, VL);

	/* Separator. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", top + 2, left, MODAL_BORDER);
	sbuf_addstr(&sb, "\xe2\x94\x9c"); /* U+251C ├ */
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\xa4"); /* U+2524 ┤ */

	/* Input row.  Scroll the visible window so the cursor stays
	 * inside the inner area even for long buffers. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", top + 3, left, MODAL_BORDER);
	sbuf_addstr(&sb, VL);
	sbuf_addstr(&sb, MODAL_INPUT);
	sbuf_addch(&sb, ' ');
	int view_w = inner - 1;
	int view_start = P->cursor > view_w - 1 ? P->cursor - (view_w - 1) : 0;
	int view_len = P->len - view_start;
	if (view_len > view_w)
		view_len = view_w;
	if (view_len > 0)
		sbuf_add(&sb, P->buf + view_start, (size_t)view_len);
	for (int i = view_len; i < view_w; i++)
		sbuf_addch(&sb, ' ');
	sbuf_addstr(&sb, MODAL_BORDER);
	sbuf_addstr(&sb, VL);

	/* Bottom border. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", top + 4, left, MODAL_BORDER);
	sbuf_addstr(&sb, "\xe2\x94\x94"); /* U+2514 └ */
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\x98"); /* U+2518 ┘ */
	sbuf_addstr(&sb, MODAL_RESET);

	/*
	 * Dim hint line just below the box advertising the key bindings.
	 * Fits within the same horizontal footprint so the eye groups it
	 * with the dialog.  Rendered only when there's a row to spare
	 * (on extremely short terminals, skip).
	 */
	if (top + PROMPT_BOX_ROWS <= P->height) {
		const char *hint = "Enter accept  \xe2\x80\xa2  Esc cancel";
		sbuf_addf(&sb, "\x1b[%d;%dH%s", top + PROMPT_BOX_ROWS, left,
			  MODAL_HINT);
		/* Left-pad one space so the hint lines up with the box
		 * content rather than the border. */
		sbuf_addch(&sb, ' ');
		sbuf_pad(&sb, hint, cols - 1);
		sbuf_addstr(&sb, MODAL_RESET);
	}

	/* Put the cursor at the edit position.  Column: left border + 1
	 * gap + (cursor - view_start).  The +1 for 1-based columns is
	 * already in @c left. */
	sbuf_addf(&sb, "\x1b[%d;%dH", top + 3,
		  left + 2 + (P->cursor - view_start));
	/*
	 * Re-show the cursor at its final edit position.  The alt-screen
	 * enter hid it, and tui_list_render also hides it at the top of
	 * every frame, so this is the one place a visible cursor is
	 * wanted while the TUI is active.
	 */
	sbuf_addstr(&sb, "\x1b[?25h");

#undef MODAL_BORDER
#undef MODAL_TITLE
#undef MODAL_INPUT
#undef MODAL_HINT
#undef MODAL_RESET

	fputs(sb.buf, stdout);
	fflush(stdout);
	sbuf_release(&sb);
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
	I->title = title;
	I->body = body ? body : "";
	info_split_lines(I);
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

void tui_info_render(const struct tui_info *I)
{
	struct sbuf sb = SBUF_INIT;

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
	int left = (I->width - cols) / 2 + 1;
	int top = (I->height - box_rows) / 2 + 1;
	int inner = cols - 2;
	int body_rows = box_rows - 4;

	sbuf_addstr(&sb, "\x1b[?25l");

	/* Top border. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", top, left, INFO_BORDER);
	sbuf_addstr(&sb, "\xe2\x94\x8c");
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\x90");

	/* Title row. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", top + 1, left, INFO_BORDER);
	sbuf_addstr(&sb, VL);
	sbuf_addstr(&sb, INFO_TITLE);
	sbuf_addch(&sb, ' ');
	sbuf_pad(&sb, I->title, inner - 1);
	sbuf_addstr(&sb, INFO_BORDER);
	sbuf_addstr(&sb, VL);

	/* Separator. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", top + 2, left, INFO_BORDER);
	sbuf_addstr(&sb, "\xe2\x94\x9c");
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\xa4");

	/* Body rows. */
	for (int r = 0; r < body_rows; r++) {
		int line_idx = I->top_line + r;
		sbuf_addf(&sb, "\x1b[%d;%dH%s", top + 3 + r, left, INFO_BORDER);
		sbuf_addstr(&sb, VL);
		sbuf_addstr(&sb, INFO_BODY);
		sbuf_addch(&sb, ' ');
		if (line_idx < I->n_lines) {
			int len = I->line_lens[line_idx];
			if (len > inner - 1)
				len = inner - 1;
			sbuf_add(&sb, I->lines[line_idx], (size_t)len);
			for (int i = len; i < inner - 1; i++)
				sbuf_addch(&sb, ' ');
		} else {
			for (int i = 0; i < inner - 1; i++)
				sbuf_addch(&sb, ' ');
		}
		sbuf_addstr(&sb, INFO_BORDER);
		sbuf_addstr(&sb, VL);
	}

	/* Bottom border. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", top + box_rows - 1, left, INFO_BORDER);
	sbuf_addstr(&sb, "\xe2\x94\x94");
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\x98");
	sbuf_addstr(&sb, INFO_RESET);

	/* Hint line below the box.  Scroll indicator when there's more
	 * content than fits. */
	if (top + box_rows <= I->height) {
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
		sbuf_addf(&sb, "\x1b[%d;%dH%s", top + box_rows, left,
			  INFO_HINT);
		sbuf_addch(&sb, ' ');
		struct sbuf hint = SBUF_INIT;
		sbuf_addstr(&hint,
			    "\xe2\x86\x91\xe2\x86\x93 scroll  \xe2\x80\xa2"
			    "  Esc / q / Enter close");
		sbuf_addstr(&hint, more);
		sbuf_pad(&sb, hint.buf, cols - 1);
		sbuf_release(&hint);
		sbuf_addstr(&sb, INFO_RESET);
	}

#undef INFO_BORDER
#undef INFO_TITLE
#undef INFO_BODY
#undef INFO_HINT
#undef INFO_RESET

	fputs(sb.buf, stdout);
	fflush(stdout);
	sbuf_release(&sb);
}
