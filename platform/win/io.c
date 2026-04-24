/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/win/io.c
 * @brief Color-aware, UTF-8-aware I/O overrides for Windows.
 *
 * Implements fprintf_p/vfprintf_p/fputs_p (activated via macros in
 * platform.h) which handle @x{...} color token expansion, UTF-8 to
 * wide-char conversion for console output, and ANSI-to-Console-API
 * fallback on legacy Windows without VT processing support.
 *
 * Also provides fopen_w, access_w, and mkdir_w for UTF-8 file paths.
 */
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "ice.h"
#include "wconv.h"

/**
 * @brief Map an ANSI SGR code to a Windows console text attribute.
 *
 * Handles all single-parameter SGR codes that the Console API can
 * express.  Extended color sequences (38;5;N, 48;5;N, 38;2;R;G;B,
 * 48;2;R;G;B) are handled by the caller (skip_extended_color).
 */
static WORD ansi_to_attr(WORD attr, int code, WORD defattr)
{
	/* Foreground color table: index 0-7 maps SGR 30-37 and 90-97. */
	static const WORD fg[] = {
	    0,					/* 0: black */
	    FOREGROUND_RED,			/* 1: red */
	    FOREGROUND_GREEN,			/* 2: green */
	    FOREGROUND_RED | FOREGROUND_GREEN,	/* 3: yellow */
	    FOREGROUND_BLUE,			/* 4: blue */
	    FOREGROUND_RED | FOREGROUND_BLUE,	/* 5: magenta */
	    FOREGROUND_GREEN | FOREGROUND_BLUE, /* 6: cyan */
	    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE, /* 7: white */
	};

	switch (code) {
	case 0:
		return defattr; /* reset */
	case 1:
		return attr | FOREGROUND_INTENSITY; /* bold */
	case 4:
		return attr | COMMON_LVB_UNDERSCORE; /* underline */
	case 7:
		return attr | COMMON_LVB_REVERSE_VIDEO; /* reverse */
	case 22:
		return attr & ~FOREGROUND_INTENSITY; /* normal */
	case 24:
		return attr & ~(WORD)COMMON_LVB_UNDERSCORE; /* no underline */
	case 27:
		return attr & ~(WORD)COMMON_LVB_REVERSE_VIDEO; /* no reverse */
	case 39:
		return (attr & ~0x0F) | (defattr & 0x0F); /* default fg */
	case 49:
		return (attr & ~0xF0) | (defattr & 0xF0); /* default bg */
	}

	/* Foreground: 30-37 */
	if (code >= 30 && code <= 37)
		return (attr & ~0x07) | fg[code - 30];
	/* Background: 40-47 */
	if (code >= 40 && code <= 47)
		return (attr & ~0x70) | (fg[code - 40] << 4);
	/* Bright foreground: 90-97 */
	if (code >= 90 && code <= 97)
		return (attr & ~0x0F) | fg[code - 90] | FOREGROUND_INTENSITY;
	/* Bright background: 100-107 */
	if (code >= 100 && code <= 107)
		return (attr & ~0xF0) | (fg[code - 100] << 4) |
		       BACKGROUND_INTENSITY;

	return attr; /* unknown code: ignore */
}

/* ================================================================== */
/*  ANSI -> Console API shim (legacy-conhost output path)             */
/* ================================================================== */

/*
 * Module state for alt-screen emulation.  Kept at file scope because
 * the ANSI stream enters / leaves alt-screen mid-write and the caller
 * can issue a plain fprintf afterwards expecting to hit the same
 * buffer.
 *
 * When @c alt_active is set, every @c console_write_legacy call writes
 * to @c alt_buffer instead of the inherited stdout handle, which lets
 * primary-screen output that happens concurrently (e.g. from a
 * background thread) stay on the primary buffer while the TUI owns
 * the alt one.  We never leak the alt buffer past process exit:
 * @c SetConsoleActiveScreenBuffer is restored by @c restore_alt_screen
 * when @c ESC[?1049l is seen or at atexit via term_screen_leave.
 */
static HANDLE alt_buffer = INVALID_HANDLE_VALUE;
static HANDLE saved_primary; /* output handle active before enter */
static int alt_active;

/*
 * Saved-cursor state for ESC[s / ESC[u.  Only one nesting level --
 * matches DEC VT100 behaviour.
 */
static COORD saved_cursor;
static int saved_cursor_valid;

static void enter_alt_screen(HANDLE primary)
{
	CONSOLE_SCREEN_BUFFER_INFO info;

	if (alt_active)
		return;
	if (alt_buffer == INVALID_HANDLE_VALUE) {
		alt_buffer = CreateConsoleScreenBuffer(
		    GENERIC_READ | GENERIC_WRITE,
		    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		    CONSOLE_TEXTMODE_BUFFER, NULL);
		if (alt_buffer == INVALID_HANDLE_VALUE)
			return;
	}
	/* Match the primary buffer's visible window size so cursor-
	 * addressing math lines up. */
	if (GetConsoleScreenBufferInfo(primary, &info))
		SetConsoleScreenBufferSize(alt_buffer, info.dwSize);

	saved_primary = primary;
	SetConsoleActiveScreenBuffer(alt_buffer);
	alt_active = 1;
}

static void leave_alt_screen(void)
{
	if (!alt_active)
		return;
	SetConsoleActiveScreenBuffer(saved_primary);
	alt_active = 0;
	/* Keep @c alt_buffer around: re-entering alt-screen is common in
	 * TUI sessions (menuconfig -> shell escape -> back). */
}

/*
 * CSI dispatch helpers.  Each takes the active output handle and the
 * parsed numeric parameters (0-filled; @p n_params counts how many the
 * sequence actually carried).  Sequences with no parameters fall back
 * to the VT100 default value encoded by the caller via @p *_default.
 */

static void csi_cursor_position(HANDLE h, const int *params, int n_params)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	int row = n_params >= 1 && params[0] > 0 ? params[0] : 1;
	int col = n_params >= 2 && params[1] > 0 ? params[1] : 1;
	COORD pos;

	if (!GetConsoleScreenBufferInfo(h, &info))
		return;
	/* VT100 positions are 1-based in the visible window; translate to
	 * the absolute buffer coordinates Console API wants. */
	pos.X = (SHORT)(info.srWindow.Left + col - 1);
	pos.Y = (SHORT)(info.srWindow.Top + row - 1);
	SetConsoleCursorPosition(h, pos);
}

static void csi_cursor_move(HANDLE h, char final, const int *params,
			    int n_params)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	int n = n_params >= 1 && params[0] > 0 ? params[0] : 1;
	COORD pos;

	if (!GetConsoleScreenBufferInfo(h, &info))
		return;
	pos = info.dwCursorPosition;
	switch (final) {
	case 'A':
		pos.Y = (SHORT)(pos.Y - n);
		break;
	case 'B':
		pos.Y = (SHORT)(pos.Y + n);
		break;
	case 'C':
		pos.X = (SHORT)(pos.X + n);
		break;
	case 'D':
		pos.X = (SHORT)(pos.X - n);
		break;
	}
	/* Clamp to window bounds -- terminals wrap-clip, not wrap-advance. */
	if (pos.X < info.srWindow.Left)
		pos.X = info.srWindow.Left;
	if (pos.X > info.srWindow.Right)
		pos.X = info.srWindow.Right;
	if (pos.Y < info.srWindow.Top)
		pos.Y = info.srWindow.Top;
	if (pos.Y > info.srWindow.Bottom)
		pos.Y = info.srWindow.Bottom;
	SetConsoleCursorPosition(h, pos);
}

/* Erase @p len cells starting at @p from using the default attribute. */
static void fill_cells(HANDLE h, COORD from, DWORD len, WORD attr)
{
	DWORD wrote;
	FillConsoleOutputCharacterW(h, L' ', len, from, &wrote);
	FillConsoleOutputAttribute(h, attr, len, from, &wrote);
}

static void csi_erase_in_line(HANDLE h, const int *params, int n_params)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	int mode = n_params >= 1 ? params[0] : 0;
	COORD from;
	DWORD len;

	if (!GetConsoleScreenBufferInfo(h, &info))
		return;
	switch (mode) {
	case 0: /* cursor -> end of line */
		from = info.dwCursorPosition;
		len = info.srWindow.Right - info.dwCursorPosition.X + 1;
		break;
	case 1: /* start of line -> cursor */
		from.X = info.srWindow.Left;
		from.Y = info.dwCursorPosition.Y;
		len = info.dwCursorPosition.X - info.srWindow.Left + 1;
		break;
	case 2: /* entire line */
		from.X = info.srWindow.Left;
		from.Y = info.dwCursorPosition.Y;
		len = info.srWindow.Right - info.srWindow.Left + 1;
		break;
	default:
		return;
	}
	fill_cells(h, from, len, info.wAttributes);
}

static void csi_erase_in_display(HANDLE h, const int *params, int n_params)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	int mode = n_params >= 1 ? params[0] : 0;
	COORD from;
	DWORD len;
	int w, rows_to_bottom;

	if (!GetConsoleScreenBufferInfo(h, &info))
		return;
	w = info.srWindow.Right - info.srWindow.Left + 1;
	switch (mode) {
	case 0: /* cursor -> end of screen */
		/* Rest of current line... */
		from = info.dwCursorPosition;
		fill_cells(h, from,
			   info.srWindow.Right - info.dwCursorPosition.X + 1,
			   info.wAttributes);
		/* ...then all following lines. */
		from.X = info.srWindow.Left;
		from.Y = (SHORT)(info.dwCursorPosition.Y + 1);
		rows_to_bottom = info.srWindow.Bottom - info.dwCursorPosition.Y;
		if (rows_to_bottom > 0)
			fill_cells(h, from, (DWORD)w * (DWORD)rows_to_bottom,
				   info.wAttributes);
		break;
	case 1: /* start -> cursor */
		from.X = info.srWindow.Left;
		from.Y = info.srWindow.Top;
		rows_to_bottom = info.dwCursorPosition.Y - info.srWindow.Top;
		if (rows_to_bottom > 0)
			fill_cells(h, from, (DWORD)w * (DWORD)rows_to_bottom,
				   info.wAttributes);
		from.Y = info.dwCursorPosition.Y;
		fill_cells(h, from,
			   info.dwCursorPosition.X - info.srWindow.Left + 1,
			   info.wAttributes);
		break;
	case 2: /* entire screen */
	case 3: /* screen + scrollback -- we just do screen */
		from.X = info.srWindow.Left;
		from.Y = info.srWindow.Top;
		len = (DWORD)w *
		      (DWORD)(info.srWindow.Bottom - info.srWindow.Top + 1);
		fill_cells(h, from, len, info.wAttributes);
		break;
	default:
		return;
	}
}

static void csi_cursor_visible(HANDLE h, int visible)
{
	CONSOLE_CURSOR_INFO ci;
	if (!GetConsoleCursorInfo(h, &ci))
		return;
	ci.bVisible = visible ? TRUE : FALSE;
	SetConsoleCursorInfo(h, &ci);
}

static void csi_save_cursor(HANDLE h)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	if (!GetConsoleScreenBufferInfo(h, &info))
		return;
	saved_cursor = info.dwCursorPosition;
	saved_cursor_valid = 1;
}

static void csi_restore_cursor(HANDLE h)
{
	if (!saved_cursor_valid)
		return;
	SetConsoleCursorPosition(h, saved_cursor);
}

static void csi_apply_sgr(HANDLE h, WORD *attr, WORD defattr, const int *params,
			  int n_params)
{
	/* No params -> SGR 0 (reset). */
	if (n_params == 0) {
		*attr = defattr;
		SetConsoleTextAttribute(h, *attr);
		return;
	}
	/* We walk raw tokens here rather than the params[] array because
	 * extended color sub-sequences (38;5;N or 38;2;R;G;B) consume an
	 * unpredictable number of following numbers that the generic
	 * parser already treated as separate params.  Skipping them is
	 * easier than reconstructing the grouping, and no ice caller emits
	 * extended colors today. */
	for (int i = 0; i < n_params; i++) {
		int code = params[i];
		if (code == 38 || code == 48) {
			/* Next param is either 5 (256-color) or 2 (RGB);
			 * skip it plus its payload.  Default to 1-skip if
			 * we don't recognise the sub-command. */
			if (i + 1 < n_params && params[i + 1] == 5)
				i += 2;
			else if (i + 1 < n_params && params[i + 1] == 2)
				i += 4;
			else if (i + 1 < n_params)
				i += 1;
			continue;
		}
		*attr = ansi_to_attr(*attr, code, defattr);
	}
	SetConsoleTextAttribute(h, *attr);
}

/*
 * Private-mode set/reset: @c ESC[?<N>h and @c ESC[?<N>l.
 *
 * Handles the two modes the ice TUI actually emits:
 *   - @c 25  cursor visibility
 *   - @c 1049 alt-screen buffer
 *
 * Returns the new output handle (alt or primary) so the caller can
 * switch where subsequent text writes go.  @p h is the "current"
 * handle on entry; @p primary is the original stdout handle, used as
 * the destination when leaving alt-screen.
 */
static HANDLE csi_private_mode(HANDLE h, HANDLE primary, int set,
			       const int *params, int n_params)
{
	for (int i = 0; i < n_params; i++) {
		switch (params[i]) {
		case 25:
			csi_cursor_visible(h, set);
			break;
		case 1049:
			if (set) {
				enter_alt_screen(primary);
				h = alt_active ? alt_buffer : primary;
			} else {
				leave_alt_screen();
				h = primary;
			}
			break;
		default:
			/* Unknown private mode: ignore silently. */
			break;
		}
	}
	return h;
}

/*
 * Parse one CSI sequence starting at @p p (which points at the byte
 * after @c ESC[).  Fills @p params / @p n_params, records the
 * private-marker flag in @p *private, and returns a pointer to the
 * byte after the final char along with the final char itself.
 */
static const char *parse_csi(const char *p, int *private_marker, int *params,
			     int *n_params, int cap, char *final)
{
	*private_marker = 0;
	*n_params = 0;
	*final = 0;

	if (*p == '?') {
		*private_marker = 1;
		p++;
	}

	int have_digit = 0;
	int value = 0;
	while (*p) {
		if (*p >= '0' && *p <= '9') {
			value = value * 10 + (*p - '0');
			have_digit = 1;
			p++;
		} else if (*p == ';') {
			if (*n_params < cap)
				params[(*n_params)++] = have_digit ? value : 0;
			value = 0;
			have_digit = 0;
			p++;
		} else {
			break;
		}
	}
	if (have_digit && *n_params < cap)
		params[(*n_params)++] = value;

	if (*p) {
		*final = *p;
		p++;
	}
	return p;
}

static void flush_text(HANDLE h, const char *text, size_t len, int *total)
{
	if (len == 0)
		return;
	wchar_t *ws = mbs_to_wcs_n(text, len);
	if (ws) {
		WriteConsoleW(h, ws, (DWORD)wcslen(ws), NULL, NULL);
		*total += (int)len;
		free(ws);
	}
}

/**
 * @brief Write a UTF-8 string to a console, translating ANSI escape
 * codes to Console API calls.
 *
 * Used on legacy Windows where VT processing is not available.  Plain
 * text runs are forwarded to @c WriteConsoleW; CSI sequences are
 * parsed into @c (private?, params, final) triples and dispatched:
 *
 *   m           -> SGR (colour / attributes)
 *   H, f        -> cursor position
 *   A/B/C/D     -> cursor up / down / right / left
 *   K           -> erase in line
 *   J           -> erase in display
 *   s, u        -> save / restore cursor
 *   ?25h, ?25l  -> cursor visibility
 *   ?1049h, l   -> alt-screen enter / leave
 */
static int console_write_legacy(HANDLE h, const char *buf)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	WORD defattr, attr;
	HANDLE active = alt_active ? alt_buffer : h;
	const char *p = buf;
	const char *text_start = buf;
	int total = 0;

	if (!GetConsoleScreenBufferInfo(active, &csbi))
		return -1;

	defattr = csbi.wAttributes;
	attr = defattr;

	while (*p) {
		if (p[0] == '\033' && p[1] == '[') {
			flush_text(active, text_start, (size_t)(p - text_start),
				   &total);

			int params[16];
			int n_params;
			int private_marker;
			char final;
			p = parse_csi(p + 2, &private_marker, params, &n_params,
				      16, &final);

			if (private_marker) {
				if (final == 'h') {
					active = csi_private_mode(
					    active, h, 1, params, n_params);
				} else if (final == 'l') {
					active = csi_private_mode(
					    active, h, 0, params, n_params);
				}
				/* Private final chars other than h/l are
				 * ignored. */
			} else {
				switch (final) {
				case 'm':
					csi_apply_sgr(active, &attr, defattr,
						      params, n_params);
					break;
				case 'H':
				case 'f':
					csi_cursor_position(active, params,
							    n_params);
					break;
				case 'A':
				case 'B':
				case 'C':
				case 'D':
					csi_cursor_move(active, final, params,
							n_params);
					break;
				case 'K':
					csi_erase_in_line(active, params,
							  n_params);
					break;
				case 'J':
					csi_erase_in_display(active, params,
							     n_params);
					break;
				case 's':
					csi_save_cursor(active);
					break;
				case 'u':
					csi_restore_cursor(active);
					break;
				default:
					/* Unrecognised final byte -- drop it.
					 * Menuconfig doesn't emit anything
					 * exotic; this includes e.g. DECSCUSR
					 * cursor-style, which has no Console
					 * API equivalent anyway. */
					break;
				}
			}

			text_start = p;
			continue;
		}
		p++;
	}

	flush_text(active, text_start, (size_t)(p - text_start), &total);
	return total;
}

/**
 * @brief Write a UTF-8 string to a console with VT processing.
 *
 * Converts to wide-char and writes with WriteConsoleW. ANSI escape
 * codes are interpreted natively by the console.
 */
static int console_write_vt(HANDLE h, const char *buf)
{
	wchar_t *ws = mbs_to_wcs(buf);
	if (!ws)
		return -1;

	BOOL ok = WriteConsoleW(h, ws, (DWORD)wcslen(ws), NULL, NULL);
	int len = (int)strlen(buf);
	free(ws);
	return ok ? len : -1;
}

/**
 * @brief Write a formatted UTF-8 string to a stream.
 *
 * Substitutes conversion specifiers first, then expands @x{...}
 * tokens so that tokens arriving through %s arguments participate
 * in expansion and nesting the same way as tokens in the literal
 * format string.
 *
 * Handles three output cases:
 *  - Redirected (pipe/file): fwrite the expanded bytes.
 *  - Console with VT: WriteConsoleW (terminal interprets ANSI).
 *  - Console without VT: parse ANSI → Console API.
 */
int vfprintf_p(FILE *stream, const char *fmt, va_list args)
{
	struct sbuf formatted = SBUF_INIT;
	struct sbuf expanded = SBUF_INIT;
	const char *out_buf;
	size_t out_len;
	int n;
	DWORD dwMode;

	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	HANDLE h = (HANDLE)_get_osfhandle(_fileno(stream));

	sbuf_vaddf(&formatted, fmt, args);

	if (memchr(formatted.buf, '@', formatted.len)) {
		expand_colors(&expanded, formatted.buf, use_color_for(stream));
		out_buf = expanded.buf;
		out_len = expanded.len;
	} else {
		out_buf = formatted.buf;
		out_len = formatted.len;
	}

	if (!GetConsoleMode(h, &dwMode)) {
		n = (int)fwrite(out_buf, 1, out_len, stream);
		if ((size_t)n != out_len)
			n = -1;
		goto out;
	}

	if (use_vt)
		n = console_write_vt(h, out_buf);
	else
		n = console_write_legacy(h, out_buf);

out:
	sbuf_release(&expanded);
	sbuf_release(&formatted);
	return n;
}

/** Color-aware fprintf for Windows. Delegates to vfprintf_p(). */
int fprintf_p(FILE *stream, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int rv = vfprintf_p(stream, fmt, args);
	va_end(args);
	return rv;
}

/** Color-aware fputs for Windows. Expands @x{...} tokens. */
int fputs_p(const char *s, FILE *stream)
{
	struct sbuf expanded = SBUF_INIT;
	int n;
	DWORD dwMode;

	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	HANDLE h = (HANDLE)_get_osfhandle(_fileno(stream));

	expand_colors(&expanded, s, use_color_for(stream));

	if (!GetConsoleMode(h, &dwMode)) {
		n = (int)fwrite(expanded.buf, 1, expanded.len, stream);
		if ((size_t)n != expanded.len)
			n = -1;
		else
			n = 0;
	} else if (use_vt) {
		n = console_write_vt(h, expanded.buf) >= 0 ? 0 : -1;
	} else {
		n = console_write_legacy(h, expanded.buf) >= 0 ? 0 : -1;
	}

	sbuf_release(&expanded);
	return n;
}

int term_width(int fd)
{
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	CONSOLE_SCREEN_BUFFER_INFO csbi;

	if (GetConsoleScreenBufferInfo(h, &csbi))
		return csbi.srWindow.Right - csbi.srWindow.Left + 1;
	return 80;
}

/**
 * @brief UTF-8-aware fopen replacement for Windows.
 */
FILE *fopen_w(const char *fn, const char *mode)
{
	wchar_t *wfn = mbs_to_wcs(fn);
	wchar_t *wmode = mbs_to_wcs(mode);
	FILE *fp = NULL;

	if (!wfn || !wmode) {
		errno = ENOMEM;
		goto out;
	}

	fp = _wfopen(wfn, wmode);
out:
	free(wfn);
	free(wmode);
	return fp;
}

/**
 * @brief UTF-8-aware access() replacement for Windows.
 *
 * The CRT's _waccess does not implement X_OK; we approximate it by
 * checking whether the file exists either at @p fn or with one of the
 * common PATHEXT extensions appended (@c .exe, @c .cmd, @c .bat).
 * That covers everything users put in @c core.pager or @c ice-<name>
 * in practice.  Other mode bits are forwarded unchanged.
 */
int access_w(const char *fn, int mode)
{
	wchar_t *wfn;
	int rv;

	if (mode & X_OK) {
		static const char *const exts[] = {"", ".exe", ".cmd", ".bat",
						   NULL};
		struct sbuf sb = SBUF_INIT;
		int found = 0;

		for (int i = 0; exts[i]; i++) {
			sbuf_reset(&sb);
			sbuf_addstr(&sb, fn);
			sbuf_addstr(&sb, exts[i]);
			wfn = mbs_to_wcs(sb.buf);
			if (!wfn)
				continue;
			if (_waccess(wfn, F_OK) == 0)
				found = 1;
			free(wfn);
			if (found)
				break;
		}
		sbuf_release(&sb);
		return found ? 0 : -1;
	}

	wfn = mbs_to_wcs(fn);
	if (!wfn) {
		errno = ENOMEM;
		return -1;
	}

	rv = _waccess(wfn, mode);
	free(wfn);

	return rv;
}

/**
 * @brief UTF-8-aware mkdir() replacement for Windows.
 */
int mkdir_w(const char *path, int mode)
{
	wchar_t *wpath = mbs_to_wcs(path);
	int rv;
	(void)mode;

	if (!wpath) {
		errno = ENOMEM;
		return -1;
	}

	rv = _wmkdir(wpath);
	free(wpath);

	return rv;
}

int unlink_w(const char *path)
{
	wchar_t *wpath = mbs_to_wcs(path);
	int rv;

	if (!wpath) {
		errno = ENOMEM;
		return -1;
	}

	rv = _wunlink(wpath);
	free(wpath);
	return rv;
}

int rmdir_w(const char *path)
{
	wchar_t *wpath = mbs_to_wcs(path);
	int rv;

	if (!wpath) {
		errno = ENOMEM;
		return -1;
	}

	rv = _wrmdir(wpath);
	free(wpath);
	return rv;
}

/**
 * @brief UTF-8-aware open() replacement for Windows.
 *
 * The three-argument form (flags + mode) covers the O_CREAT cases we
 * need for lockfiles and atomic updates.  Callers that don't need
 * O_CREAT still pass a mode argument -- _wopen ignores it when
 * O_CREAT is absent.
 */
int open_w(const char *path, int flags, int mode)
{
	wchar_t *wpath = mbs_to_wcs(path);
	int rv;

	if (!wpath) {
		errno = ENOMEM;
		return -1;
	}

	rv = _wopen(wpath, flags, mode);
	free(wpath);
	return rv;
}

/**
 * @brief UTF-8-aware chdir() replacement for Windows.
 */
int chdir_w(const char *path)
{
	wchar_t *wpath = mbs_to_wcs(path);
	int rv;

	if (!wpath) {
		errno = ENOMEM;
		return -1;
	}

	rv = _wchdir(wpath);
	free(wpath);
	return rv;
}

/**
 * @brief chmod() no-op for Windows.
 *
 * The Win32 CRT's _chmod only honours read/write bits and does not
 * know about POSIX execute permissions at all.  Callers of chmod()
 * in tar extraction and similar are preserving POSIX mode bits that
 * have no meaningful Win32 equivalent, so the shim is intentionally
 * a no-op returning success -- matching the same "graceful decay"
 * policy as symlink_w().
 */
int chmod_w(const char *path, int mode)
{
	(void)path;
	(void)mode;
	return 0;
}

/**
 * @brief UTF-8-aware getcwd() replacement for Windows.
 *
 * The CRT's _getcwd encodes the result in the system ANSI code page;
 * we need UTF-8 to match the rest of the codebase.  Call _wgetcwd and
 * convert to UTF-8 via wcs_to_mbs.  Supports the POSIX-conventional
 * caller-provided buffer form (the NULL-buf allocating extension is
 * not needed by any ice call site).
 */
char *getcwd_w(char *buf, size_t size)
{
	wchar_t wbuf[4096];
	char *utf8;
	size_t len;

	if (!_wgetcwd(wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0]))))
		return NULL;

	utf8 = wcs_to_mbs(wbuf);
	if (!utf8) {
		errno = ENOMEM;
		return NULL;
	}

	len = strlen(utf8);
	if (len + 1 > size) {
		free(utf8);
		errno = ERANGE;
		return NULL;
	}

	memcpy(buf, utf8, len + 1);
	free(utf8);
	return buf;
}

/*
 * Atomic-replace rename: POSIX rename() already replaces an existing
 * target atomically, but the Windows CRT rename() fails with EEXIST.
 * Use MoveFileExW with MOVEFILE_REPLACE_EXISTING to match POSIX
 * semantics, so write-to-tmp + rename idioms work on both sides.
 */
int rename_w(const char *oldp, const char *newp)
{
	wchar_t *wold = mbs_to_wcs(oldp);
	wchar_t *wnew = mbs_to_wcs(newp);
	int rv = -1;

	if (!wold || !wnew) {
		errno = ENOMEM;
		goto out;
	}

	if (MoveFileExW(wold, wnew, MOVEFILE_REPLACE_EXISTING))
		rv = 0;
	else
		errno = EIO;

out:
	free(wold);
	free(wnew);
	return rv;
}

int is_directory(const char *path)
{
	wchar_t *wpath = mbs_to_wcs(path);
	DWORD attr;

	if (!wpath)
		return 0;

	attr = GetFileAttributesW(wpath);
	free(wpath);

	if (attr == INVALID_FILE_ATTRIBUTES)
		return 0;
	return (attr & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

/*
 * Symlinks on Windows require SeCreateSymbolicLinkPrivilege (admin)
 * or Developer Mode.  Espressif Windows toolchain archives don't use
 * them -- Unix archives' gcc → cc symlinks become separate .exe files
 * on Windows -- so this is a no-op rather than a failure.
 */
int symlink_w(const char *target, const char *linkpath)
{
	(void)target;
	(void)linkpath;
	return 0;
}

int link_w(const char *target, const char *linkpath)
{
	wchar_t *wtarget = mbs_to_wcs(target);
	wchar_t *wlink = mbs_to_wcs(linkpath);
	int rv = -1;

	if (!wtarget || !wlink) {
		errno = ENOMEM;
		goto out;
	}

	if (CreateHardLinkW(wlink, wtarget, NULL))
		rv = 0;
	else
		errno = EIO;

out:
	free(wtarget);
	free(wlink);
	return rv;
}

/*
 * UTF-8-aware setenv() for Windows.  The CRT's narrow _putenv_s takes
 * the current ANSI code page, so non-ASCII bytes in a UTF-8 value would
 * be mangled before entering the environment block (where Win32 stores
 * them as UTF-16).  Convert both name and value to wide-char first,
 * then call _wputenv_s which writes the environment natively.
 *
 * _wputenv_s copies its inputs, matching POSIX setenv semantics (no
 * lifetime requirement on the source strings).  The overwrite flag is
 * emulated: _wputenv_s always overwrites, so we check _wgetenv first
 * when the caller asks to preserve an existing value.
 */
int setenv_w(const char *name, const char *value, int overwrite)
{
	wchar_t *wname = mbs_to_wcs(name);
	wchar_t *wvalue = mbs_to_wcs(value ? value : "");
	int rv = -1;

	if (!wname || !wvalue) {
		errno = ENOMEM;
		goto out;
	}

	if (!overwrite && _wgetenv(wname)) {
		rv = 0;
		goto out;
	}

	rv = _wputenv_s(wname, wvalue) == 0 ? 0 : -1;
out:
	free(wname);
	free(wvalue);
	return rv;
}

int dir_foreach(const char *path, int (*cb)(const char *name, void *ud),
		void *ud)
{
	struct sbuf pattern = SBUF_INIT;
	wchar_t *wpattern;
	WIN32_FIND_DATAW fd;
	HANDLE h;
	struct svec names = SVEC_INIT;
	int rc = 0;

	sbuf_addf(&pattern, "%s\\*", path);
	wpattern = mbs_to_wcs(pattern.buf);
	sbuf_release(&pattern);
	if (!wpattern)
		return -1;

	h = FindFirstFileW(wpattern, &fd);
	free(wpattern);
	if (h == INVALID_HANDLE_VALUE)
		return -1;

	do {
		char *utf8;

		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
			continue;

		utf8 = wcs_to_mbs(fd.cFileName);
		if (utf8) {
			svec_push(&names, utf8);
			free(utf8);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);

	for (size_t i = 0; i < names.nr; i++) {
		rc = cb(names.v[i], ud);
		if (rc)
			break;
	}
	svec_clear(&names);
	return rc;
}
