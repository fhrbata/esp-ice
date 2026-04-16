/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/posix/console.c
 * @brief POSIX raw-mode console for interactive tools (serial monitor).
 *
 * Puts stdin into character-at-a-time, no-echo mode with signals
 * disabled so that control characters (Ctrl-C, Ctrl-], ...) arrive
 * as regular bytes.  Output processing (OPOST) is left enabled so
 * that '\n' still produces a carriage return.
 */
#include <errno.h>
#include <stdlib.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

/* Include platform.h for the console_* declarations.  Must come after
 * the system headers above (platform.h redefines fprintf et al.). */
#include "platform.h"

static struct termios saved_tio;
static int raw_active;

static void restore_on_exit(void)
{
	if (raw_active)
		console_raw_leave();
}

int console_raw_enter(void)
{
	struct termios tio;

	if (!isatty(STDIN_FILENO))
		return -ENOTTY;

	if (tcgetattr(STDIN_FILENO, &saved_tio) < 0)
		return -errno;

	tio = saved_tio;

	/* Raw input: no echo, no canonical buffering, no signal
	 * generation, no extended input processing. */
	tio.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	tio.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
	tio.c_cflag |= CS8;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &tio) < 0)
		return -errno;

	if (!raw_active) {
		atexit(restore_on_exit);
		raw_active = 1;
	}

	return 0;
}

void console_raw_leave(void)
{
	if (!raw_active)
		return;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
	raw_active = 0;
}

ssize_t console_read(void *buf, size_t n, unsigned timeout_ms)
{
	fd_set rfds;
	struct timeval tv;
	int rc;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);

	tv.tv_sec = (long)(timeout_ms / 1000u);
	tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

	do {
		rc = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
		return -1;
	if (rc == 0)
		return 0;

	return read(STDIN_FILENO, buf, n);
}
