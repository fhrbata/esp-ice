/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for zip.c.  The test wrapper (.t) builds a fixture
 * archive in $O using the system @c zip utility (GNU or Info-ZIP);
 * this binary then extracts it and asserts on the resulting tree.
 */
#include "ice.h"
#include "tap.h"
#include "zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *slurp(const char *path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0) {
		sbuf_release(&sb);
		return NULL;
	}
	return sbuf_detach(&sb);
}

static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

int main(void)
{
	/* Happy path: a multi-file archive with a nested directory
	 * gets fully extracted.  The fixture was written by the
	 * wrapper script; we just verify the tree. */
	{
		tap_check(zip_extract_all("basic.zip", "out_basic") == 0);

		char *hello = slurp("out_basic/hello.txt");
		tap_check(hello && !strcmp(hello, "hello\n"));
		free(hello);

		char *nested = slurp("out_basic/sub/nested.txt");
		tap_check(nested && !strcmp(nested, "nested\n"));
		free(nested);

		tap_done("extract multi-file archive with subdirectory");
	}

	/* Path traversal guard: filename with ".." component is
	 * rejected and no files land outside the destination. */
	{
		tap_check(zip_extract_all("slip.zip", "out_slip") == -1);
		/* The attacker file name was "../escape": after rejection,
		 * nothing should have been written at that relative path. */
		tap_check(!file_exists("escape"));
		tap_done("reject zip-slip '..' entry");
	}

	/* Path traversal guard: backslash separators (Windows fopen
	 * accepts them) must also be refused.  The attacker file name
	 * was "..\\escape_bs". */
	{
		tap_check(zip_extract_all("slip_bs.zip", "out_slip_bs") == -1);
		tap_check(!file_exists("escape_bs"));
		tap_done("reject zip-slip backslash entry");
	}

	/* Corrupt (truncated) archive: yaml-like garbage and truncated
	 * data both fail cleanly without a crash. */
	{
		tap_check(zip_extract_all("no_such_file.zip", "out_nx") == -1);
		tap_check(zip_extract_all("garbage.bin", "out_garbage") == -1);
		tap_done("missing / garbage input returns -1");
	}

	return tap_result();
}
