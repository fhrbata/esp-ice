/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/posix/term.c
 * @brief POSIX terminal primitives -- raw mode, size, alt screen.
 *
 * Used by the interactive serial monitor and TUI commands (menuconfig,
 * future log viewers).  See term.h for the public API.
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

/* platform.h must come after the system headers above (it redefines
 * fprintf et al).  term.h carries the public declarations we
 * implement here. */
#include "platform.h"
#include "term.h"

static struct termios saved_tio;
static int raw_active;
static int screen_active;

static volatile sig_atomic_t resize_flag;
static struct sigaction saved_winch;
static int winch_installed;

static void on_winch(int sig)
{
	(void)sig;
	resize_flag = 1;
}

static void install_winch(void)
{
	struct sigaction sa;

	if (winch_installed)
		return;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_winch;
	sigemptyset(&sa.sa_mask);
	/* No SA_RESTART: we want select() to return EINTR on resize so
	 * the TUI loop can redraw and term_read() can bail early. */
	sa.sa_flags = 0;
	sigaction(SIGWINCH, &sa, &saved_winch);
	winch_installed = 1;
}

static void restore_on_exit(void)
{
	if (screen_active)
		term_screen_leave();
	if (raw_active)
		term_raw_leave();
}

int term_raw_enter(unsigned flags)
{
	struct termios tio;
	tcflag_t lflag_clear;

	if (!isatty(STDIN_FILENO))
		return -ENOTTY;

	if (tcgetattr(STDIN_FILENO, &saved_tio) < 0)
		return -errno;

	tio = saved_tio;

	/* Raw input: no echo, no canonical buffering, no extended input
	 * processing.  ISIG (signal generation from Ctrl-C / Ctrl-Z / ...)
	 * is left intact when TERM_RAW_KEEP_SIG is requested so the caller
	 * can poll stdin while still letting Ctrl-C kill the process. */
	tio.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	lflag_clear = ECHO | ICANON | IEXTEN;
	if (!(flags & TERM_RAW_KEEP_SIG))
		lflag_clear |= ISIG;
	tio.c_lflag &= ~lflag_clear;
	tio.c_cflag |= CS8;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &tio) < 0)
		return -errno;

	install_winch();

	if (!raw_active) {
		atexit(restore_on_exit);
		raw_active = 1;
	}

	return 0;
}

void term_raw_leave(void)
{
	if (!raw_active)
		return;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
	raw_active = 0;
}

ssize_t term_read(void *buf, size_t n, unsigned timeout_ms)
{
	fd_set rfds;
	struct timeval tv;
	int rc;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);

	tv.tv_sec = (long)(timeout_ms / 1000u);
	tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

	/* Retry on EINTR unless a SIGWINCH woke us -- TUI callers need
	 * the early return so they can redraw on resize. */
	do {
		rc = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
	} while (rc < 0 && errno == EINTR && !resize_flag);

	if (rc < 0)
		return errno == EINTR ? 0 : -1;
	if (rc == 0)
		return 0;

	return read(STDIN_FILENO, buf, n);
}

int term_size(int *cols, int *rows)
{
	struct winsize ws;
	int fd = isatty(STDOUT_FILENO) ? STDOUT_FILENO : STDIN_FILENO;

	if (ioctl(fd, TIOCGWINSZ, &ws) < 0)
		return -errno;
	if (cols)
		*cols = ws.ws_col ? ws.ws_col : 80;
	if (rows)
		*rows = ws.ws_row ? ws.ws_row : 24;
	return 0;
}

int term_resize_pending(void)
{
	if (!resize_flag)
		return 0;
	resize_flag = 0;
	return 1;
}

void term_screen_enter(void)
{
	if (screen_active)
		return;
	/* DEC alt-screen (1049) + hide cursor. */
	fputs("\x1b[?1049h\x1b[?25l", stdout);
	fflush(stdout);
	if (!raw_active)
		atexit(restore_on_exit);
	screen_active = 1;
}

void term_screen_leave(void)
{
	if (!screen_active)
		return;
	fputs("\x1b[?25h\x1b[?1049l", stdout);
	fflush(stdout);
	screen_active = 0;
}
