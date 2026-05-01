/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for tui.c -- the rectangle layout helpers and the
 * widget origin (origin_x/origin_y) accessors that let two or more
 * panes share a single terminal frame without trampling each other.
 *
 * The render path is exercised indirectly: we set an origin on a
 * @ref tui_log, render a frame to an @c sbuf, and assert that the
 * emitted CSI cursor-position codes land at the origin-shifted
 * coordinates rather than at @c (1,1).
 */
#include "ice.h"
#include "sbuf.h"
#include "tap.h"
#include "tui.h"

#include <string.h>

/* Return non-zero if @p haystack contains the literal byte sequence
 * @p needle.  Plain memmem-style scan; the haystack here is always
 * a render frame of a few hundred bytes so the cost is negligible. */
static int contains(const struct sbuf *haystack, const char *needle)
{
	size_t nlen = strlen(needle);
	if (nlen == 0)
		return 1;
	if (haystack->len < nlen)
		return 0;
	for (size_t i = 0; i + nlen <= haystack->len; i++) {
		if (memcmp(haystack->buf + i, needle, nlen) == 0)
			return 1;
	}
	return 0;
}

int main(void)
{
	/* tui_rect_split_v halves a parent into left + right covering
	 * exactly the parent with no gap. */
	{
		struct tui_rect parent = {.x = 1, .y = 1, .w = 80, .h = 24};
		struct tui_rect left, right;
		tui_rect_split_v(&parent, &left, &right, 40);
		tap_check(left.x == 1 && left.y == 1);
		tap_check(left.w == 40 && left.h == 24);
		tap_check(right.x == 41 && right.y == 1);
		tap_check(right.w == 40 && right.h == 24);
		tap_done("split_v halves a 80x24 rect at column 40");
	}

	/* split_v clamps a negative left width to zero (right takes all). */
	{
		struct tui_rect parent = {.x = 5, .y = 7, .w = 30, .h = 10};
		struct tui_rect left, right;
		tui_rect_split_v(&parent, &left, &right, -3);
		tap_check(left.w == 0);
		tap_check(right.x == 5 && right.w == 30);
		tap_done("split_v clamps negative left_w to zero");
	}

	/* split_v clamps a too-large left width to the parent width. */
	{
		struct tui_rect parent = {.x = 1, .y = 1, .w = 20, .h = 5};
		struct tui_rect left, right;
		tui_rect_split_v(&parent, &left, &right, 999);
		tap_check(left.w == 20);
		tap_check(right.x == 21 && right.w == 0);
		tap_done("split_v clamps oversized left_w to parent width");
	}

	/* split_h splits along the row axis and preserves the x range. */
	{
		struct tui_rect parent = {.x = 10, .y = 5, .w = 60, .h = 20};
		struct tui_rect top, bottom;
		tui_rect_split_h(&parent, &top, &bottom, 8);
		tap_check(top.x == 10 && top.y == 5);
		tap_check(top.w == 60 && top.h == 8);
		tap_check(bottom.x == 10 && bottom.y == 13);
		tap_check(bottom.w == 60 && bottom.h == 12);
		tap_done("split_h splits at row 8 from y=5");
	}

	/* tui_rect_inset shrinks all four sides simultaneously. */
	{
		struct tui_rect r = {.x = 10, .y = 10, .w = 20, .h = 10};
		struct tui_rect inner = tui_rect_inset(r, 1, 2, 1, 2);
		tap_check(inner.x == 12 && inner.y == 11);
		tap_check(inner.w == 16 && inner.h == 8);
		tap_done("inset trims top/right/bottom/left margins");
	}

	/* inset that exceeds the parent dimensions clamps to zero. */
	{
		struct tui_rect r = {.x = 1, .y = 1, .w = 4, .h = 3};
		struct tui_rect inner = tui_rect_inset(r, 5, 5, 5, 5);
		tap_check(inner.w == 0 && inner.h == 0);
		tap_done("inset larger than parent clamps to 0x0");
	}

	/* Nested splits build a 2x2 grid -- the four rects cover the
	 * parent exactly. */
	{
		struct tui_rect parent = {.x = 1, .y = 1, .w = 100, .h = 30};
		struct tui_rect left, right, lt, lb, rt, rb;
		tui_rect_split_v(&parent, &left, &right, 50);
		tui_rect_split_h(&left, &lt, &lb, 15);
		tui_rect_split_h(&right, &rt, &rb, 15);
		tap_check(lt.x == 1 && lt.y == 1 && lt.w == 50 && lt.h == 15);
		tap_check(lb.x == 1 && lb.y == 16 && lb.w == 50 && lb.h == 15);
		tap_check(rt.x == 51 && rt.y == 1 && rt.w == 50 && rt.h == 15);
		tap_check(rb.x == 51 && rb.y == 16 && rb.w == 50 && rb.h == 15);
		tap_done(
		    "nested splits compose a 2x2 grid covering the parent");
	}

	/* tui_log_init defaults origin to (1, 1) so existing single-pane
	 * callers behave exactly as before the origin field was added. */
	{
		struct tui_log L;
		tui_log_init(&L, 100);
		tap_check(L.origin_x == 1);
		tap_check(L.origin_y == 1);
		tui_log_release(&L);
		tap_done("tui_log_init defaults origin to (1, 1)");
	}

	/* A log rendered at origin (1, 1) emits cursor positioning at
	 * column 1 -- baseline that previous releases relied on. */
	{
		struct tui_log L;
		tui_log_init(&L, 100);
		tui_log_resize(&L, 80, 24);
		tui_log_append(&L, "hello\n", 6);

		struct sbuf out = SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(contains(&out, "\x1b[1;1H"));
		sbuf_release(&out);
		tui_log_release(&L);
		tap_done("origin (1,1) emits CSI at column 1");
	}

	/* A log moved to origin (50, 10) emits cursor positioning at
	 * (10, 50) and never at column 1 -- the entire frame stays
	 * within the pane's column range so a neighbouring pane on the
	 * left half is preserved. */
	{
		struct tui_log L;
		tui_log_init(&L, 100);
		tui_log_set_origin(&L, 50, 10);
		tui_log_resize(&L, 30, 12);
		tui_log_append(&L, "hello\n", 6);

		struct sbuf out = SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(contains(&out, "\x1b[10;50H"));
		tap_check(!contains(&out, "\x1b[1;1H"));
		tap_check(!contains(&out, ";1H"));
		sbuf_release(&out);
		tui_log_release(&L);
		tap_done("origin (50,10) shifts every CSI off column 1");
	}

	/* Frame should not contain \x1b[K (clear-to-EOL) at all -- pane
	 * mode replaces it with bounded ECH (\x1b[<n>X) so adjacent
	 * panes' content is not erased. */
	{
		struct tui_log L;
		tui_log_init(&L, 100);
		tui_log_set_origin(&L, 50, 10);
		tui_log_resize(&L, 30, 12);
		tui_log_append(&L, "hi\n", 3);

		struct sbuf out = SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(!contains(&out, "\x1b[K"));
		sbuf_release(&out);
		tui_log_release(&L);
		tap_done("pane render uses ECH instead of clear-to-EOL");
	}

	/* tui_list / tui_prompt / tui_info also default to origin (1,1)
	 * after their respective init functions. */
	{
		struct tui_list listw;
		struct tui_prompt promptw;
		struct tui_info infow;
		tui_list_init(&listw);
		tui_prompt_init(&promptw, "title", "");
		tui_info_init(&infow, "title", "body");
		tap_check(listw.origin_x == 1 && listw.origin_y == 1);
		tap_check(promptw.origin_x == 1 && promptw.origin_y == 1);
		tap_check(infow.origin_x == 1 && infow.origin_y == 1);
		tui_info_release(&infow);
		tap_done("list/prompt/info init defaults origin to (1,1)");
	}

	return tap_result();
}
