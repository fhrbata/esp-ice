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
