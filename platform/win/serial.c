/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/win/serial.c
 * @brief Win32 Comm API implementation of @ref serial.h.
 *
 * Uses CreateFileA for device open, SetCommState / SetCommTimeouts
 * for configuration, ReadFile / WriteFile for I/O, and
 * EscapeCommFunction for DTR / RTS control.  COM port paths must be
 * in the "\\\\.\\COMn" form for port numbers > 9; callers on the CLI
 * may type plain "COM3" which is accepted as-is by CreateFileA for
 * low-numbered ports.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "serial.h"

struct serial {
	HANDLE handle;
	unsigned baud;
	char path[256];
};

int serial_open(struct serial **out, const char *path)
{
	struct serial *s;
	HANDLE h;
	DCB dcb;
	COMMTIMEOUTS cto;

	*out = NULL;

	h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();
		return (err == ERROR_FILE_NOT_FOUND) ? -ENOENT : -EIO;
	}

	memset(&dcb, 0, sizeof dcb);
	dcb.DCBlength = sizeof dcb;
	if (!GetCommState(h, &dcb)) {
		CloseHandle(h);
		return -EIO;
	}

	dcb.BaudRate = CBR_115200;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary = TRUE;
	dcb.fDtrControl = DTR_CONTROL_DISABLE;
	dcb.fRtsControl = RTS_CONTROL_DISABLE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDsrSensitivity = FALSE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	dcb.fErrorChar = FALSE;
	dcb.fNull = FALSE;
	dcb.fAbortOnError = FALSE;

	if (!SetCommState(h, &dcb)) {
		CloseHandle(h);
		return -EIO;
	}

	/* Non-blocking reads: ReadFile returns immediately with
	 * whatever is available.  serial_read implements timeouts on
	 * top via ReadTotalTimeoutConstant. */
	memset(&cto, 0, sizeof cto);
	cto.ReadIntervalTimeout = MAXDWORD;
	cto.ReadTotalTimeoutMultiplier = 0;
	cto.ReadTotalTimeoutConstant = 0;
	cto.WriteTotalTimeoutMultiplier = 0;
	cto.WriteTotalTimeoutConstant = 0;

	if (!SetCommTimeouts(h, &cto)) {
		CloseHandle(h);
		return -EIO;
	}

	s = calloc(1, sizeof(*s));
	if (!s) {
		CloseHandle(h);
		return -ENOMEM;
	}

	s->handle = h;
	s->baud = 115200;
	strncpy(s->path, path, sizeof s->path - 1);
	*out = s;
	return 0;
}

void serial_close(struct serial *s)
{
	if (!s)
		return;
	if (s->handle != INVALID_HANDLE_VALUE)
		CloseHandle(s->handle);
	free(s);
}

int serial_set_baud(struct serial *s, unsigned baud)
{
	DCB dcb;

	memset(&dcb, 0, sizeof dcb);
	dcb.DCBlength = sizeof dcb;
	if (!GetCommState(s->handle, &dcb))
		return -EIO;

	dcb.BaudRate = (DWORD)baud;
	if (!SetCommState(s->handle, &dcb))
		return -EIO;

	s->baud = baud;
	return 0;
}

ssize_t serial_read(struct serial *s, void *buf, size_t n, unsigned timeout_ms)
{
	COMMTIMEOUTS cto;
	DWORD got = 0;

	memset(&cto, 0, sizeof cto);
	if (timeout_ms == 0) {
		/* Non-blocking: return immediately. */
		cto.ReadIntervalTimeout = MAXDWORD;
	} else {
		cto.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
	}

	if (!SetCommTimeouts(s->handle, &cto))
		return -1;
	if (!ReadFile(s->handle, buf, (DWORD)n, &got, NULL))
		return -1;

	return (ssize_t)got;
}

ssize_t serial_write(struct serial *s, const void *buf, size_t n)
{
	const char *p = (const char *)buf;
	size_t total = 0;

	while (total < n) {
		DWORD wrote = 0;

		if (!WriteFile(s->handle, p + total, (DWORD)(n - total), &wrote,
			       NULL))
			return -1;
		total += (size_t)wrote;
	}
	return (ssize_t)total;
}

int serial_set_dtr(struct serial *s, int on)
{
	DWORD func = on ? SETDTR : CLRDTR;

	if (!EscapeCommFunction(s->handle, func))
		return -EIO;
	return 0;
}

int serial_set_rts(struct serial *s, int on)
{
	DWORD func = on ? SETRTS : CLRRTS;

	if (!EscapeCommFunction(s->handle, func))
		return -EIO;
	return 0;
}

int serial_flush_input(struct serial *s)
{
	if (!PurgeComm(s->handle, PURGE_RXCLEAR))
		return -EIO;
	return 0;
}
