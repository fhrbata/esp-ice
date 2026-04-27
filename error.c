/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file error.c
 * @brief Error reporting implementation.
 *
 * All output goes through the platform fprintf override, which
 * handles @x{...} color token expansion and UTF-8 on Windows.
 *
 * The diagnostic prefix (error:/warning:/hint:/fatal:) is the only
 * coloured part of the line.  Wrapping the whole message in a colour
 * block was tempting but leaks: a substituted %s arg containing a
 * literal '}' (e.g. a Python-style "{0}" template echoed back in an
 * error message) closes the block early, mangling everything after
 * it.  Keeping the body outside the block makes its content opaque
 * to expand_colors -- callers can still embed @b{...} etc. in their
 * format strings since those tokens parse correctly from depth 0.
 */
#include "ice.h"

/**
 * @brief Format and print a diagnostic message to stderr (red prefix).
 *
 * Output format:  error: file:line: in func: <message>[: strerror]\n
 */
void err_msg(char *file, int line, const char *func, int add_errno, char *fmt,
	     ...)
{
	struct sbuf msg = SBUF_INIT;
	va_list ap;

	sbuf_addf(&msg, "%s:%d: in %s: ", file, line, func);

	va_start(ap, fmt);
	sbuf_vaddf(&msg, fmt, ap);
	va_end(ap);

	if (add_errno)
		sbuf_addf(&msg, ": %s (%d)", strerror(errno), errno);

	fprintf(stderr, "@r{error:} %s\n", msg.buf);
	sbuf_release(&msg);
}

/**
 * @brief Print a warning message to stderr (yellow prefix).
 *
 * Output format:  warning: <message>[: strerror]\n
 */
void warn_msg(int add_errno, const char *fmt, ...)
{
	struct sbuf msg = SBUF_INIT;
	va_list ap;

	va_start(ap, fmt);
	sbuf_vaddf(&msg, fmt, ap);
	va_end(ap);

	if (add_errno)
		sbuf_addf(&msg, ": %s (%d)", strerror(errno), errno);

	fprintf(stderr, "@y{warning:} %s\n", msg.buf);
	sbuf_release(&msg);
}

/**
 * @brief Print a hint message to stderr (orange prefix).
 *
 * Output format:  hint: <message>\n
 *
 * Pair with a preceding err()/warn() or a following die() to suggest
 * a next step to the user.  Because die() exits, call hint() before
 * die() -- the hint will appear above the fatal line.
 */
void hint(const char *fmt, ...)
{
	struct sbuf msg = SBUF_INIT;
	va_list ap;

	va_start(ap, fmt);
	sbuf_vaddf(&msg, fmt, ap);
	va_end(ap);

	fprintf(stderr, "@[38;5;208]{hint:} %s\n", msg.buf);
	sbuf_release(&msg);
}

/**
 * @brief Print a fatal error message to stderr (red prefix) and exit.
 *
 * Output format:  fatal: <message>[: strerror]\n
 */
void NORETURN die_msg(int add_errno, const char *fmt, ...)
{
	struct sbuf msg = SBUF_INIT;
	va_list ap;

	va_start(ap, fmt);
	sbuf_vaddf(&msg, fmt, ap);
	va_end(ap);

	if (add_errno)
		sbuf_addf(&msg, ": %s (%d)", strerror(errno), errno);

	fprintf(stderr, "@r{fatal:} %s\n", msg.buf);
	sbuf_release(&msg);

	exit(EXIT_FAILURE);
}
