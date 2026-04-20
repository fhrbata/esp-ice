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
 */
#include "ice.h"

/**
 * @brief Format and print a diagnostic message to stderr (red).
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

	fprintf(stderr, "@r{error: %s}\n", msg.buf);
	sbuf_release(&msg);
}

/**
 * @brief Print a warning message to stderr (yellow).
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

	fprintf(stderr, "@y{warning: %s}\n", msg.buf);
	sbuf_release(&msg);
}

/**
 * @brief Print a hint message to stderr (orange).
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

	fprintf(stderr, "@[38;5;208]{hint: %s}\n", msg.buf);
	sbuf_release(&msg);
}

/**
 * @brief Print a fatal error message to stderr (red) and exit.
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

	fprintf(stderr, "@r{fatal: %s}\n", msg.buf);
	sbuf_release(&msg);

	exit(EXIT_FAILURE);
}
