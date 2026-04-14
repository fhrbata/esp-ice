/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wmain.c
 * @brief Windows wide-char entry point.
 *
 * Provides wmain() which converts the wide-char argument vector to UTF-8
 * and delegates to main() (defined in ice.c).  Separated from other
 * Windows files so that the extern main declaration does not leak into
 * translation units that define their own main (e.g. unit tests).
 *
 * Linked with -municode so that the CRT calls wmain() instead of main().
 */
#include <fcntl.h>
#include <io.h>
#include <stddef.h>
#include <stdlib.h>

#include <processenv.h>
#include <wchar.h>
#include <windows.h>

#include "../../ice.h"
#include "wconv.h"

extern int main(int argc, char **argv);

/**
 * @brief Set up console and stream modes before entering main().
 *
 * For console handles (stdout, stderr): try to enable ANSI escape code
 * processing (ENABLE_VIRTUAL_TERMINAL_PROCESSING). If successful, sets
 * use_vt so that ANSI codes are emitted directly. If not (legacy
 * Windows), use_vt stays 0 and cfprintf will use the Console API
 * fallback for colors.
 *
 * For redirected handles (pipes, files): switch the CRT file
 * descriptor to binary mode so that \n is not translated to \r\n,
 * keeping output clean for piping.
 */
static void setup_io(void)
{
	int fds[] = {STDOUT_FILENO, STDERR_FILENO};
	DWORD std_handles[] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};

	for (int i = 0; i < 2; i++) {
		HANDLE h = GetStdHandle(std_handles[i]);
		DWORD mode;

		if (GetConsoleMode(h, &mode)) {
			if (SetConsoleMode(
				h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
				use_vt = 1;
		} else {
			_setmode(fds[i], _O_BINARY);
		}
	}
}

/**
 * @brief Windows wide-char entry point.
 *
 * Sets up I/O modes, converts the wide-char argument vector to UTF-8
 * strings, and delegates to main(). This is the true entry point on
 * Windows (linked with -municode); main() is defined in ice.c.
 */
int wmain(int argc, wchar_t **wargv)
{
	char **argv;

	setup_io();

	argv = malloc(((size_t)argc + 1) * sizeof(char *));
	if (!argv)
		die_errno("malloc");

	for (int i = 0; i < argc; i++) {
		argv[i] = wcs_to_mbs(wargv[i]);
		if (!argv[i])
			die("argument conversion failed");
	}
	argv[argc] = NULL;

	return main(argc, argv);
}
