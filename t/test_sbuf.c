/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for sbuf.c -- the dynamic NUL-terminated string buffer.
 */
#include "ice.h"
#include "tap.h"

int main(void)
{
	/* SBUF_INIT yields a valid empty buffer (sb->buf is never NULL). */
	{
		struct sbuf sb = SBUF_INIT;
		tap_check(sb.buf != NULL);
		tap_check(sb.len == 0);
		tap_check(sb.alloc == 0);
		tap_check(sb.buf[0] == '\0');
		sbuf_release(&sb);
		tap_done("SBUF_INIT yields a valid empty buffer");
	}

	/* sbuf_addch grows on demand and NUL-terminates after each write. */
	{
		struct sbuf sb = SBUF_INIT;
		sbuf_addch(&sb, 'a');
		sbuf_addch(&sb, 'b');
		sbuf_addch(&sb, 'c');
		tap_check(sb.len == 3);
		tap_check(strcmp(sb.buf, "abc") == 0);
		sbuf_release(&sb);
		tap_done("sbuf_addch appends and NUL-terminates");
	}

	/* sbuf_addstr concatenates strings. */
	{
		struct sbuf sb = SBUF_INIT;
		sbuf_addstr(&sb, "hello");
		sbuf_addstr(&sb, " ");
		sbuf_addstr(&sb, "world");
		tap_check(sb.len == 11);
		tap_check(strcmp(sb.buf, "hello world") == 0);
		sbuf_release(&sb);
		tap_done("sbuf_addstr concatenates");
	}

	/* sbuf_addf supports printf-style formatting. */
	{
		struct sbuf sb = SBUF_INIT;
		sbuf_addf(&sb, "%s=%d", "answer", 42);
		tap_check(strcmp(sb.buf, "answer=42") == 0);
		sbuf_release(&sb);
		tap_done("sbuf_addf formats printf-style");
	}

	/* sbuf_reset clears length, keeps allocation for reuse. */
	{
		struct sbuf sb = SBUF_INIT;
		size_t cap;
		sbuf_addstr(&sb, "longer than empty");
		cap = sb.alloc;
		sbuf_reset(&sb);
		tap_check(sb.len == 0);
		tap_check(sb.alloc == cap);
		tap_check(sb.buf[0] == '\0');
		sbuf_release(&sb);
		tap_done("sbuf_reset clears length, keeps allocation");
	}

	/* sbuf_release returns the buffer to the empty sentinel. */
	{
		struct sbuf sb = SBUF_INIT;
		sbuf_addstr(&sb, "anything");
		sbuf_release(&sb);
		tap_check(sb.len == 0);
		tap_check(sb.alloc == 0);
		tap_check(sb.buf == sbuf_empty);
		tap_done("sbuf_release returns to empty sentinel");
	}

	/* sbuf_strdup / sbuf_strndup. */
	{
		char *a = sbuf_strdup("abc");
		char *b = sbuf_strndup("abcdefg", 3);
		tap_check(a != NULL && strcmp(a, "abc") == 0);
		tap_check(b != NULL && strcmp(b, "abc") == 0);
		free(a);
		free(b);
		tap_done("sbuf_strdup and sbuf_strndup duplicate strings");
	}

	/* sbuf_strndup reads at most n bytes, and stops early at a NUL. */
	{
		const char unterminated[3] = {'x', 'y', 'z'};
		char *a = sbuf_strndup(unterminated, 2);
		char *b = sbuf_strndup("ab\0cd", 5);
		tap_check(a != NULL && strcmp(a, "xy") == 0);
		tap_check(b != NULL && strcmp(b, "ab") == 0);
		free(a);
		free(b);
		tap_done("sbuf_strndup stops at n bytes or at a NUL");
	}

	/* sbuf_getline reads NUL-terminated lines from a buffer in-place,
	 * including the last line when there is no trailing newline. */
	{
		char buf[] = "line1\nline2\nline3";
		size_t pos = 0;
		char *l;

		l = sbuf_getline(buf, sizeof(buf) - 1, &pos);
		tap_check(l != NULL && strcmp(l, "line1") == 0);
		l = sbuf_getline(buf, sizeof(buf) - 1, &pos);
		tap_check(l != NULL && strcmp(l, "line2") == 0);
		l = sbuf_getline(buf, sizeof(buf) - 1, &pos);
		tap_check(l != NULL && strcmp(l, "line3") == 0);
		l = sbuf_getline(buf, sizeof(buf) - 1, &pos);
		tap_check(l == NULL);
		tap_done("sbuf_getline iterates lines, last has no newline");
	}

	/* sbuf_split: max-N tokens, last gets the remainder verbatim. */
	{
		char in[] = "a b c d e";
		char *tok[3];
		int n = sbuf_split(in, tok, 3);
		tap_check(n == 3);
		tap_check(strcmp(tok[0], "a") == 0);
		tap_check(strcmp(tok[1], "b") == 0);
		tap_check(strcmp(tok[2], "c d e") == 0);
		tap_done("sbuf_split honours max and preserves remainder");
	}

	return tap_result();
}
