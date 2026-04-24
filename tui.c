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
 * Render one body row @p idx (visible index 0..body_rows-1) with
 * the underlying item at @p item_idx or a blank filler row.
 *
 * Layout per row (1-based columns):
 *   col 1      :  '>' if cursor, ' ' otherwise
 *   col 2      :  ' ' (padding)
 *   col 3..W-V :  left-aligned text, truncated / padded
 *   col W-V..W :  right-aligned value (if any)
 */
static void render_item_row(struct sbuf *sb, const struct tui_list *L, int idx)
{
	int item_idx = L->top + idx;
	sbuf_addf(sb, "\x1b[%d;1H", 2 + idx); /* body row = title + 1 + idx */
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

	if (is_cursor)
		sbuf_addstr(sb, "\x1b[7m"); /* reverse video */
	else if (is_heading)
		sbuf_addstr(sb, "\x1b[1m"); /* bold */
	else if (is_disabled)
		sbuf_addstr(sb, "\x1b[2m"); /* dim */

	sbuf_addch(sb, is_cursor ? '>' : ' ');
	sbuf_addch(sb, ' ');

	int value_w = it->value ? (int)strlen(it->value) : 0;
	/*
	 * Column budget: width - 2 (prefix) - 1 (gap) - value_w - 1 (trailing).
	 * Falls back to "no value" if the value itself wouldn't fit.
	 */
	int text_w = L->width - 3 - value_w - 1;
	if (text_w < 0) {
		text_w = L->width - 3;
		value_w = 0;
	}
	sbuf_pad(sb, it->text, text_w);
	if (value_w > 0) {
		sbuf_addch(sb, ' ');
		sbuf_add(sb, it->value, (size_t)value_w);
		sbuf_addch(sb, ' ');
	} else {
		/* Pad out the trailing cells so the reverse-video cursor
		 * stripe reaches the right edge. */
		for (int i = 0; i < value_w + 1; i++)
			sbuf_addch(sb, ' ');
	}

	sbuf_addstr(sb, "\x1b[0m"); /* reset SGR */
}

void tui_list_render(const struct tui_list *L)
{
	struct sbuf sb = SBUF_INIT;

	/* Full repaint: rather than track dirty regions, clear and redraw.
	 * With sbuf batching the cost is one write per frame -- dwarfed
	 * by the user's keystroke cadence. */
	sbuf_addstr(&sb, "\x1b[2J\x1b[H");

	/* Title bar in reverse video, left-padded. */
	sbuf_addstr(&sb, "\x1b[1;1H\x1b[7m");
	sbuf_addch(&sb, ' ');
	sbuf_pad(&sb, L->title, L->width - 1);
	sbuf_addstr(&sb, "\x1b[0m");

	/* Body rows. */
	int rows = body_rows(L);
	for (int i = 0; i < rows; i++)
		render_item_row(&sb, L, i);

	/* Footer row in reverse video.  L->height is 1-based; position at
	 * the last row. */
	if (L->height >= 2) {
		sbuf_addf(&sb, "\x1b[%d;1H\x1b[7m", L->height);
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

	/* Light-line box using the term.h box-draw macros.  The top line
	 * carries the title; the middle separator splits it from the
	 * input; the input line holds the buffer + cursor; the bottom
	 * line closes the box. */
	/* Top border. */
	sbuf_addf(&sb, "\x1b[%d;%dH", top, left);
	sbuf_addstr(&sb, "\xe2\x94\x8c"); /* U+250C ┌ */
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\x90"); /* U+2510 ┐ */

	/* Title row. */
	sbuf_addf(&sb, "\x1b[%d;%dH", top + 1, left);
	sbuf_addstr(&sb, VL);
	sbuf_addch(&sb, ' ');
	sbuf_pad(&sb, P->title, inner - 1);
	sbuf_addstr(&sb, VL);

	/* Separator. */
	sbuf_addf(&sb, "\x1b[%d;%dH", top + 2, left);
	sbuf_addstr(&sb, "\xe2\x94\x9c"); /* U+251C ├ */
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\xa4"); /* U+2524 ┤ */

	/* Input row.  Scroll the visible window so the cursor stays
	 * inside the inner area even for long buffers. */
	sbuf_addf(&sb, "\x1b[%d;%dH", top + 3, left);
	sbuf_addstr(&sb, VL);
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
	sbuf_addstr(&sb, VL);

	/* Bottom border. */
	sbuf_addf(&sb, "\x1b[%d;%dH", top + 4, left);
	sbuf_addstr(&sb, "\xe2\x94\x94"); /* U+2514 └ */
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\x98"); /* U+2518 ┘ */

	/* Put the cursor at the edit position.  Column: left border + 1
	 * gap + (cursor - view_start).  The +1 for 1-based columns is
	 * already in @c left. */
	sbuf_addf(&sb, "\x1b[%d;%dH", top + 3,
		  left + 2 + (P->cursor - view_start));
	/* Show the cursor while the modal is up -- the alt-screen enter
	 * hid it.  Callers restore by calling term_screen_leave on exit. */
	sbuf_addstr(&sb, "\x1b[?25h");

	fputs(sb.buf, stdout);
	fflush(stdout);
	sbuf_release(&sb);
}
