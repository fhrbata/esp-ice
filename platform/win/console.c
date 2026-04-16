/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/win/console.c
 * @brief Win32 raw-mode console for interactive tools (serial monitor).
 *
 * Switches the console input to character-at-a-time, no-echo mode
 * with Ctrl-C processing disabled so that control characters arrive
 * as regular key events.
 */
#include <errno.h>
#include <stdlib.h>
#include <windows.h>

#include "platform.h"

static DWORD saved_mode;
static int raw_active;

static void restore_on_exit(void)
{
	if (raw_active)
		console_raw_leave();
}

int console_raw_enter(void)
{
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	DWORD mode;

	if (h == INVALID_HANDLE_VALUE)
		return -EIO;

	if (!GetConsoleMode(h, &saved_mode))
		return -ENOTTY;

	mode = saved_mode;
	mode &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
			 ENABLE_PROCESSED_INPUT);

	if (!SetConsoleMode(h, mode))
		return -EIO;

	if (!raw_active) {
		atexit(restore_on_exit);
		raw_active = 1;
	}

	return 0;
}

void console_raw_leave(void)
{
	HANDLE h;

	if (!raw_active)
		return;

	h = GetStdHandle(STD_INPUT_HANDLE);
	if (h != INVALID_HANDLE_VALUE)
		SetConsoleMode(h, saved_mode);
	raw_active = 0;
}

ssize_t console_read(void *buf, size_t n, unsigned timeout_ms)
{
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	unsigned char *p = (unsigned char *)buf;
	size_t got = 0;
	DWORD rc;

	rc = WaitForSingleObject(h, timeout_ms);
	if (rc == WAIT_TIMEOUT)
		return 0;
	if (rc != WAIT_OBJECT_0)
		return -1;

	while (got < n) {
		INPUT_RECORD rec;
		DWORD count;

		if (!PeekConsoleInput(h, &rec, 1, &count) || count == 0)
			break;

		ReadConsoleInput(h, &rec, 1, &count);

		if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
			continue;

		if (rec.Event.KeyEvent.uChar.AsciiChar == 0)
			continue;

		p[got++] = (unsigned char)rec.Event.KeyEvent.uChar.AsciiChar;
	}

	return (ssize_t)got;
}
