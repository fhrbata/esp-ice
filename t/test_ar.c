/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for ar.c -- the static-archive (AR) reader.
 *
 * Fixture archive `test.a` is assembled by the `.t` wrapper from two
 * small member files; this test iterates it and checks that each
 * member surfaces with its expected name and size.
 */
#include "ice.h"
#include "tap.h"

int main(void)
{
	struct sbuf sb = SBUF_INIT;
	struct ar_reader r;
	struct ar_member m;
	int seen_foo = 0, seen_bar = 0;
	int n_members = 0;

	tap_check(sbuf_read_file(&sb, "test.a") > 0);
	ar_reader_init(&r, sb.buf, sb.len);

	while (ar_reader_next(&r, &m)) {
		n_members++;
		if (!strcmp(m.name, "foo.txt")) {
			seen_foo = 1;
			tap_check(m.size == 15); /* "contents of foo" */
			tap_check(memcmp(m.data, "contents of foo", 15) == 0);
		} else if (!strcmp(m.name, "bar.txt")) {
			seen_bar = 1;
			tap_check(m.size == 17); /* "file bar contents" */
			tap_check(memcmp(m.data, "file bar contents", 17) == 0);
		}
		free(m.name);
	}

	/* BSD ar (e.g. Xcode's) injects a __.SYMDEF member that our parser
	 * surfaces as a regular entry, so the exact count is host-dependent;
	 * just require both our fixtures show up with the right contents. */
	tap_check(n_members >= 2);
	tap_check(seen_foo);
	tap_check(seen_bar);
	tap_done(
	    "ar_reader iterates the fixture and exposes correct name/data");

	sbuf_release(&sb);
	return tap_result();
}
