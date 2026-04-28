/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/win/term.c
 * @brief Win32 terminal primitives -- raw mode, size, alt screen.
 *
 * Presents a uniform byte stream to the portable @ref term_read_event
 * decoder in @c term.c regardless of Windows version.  Every KEY_EVENT
 * delivered by @c ReadConsoleInput is translated into the same ANSI
 * escape sequences an xterm / VT220 terminal would produce, so the
 * root TUI code never has to branch on the OS.  WINDOW_BUFFER_SIZE_EVENT
 * records set a flag polled by @ref term_resize_pending.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "platform.h"
#include "term.h"

static DWORD saved_mode;
static int raw_active;
static int screen_active;
static volatile LONG resize_flag;

/*
 * Ring-buffered translator output.  One console @c KEY_EVENT can expand
 * to up to ~4 bytes (e.g. @c "\x1b[24~" for F12).  term_read() callers
 * may ask for as little as 1 byte at a time, so the overflow stays
 * parked here until they come back for it.  Single-threaded access
 * matches the rest of the ice codebase.
 */
#define PENDING_CAP 32
static unsigned char pending[PENDING_CAP];
static size_t pending_head; /* next byte to read */
static size_t pending_tail; /* next slot to write */

static void pending_push(const char *bytes, size_t n)
{
	for (size_t i = 0; i < n && pending_tail < PENDING_CAP; i++)
		pending[pending_tail++] = (unsigned char)bytes[i];
}

static size_t pending_drain(unsigned char *out, size_t n)
{
	size_t got = 0;
	while (got < n && pending_head < pending_tail)
		out[got++] = pending[pending_head++];
	if (pending_head == pending_tail)
		pending_head = pending_tail = 0;
	return got;
}

/*
 * VK -> ANSI escape sequence table.  Values match xterm's VT220
 * emulation so the portable decoder in term.c recognises them.
 *
 *   Arrows / Home / End use CSI form (ESC [ A..F).
 *   F1..F4 use SS3 form (ESC O P..S) to match xterm defaults.
 *   F5..F12 and navigation keys (PgUp/PgDn/Ins/Del) use CSI tilde
 *   sequences -- the vt220 numbering that all common terminals agree on.
 */
static const struct {
	WORD vk;
	const char *seq;
} vk_to_seq[] = {
    {VK_UP, "\x1b[A"},	    {VK_DOWN, "\x1b[B"},    {VK_RIGHT, "\x1b[C"},
    {VK_LEFT, "\x1b[D"},    {VK_HOME, "\x1b[H"},    {VK_END, "\x1b[F"},
    {VK_PRIOR, "\x1b[5~"}, /* Page Up */
    {VK_NEXT, "\x1b[6~"},  /* Page Down */
    {VK_INSERT, "\x1b[2~"}, {VK_DELETE, "\x1b[3~"}, {VK_F1, "\x1bOP"},
    {VK_F2, "\x1bOQ"},	    {VK_F3, "\x1bOR"},	    {VK_F4, "\x1bOS"},
    {VK_F5, "\x1b[15~"},    {VK_F6, "\x1b[17~"},    {VK_F7, "\x1b[18~"},
    {VK_F8, "\x1b[19~"},    {VK_F9, "\x1b[20~"},    {VK_F10, "\x1b[21~"},
    {VK_F11, "\x1b[23~"},   {VK_F12, "\x1b[24~"},
};

/*
 * Translate one @c KEY_EVENT into byte(s) and queue them on the ring.
 *
 * Priority:
 *   1. VK_BACK -> DEL (0x7f), matching the modern terminal convention
 *      TK_BACKSPACE expects.  Ctrl-H stays as 0x08.
 *   2. AsciiChar != 0 -> emit verbatim.  Windows pre-translates Ctrl-
 *      letter chords into the matching C0 control code (Ctrl-C = 0x03
 *      etc.), so this path covers printable keys and ctrl-letter
 *      chords without any extra bookkeeping.
 *   3. Lookup the virtual-key code in @ref vk_to_seq.
 *   4. Anything else (Shift-only, media keys, ...) is dropped.
 */
static void translate_key(const KEY_EVENT_RECORD *k)
{
	if (k->wVirtualKeyCode == VK_BACK) {
		pending_push("\x7f", 1);
		return;
	}
	if (k->uChar.AsciiChar != 0) {
		pending_push(&k->uChar.AsciiChar, 1);
		return;
	}
	for (size_t i = 0; i < sizeof(vk_to_seq) / sizeof(vk_to_seq[0]); i++) {
		if (vk_to_seq[i].vk == k->wVirtualKeyCode) {
			pending_push(vk_to_seq[i].seq,
				     strlen(vk_to_seq[i].seq));
			return;
		}
	}
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
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	DWORD mode;
	DWORD clear;

	if (h == INVALID_HANDLE_VALUE)
		return -EIO;

	if (!GetConsoleMode(h, &saved_mode))
		return -ENOTTY;

	mode = saved_mode;
	/*
	 * ENABLE_PROCESSED_INPUT is the Windows analogue of POSIX ISIG --
	 * with it on the console driver translates Ctrl-C into a CTRL_C_EVENT
	 * (delivered via SetConsoleCtrlHandler).  Strip it for a fully raw
	 * mode unless TERM_RAW_KEEP_SIG asks us to leave it in.
	 */
	clear = ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;
	if (!(flags & TERM_RAW_KEEP_SIG))
		clear |= ENABLE_PROCESSED_INPUT;
	mode &= ~clear;
	/*
	 * ENABLE_WINDOW_INPUT delivers WINDOW_BUFFER_SIZE_EVENT records we
	 * rely on for resize notification.  We deliberately do NOT enable
	 * ENABLE_VIRTUAL_TERMINAL_INPUT -- translate_key() synthesises the
	 * ANSI sequences ourselves, so the shim works identically on
	 * modern (Win10 1809+) and legacy consoles without mode detection.
	 */
	mode |= ENABLE_WINDOW_INPUT;

	if (!SetConsoleMode(h, mode))
		return -EIO;

	if (!raw_active) {
		atexit(restore_on_exit);
		raw_active = 1;
	}

	return 0;
}

void term_raw_leave(void)
{
	HANDLE h;

	if (!raw_active)
		return;

	h = GetStdHandle(STD_INPUT_HANDLE);
	if (h != INVALID_HANDLE_VALUE)
		SetConsoleMode(h, saved_mode);
	raw_active = 0;
}

ssize_t term_read(void *buf, size_t n, unsigned timeout_ms)
{
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	unsigned char *p = (unsigned char *)buf;
	size_t got;

	/*
	 * Drain any bytes left over from a prior call's translation first:
	 * the caller might ask for 1 byte at a time while we already split
	 * a 3-byte arrow-key sequence into the ring.
	 */
	got = pending_drain(p, n);
	if (got > 0)
		return (ssize_t)got;

	DWORD rc = WaitForSingleObject(h, timeout_ms);
	if (rc == WAIT_TIMEOUT)
		return 0;
	if (rc != WAIT_OBJECT_0)
		return -1;

	/*
	 * Translate every currently-available record into the ring, then
	 * drain what fits in @p buf.  Anything that overflows stays in
	 * @c pending for the next call -- crucial for 1-byte reads.
	 */
	for (;;) {
		INPUT_RECORD rec;
		DWORD count;

		if (!PeekConsoleInput(h, &rec, 1, &count) || count == 0)
			break;

		ReadConsoleInput(h, &rec, 1, &count);

		if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
			InterlockedExchange(&resize_flag, 1);
			continue;
		}

		if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
			continue;

		translate_key(&rec.Event.KeyEvent);
		/* Stop accumulating once we have enough for this caller;
		 * the rest waits in pending / the console queue. */
		if (pending_tail - pending_head >= n)
			break;
	}

	return (ssize_t)pending_drain(p, n);
}

int term_size(int *cols, int *rows)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);

	if (h == INVALID_HANDLE_VALUE)
		return -EIO;
	if (!GetConsoleScreenBufferInfo(h, &info))
		return -EIO;
	if (cols)
		*cols = info.srWindow.Right - info.srWindow.Left + 1;
	if (rows)
		*rows = info.srWindow.Bottom - info.srWindow.Top + 1;
	return 0;
}

int term_resize_pending(void)
{
	return InterlockedExchange(&resize_flag, 0) ? 1 : 0;
}

void term_screen_enter(void)
{
	if (screen_active)
		return;
	/* DEC alt-screen + hide cursor.  Requires VT processing on the
	 * output handle, enabled at startup by setup_io() when available
	 * (Windows 10 1511+).  A legacy-conhost output shim that parses
	 * these sequences into Console API calls is a future item -- for
	 * now callers gate full TUI use on use_vt. */
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
