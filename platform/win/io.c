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

/**
 * @brief Skip sub-parameters of an extended color sequence.
 *
 * Called when the SGR parser sees code 38 (fg) or 48 (bg).
 * Peeks at the next parameter: if 5, skips 1 more (256-color);
 * if 2, skips 3 more (RGB).  Returns the updated pointer.
 */
static const char *skip_extended_color(const char *p)
{
	int sub, skip, i;

	if (*p != ';')
		return p;
	p++;

	/* Parse the sub-command: 5 = 256-color, 2 = RGB */
	sub = 0;
	while (*p >= '0' && *p <= '9')
		sub = sub * 10 + (*p++ - '0');

	if (sub == 5)
		skip = 1; /* 38;5;N */
	else if (sub == 2)
		skip = 3; /* 38;2;R;G;B */
	else
		return p;

	for (i = 0; i < skip; i++) {
		if (*p != ';')
			break;
		p++;
		while (*p >= '0' && *p <= '9')
			p++;
	}
	return p;
}

/**
 * @brief Write a UTF-8 string to a console, translating ANSI escape
 * codes to Console API calls.
 *
 * Used on legacy Windows where VT processing is not available.
 * Text is converted to wide-char and written with WriteConsoleW;
 * ESC[...m sequences are translated to SetConsoleTextAttribute.
 */
static int console_write_legacy(HANDLE h, const char *buf)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	WORD defattr, attr;
	const char *p = buf;
	const char *text_start = buf;
	int total = 0;

	if (!GetConsoleScreenBufferInfo(h, &csbi))
		return -1;

	defattr = csbi.wAttributes;
	attr = defattr;

	while (*p) {
		if (p[0] == '\033' && p[1] == '[') {
			/* Flush text before the escape sequence. */
			if (p > text_start) {
				wchar_t *ws =
				    mbs_to_wcs_n(text_start, p - text_start);
				if (ws) {
					WriteConsoleW(h, ws, (DWORD)wcslen(ws),
						      NULL, NULL);
					total += (int)(p - text_start);
					free(ws);
				}
			}

			/* Parse SGR parameters: ESC[ n1;n2;...m */
			p += 2;
			while (*p && *p != 'm') {
				int code = 0;
				while (*p >= '0' && *p <= '9')
					code = code * 10 + (*p++ - '0');
				if (code == 38 || code == 48)
					p = skip_extended_color(p);
				else
					attr =
					    ansi_to_attr(attr, code, defattr);
				if (*p == ';')
					p++;
			}
			if (*p == 'm')
				p++;

			SetConsoleTextAttribute(h, attr);
			text_start = p;
			continue;
		}
		p++;
	}

	/* Flush remaining text. */
	if (p > text_start) {
		wchar_t *ws = mbs_to_wcs_n(text_start, p - text_start);
		if (ws) {
			WriteConsoleW(h, ws, (DWORD)wcslen(ws), NULL, NULL);
			total += (int)(p - text_start);
			free(ws);
		}
	}

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
 * Handles three cases:
 *  - Redirected (pipe/file): expand colors, format, fwrite.
 *  - Console with VT: expand colors, format, WriteConsoleW.
 *  - Console without VT: expand colors, format, parse ANSI → Console API.
 */
int vfprintf_p(FILE *stream, const char *fmt, va_list args)
{
	struct sbuf expanded = SBUF_INIT;
	struct sbuf output = SBUF_INIT;
	int n;
	va_list args_copy;
	DWORD dwMode;

	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	HANDLE h = (HANDLE)_get_osfhandle(_fileno(stream));

	expand_colors(&expanded, fmt, use_color_for(stream));

	/* Redirected: format and fwrite. */
	if (!GetConsoleMode(h, &dwMode)) {
		va_copy(args_copy, args);
		sbuf_vaddf(&output, expanded.buf, args_copy);
		va_end(args_copy);
		n = (int)fwrite(output.buf, 1, output.len, stream);
		if ((size_t)n != output.len)
			n = -1;
		goto out;
	}

	/* Console: format, then write with UTF-8 → wide-char. */
	va_copy(args_copy, args);
	sbuf_vaddf(&output, expanded.buf, args_copy);
	va_end(args_copy);

	if (use_vt)
		n = console_write_vt(h, output.buf);
	else
		n = console_write_legacy(h, output.buf);

out:
	sbuf_release(&output);
	sbuf_release(&expanded);
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
 */
int access_w(const char *fn, int mode)
{
	wchar_t *wfn = mbs_to_wcs(fn);
	int rv;

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
