/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for base64.c -- decoding helper used by crt-bundle and
 * the (incoming) coredump command.
 */
#include "base64.h"
#include "ice.h"
#include "tap.h"

static int decode_eq(const char *src, const char *expected, size_t expected_len)
{
	struct sbuf out = SBUF_INIT;
	int rc = base64_decode(src, strlen(src), &out);
	int ok = rc == 0 && out.len == expected_len &&
		 memcmp(out.buf, expected, expected_len) == 0;
	sbuf_release(&out);
	return ok;
}

int main(void)
{
	/* Empty input yields empty output and returns 0. */
	{
		struct sbuf out = SBUF_INIT;
		tap_check(base64_decode("", 0, &out) == 0);
		tap_check(out.len == 0);
		sbuf_release(&out);
		tap_done("empty input decodes to empty output");
	}

	/* Three-byte alignment (no padding). */
	tap_check(decode_eq("Zm9v", "foo", 3));
	tap_done("decode without padding");

	/* One '=' pad byte: 2-byte tail. */
	tap_check(decode_eq("Zm9vYmE=", "fooba", 5));
	tap_done("decode with one '=' pad");

	/* Two '=' pad bytes: 1-byte tail. */
	tap_check(decode_eq("Zg==", "f", 1));
	tap_done("decode with two '=' pads");

	/* Whitespace inside the stream is skipped (PEM / UART line wrap). */
	tap_check(decode_eq("Zm9v\nYmFy", "foobar", 6));
	tap_check(decode_eq("Zm9v\r\nYmFy", "foobar", 6));
	tap_check(decode_eq("Zm 9v\tYmFy", "foobar", 6));
	tap_done("whitespace bytes are tolerated");

	/* PEM-style 64-column wrap with trailing newline. */
	{
		const char *wrapped = "VGhpcyBpcyBhIHRlc3Qgc3RyaW5nIGZvciBQRU0t"
				      "c3R5bGUgd3JhcA==\n";
		const char *want = "This is a test string for PEM-style wrap";
		tap_check(decode_eq(wrapped, want, strlen(want)));
		tap_done("PEM-wrapped input decodes correctly");
	}

	/* '+' and '/' are valid alphabet members. */
	tap_check(decode_eq("Pz8/", "???", 3));
	tap_done("'/' alphabet character decodes");

	/* Invalid bytes return -1; output state up to the bad byte is
	 * unspecified, but the call must report failure. */
	{
		struct sbuf out = SBUF_INIT;
		tap_check(base64_decode("Zm9v!Zm9v", 9, &out) == -1);
		sbuf_release(&out);
		tap_done("invalid byte returns -1");
	}

	/* '=' before the end terminates decoding -- the bytes after the
	 * pad are not consulted. */
	tap_check(decode_eq("Zg==garbage", "f", 1));
	tap_done("bytes after '=' pad are ignored");

	tap_result();
	return 0;
}
