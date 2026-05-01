/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/win/process.c
 * @brief Windows child process API and self-path resolution.
 *
 * process_start() / process_finish() use CreateProcessW() /
 * WaitForSingleObject() with optional pipe redirection.  process_exe()
 * resolves the path of the running binary via GetModuleFileNameW +
 * UTF-8 conversion, caching the result on first call.
 */
#include <errhandlingapi.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <processenv.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>
#include <winerror.h>

/* ConPTY (Windows 10 1809+).  Symbols are runtime-resolved so the
 * binary still loads on older Windows; pty mode just fails cleanly. */
#include <consoleapi.h>

#include "ice.h"
#include "wconv.h"

/* ConPTY function pointers, lazily resolved via GetProcAddress.  Set to
 * NULL and stays NULL on Windows < 1809; @ref process_start checks
 * @ref conpty_available before allocating a pty. */
typedef HRESULT(WINAPI *create_pty_fn)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef HRESULT(WINAPI *resize_pty_fn)(HPCON, COORD);
typedef void(WINAPI *close_pty_fn)(HPCON);

static create_pty_fn p_CreatePseudoConsole;
static resize_pty_fn p_ResizePseudoConsole;
static close_pty_fn p_ClosePseudoConsole;
static int conpty_inited;

static int conpty_available(void)
{
	if (conpty_inited)
		return p_CreatePseudoConsole != NULL;
	HMODULE k = GetModuleHandleW(L"kernel32.dll");
	if (k) {
		p_CreatePseudoConsole =
		    (create_pty_fn)GetProcAddress(k, "CreatePseudoConsole");
		p_ResizePseudoConsole =
		    (resize_pty_fn)GetProcAddress(k, "ResizePseudoConsole");
		p_ClosePseudoConsole =
		    (close_pty_fn)GetProcAddress(k, "ClosePseudoConsole");
	}
	conpty_inited = 1;
	return p_CreatePseudoConsole && p_ResizePseudoConsole &&
	       p_ClosePseudoConsole;
}

/**
 * @brief Build a wide-char command line from a NULL-terminated argv.
 *
 * Quotes arguments that contain spaces. Returns a malloc'd string
 * that the caller must free.
 */
static wchar_t *build_cmdline(const char **argv)
{
	/* First pass: compute total length needed. */
	size_t len = 0;
	for (int i = 0; argv[i]; i++) {
		if (i > 0)
			len++;		    /* space separator */
		len += strlen(argv[i]) + 2; /* worst case: quotes */
	}
	len++; /* NUL */

	char *cmdl = malloc(len);
	if (!cmdl) {
		err_errno("malloc failed");
		return NULL;
	}

	/* Second pass: build the UTF-8 command line. */
	char *p = cmdl;
	for (int i = 0; argv[i]; i++) {
		if (i > 0)
			*p++ = ' ';
		int need_quote = strchr(argv[i], ' ') != NULL;
		if (need_quote)
			*p++ = '"';
		size_t alen = strlen(argv[i]);
		memcpy(p, argv[i], alen);
		p += alen;
		if (need_quote)
			*p++ = '"';
	}
	*p = '\0';

	wchar_t *wcmdl = mbs_to_wcs(cmdl);
	free(cmdl);
	return wcmdl;
}

/**
 * @brief Create a pipe returning CRT file descriptors.
 *
 * Uses CreatePipe for inheritable handles, then converts them to
 * CRT file descriptors with _open_osfhandle.
 *
 * @param read_fd   Receives the read-end fd.
 * @param write_fd  Receives the write-end fd.
 * @return 0 on success, -1 on error.
 */
static int create_pipe(int *read_fd, int *write_fd)
{
	HANDLE hRead, hWrite;
	SECURITY_ATTRIBUTES sa;

	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
		err("CreatePipe failed (%lu)", GetLastError());
		return -1;
	}

	*read_fd = _open_osfhandle((intptr_t)hRead, _O_RDONLY);
	*write_fd = _open_osfhandle((intptr_t)hWrite, _O_WRONLY);
	if (*read_fd == -1 || *write_fd == -1) {
		err("_open_osfhandle failed");
		CloseHandle(hRead);
		CloseHandle(hWrite);
		return -1;
	}

	return 0;
}

/*
 * Spawn a child attached to a ConPTY pseudo-console.  Sets proc->in /
 * proc->out to the parent-side pipe fds (input write, output read) and
 * stashes the HPCON in proc->_pty_internal so @ref pty_resize can
 * reach it.  Returns 0 on success, -1 with GetLastError-style err()
 * already logged on failure.
 */
static int start_with_pty(struct process *proc, wchar_t *wcmdl, wchar_t *wdir)
{
	HANDLE hInRead = NULL, hInWrite = NULL;
	HANDLE hOutRead = NULL, hOutWrite = NULL;
	HPCON hPC = NULL;
	STARTUPINFOEXW siex;
	PROCESS_INFORMATION pi;
	SIZE_T attr_size = 0;
	LPPROC_THREAD_ATTRIBUTE_LIST attr_list = NULL;
	int in_fd = -1, out_fd = -1;
	int rc = -1;

	if (!conpty_available()) {
		err("ConPTY not available (Windows 10 1809 or later required)");
		errno = ENOSYS;
		return -1;
	}

	if (!CreatePipe(&hInRead, &hInWrite, NULL, 0) ||
	    !CreatePipe(&hOutRead, &hOutWrite, NULL, 0)) {
		err("CreatePipe failed (%lu)", GetLastError());
		goto cleanup;
	}

	COORD size;
	size.X = (SHORT)(proc->pty_cols > 0 ? proc->pty_cols : 80);
	size.Y = (SHORT)(proc->pty_rows > 0 ? proc->pty_rows : 24);

	HRESULT hr = p_CreatePseudoConsole(size, hInRead, hOutWrite, 0, &hPC);
	if (FAILED(hr)) {
		err("CreatePseudoConsole failed (0x%lx)", (unsigned long)hr);
		goto cleanup;
	}
	/* The pseudo-console owns these ends now; we don't need our copies. */
	CloseHandle(hInRead);
	hInRead = NULL;
	CloseHandle(hOutWrite);
	hOutWrite = NULL;

	/* STARTUPINFOEX with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE.  The
	 * attribute list size is queried by InitializeProcThreadAttributeList
	 * itself in the canonical two-call dance. */
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	attr_list = malloc(attr_size);
	if (!attr_list) {
		err_errno("malloc(attr_list)");
		goto cleanup;
	}
	if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
		err("InitializeProcThreadAttributeList failed (%lu)",
		    GetLastError());
		goto cleanup;
	}
	if (!UpdateProcThreadAttribute(attr_list, 0,
				       PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC,
				       sizeof(hPC), NULL, NULL)) {
		err("UpdateProcThreadAttribute failed (%lu)", GetLastError());
		goto cleanup;
	}

	ZeroMemory(&siex, sizeof(siex));
	siex.StartupInfo.cb = sizeof(siex);
	siex.lpAttributeList = attr_list;

	ZeroMemory(&pi, sizeof(pi));
	if (!CreateProcessW(NULL, wcmdl, NULL, NULL, FALSE,
			    EXTENDED_STARTUPINFO_PRESENT, NULL, wdir,
			    &siex.StartupInfo, &pi)) {
		err("CreateProcess (pty) failed (%lu)", GetLastError());
		goto cleanup;
	}

	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	proc->pid = (pid_t)(intptr_t)pi.hProcess;
	CloseHandle(pi.hThread);

	/* Convert the parent-side pipe ends to CRT fds so they slot into
	 * the existing in/out pattern (and pipe_read_timed works on them
	 * unchanged). */
	in_fd = _open_osfhandle((intptr_t)hInWrite, _O_WRONLY | _O_BINARY);
	out_fd = _open_osfhandle((intptr_t)hOutRead, _O_RDONLY | _O_BINARY);
	if (in_fd == -1 || out_fd == -1) {
		err("_open_osfhandle failed");
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hProcess);
		proc->pid = 0;
		goto cleanup;
	}
	hInWrite = NULL; /* now owned by in_fd */
	hOutRead = NULL; /* now owned by out_fd */

	proc->in = in_fd;
	proc->out = out_fd;
	proc->err = -1;
	proc->_pty_internal = hPC;
	hPC = NULL; /* now owned by proc->_pty_internal */
	rc = 0;

cleanup:
	if (hInRead)
		CloseHandle(hInRead);
	if (hInWrite)
		CloseHandle(hInWrite);
	if (hOutRead)
		CloseHandle(hOutRead);
	if (hOutWrite)
		CloseHandle(hOutWrite);
	if (hPC)
		p_ClosePseudoConsole(hPC);
	if (attr_list) {
		DeleteProcThreadAttributeList(attr_list);
		free(attr_list);
	}
	return rc;
}

/**
 * @brief Start a child process (Windows).
 *
 * Builds a wide-char command line from argv, sets up pipe
 * redirections if requested, and calls CreateProcessW.
 */
int process_start(struct process *proc)
{
	int pipe_in[2] = {-1, -1};
	int pipe_out[2] = {-1, -1};
	int pipe_err[2] = {-1, -1};
	wchar_t *wcmdl = NULL;
	wchar_t *wdir = NULL;
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;

	if (proc->use_shell) {
		const char *sh_argv[] = {"cmd.exe", "/c", proc->argv[0], NULL};
		wcmdl = build_cmdline(sh_argv);
	} else {
		wcmdl = build_cmdline(proc->argv);
	}
	if (!wcmdl)
		return -1;

	if (proc->dir) {
		wdir = mbs_to_wcs(proc->dir);
		if (!wdir)
			goto err;
	}

	if (proc->use_pty) {
		int rc = start_with_pty(proc, wcmdl, wdir);
		free(wcmdl);
		free(wdir);
		return rc;
	}

	if (proc->pipe_in && create_pipe(&pipe_in[0], &pipe_in[1]))
		goto err;
	if (proc->pipe_out && create_pipe(&pipe_out[0], &pipe_out[1]))
		goto err;
	if (proc->pipe_err && create_pipe(&pipe_err[0], &pipe_err[1]))
		goto err;

	/* Set extra environment variables (inherited by child). */
	if (proc->env) {
		for (int i = 0; proc->env[i]; i++) {
			const char *kv = proc->env[i];
			const char *eq = strchr(kv, '=');
			char name[256];
			size_t nlen;

			if (!eq)
				continue;
			nlen = (size_t)(eq - kv);
			if (nlen >= sizeof(name))
				continue;
			memcpy(name, kv, nlen);
			name[nlen] = '\0';
			setenv(name, eq + 1, 1);
		}
	}

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);

	if (proc->pipe_in || proc->pipe_out || proc->pipe_err ||
	    proc->merge_err) {
		si.dwFlags |= STARTF_USESTDHANDLES;
		si.hStdInput = proc->pipe_in
				   ? (HANDLE)_get_osfhandle(pipe_in[0])
				   : GetStdHandle(STD_INPUT_HANDLE);
		si.hStdOutput = proc->pipe_out
				    ? (HANDLE)_get_osfhandle(pipe_out[1])
				    : GetStdHandle(STD_OUTPUT_HANDLE);
		si.hStdError =
		    proc->pipe_err    ? (HANDLE)_get_osfhandle(pipe_err[1])
		    : proc->merge_err ? si.hStdOutput
				      : GetStdHandle(STD_ERROR_HANDLE);
	}

	ZeroMemory(&pi, sizeof(pi));

	if (!CreateProcessW(NULL, wcmdl, NULL, NULL, TRUE, 0, NULL, wdir, &si,
			    &pi)) {
		err("CreateProcess failed (%lu)", GetLastError());
		goto err;
	}

	/* Store the process handle as pid for process_finish. */
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	proc->pid = (pid_t)(intptr_t)pi.hProcess;
	CloseHandle(pi.hThread);

	/* Close child-side pipe ends, keep parent-side. */
	if (proc->pipe_in) {
		_close(pipe_in[0]);
		proc->in = pipe_in[1];
	}
	if (proc->pipe_out) {
		_close(pipe_out[1]);
		proc->out = pipe_out[0];
	}
	if (proc->pipe_err) {
		_close(pipe_err[1]);
		proc->err = pipe_err[0];
	}

	free(wcmdl);
	free(wdir);
	return 0;

err:
	if (pipe_in[0] != -1) {
		_close(pipe_in[0]);
		_close(pipe_in[1]);
	}
	if (pipe_out[0] != -1) {
		_close(pipe_out[0]);
		_close(pipe_out[1]);
	}
	if (pipe_err[0] != -1) {
		_close(pipe_err[0]);
		_close(pipe_err[1]);
	}
	free(wcmdl);
	free(wdir);
	return -1;
}

/**
 * @brief Wait for a child process to finish (Windows).
 *
 * Closes any open pipe fds, waits for the process to exit, and
 * retrieves its exit code.
 */
int process_finish(struct process *proc)
{
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	HANDLE hProcess = (HANDLE)(intptr_t)proc->pid;
	DWORD exitCode;

	if (proc->use_pty) {
		/* Closing the pseudo-console signals EOF to the child;
		 * standard well-behaved CLI children wind down on stdin EOF.
		 * We free the in/out fds afterwards. */
		if (proc->_pty_internal) {
			p_ClosePseudoConsole((HPCON)proc->_pty_internal);
			proc->_pty_internal = NULL;
		}
		if (proc->in != -1) {
			_close(proc->in);
			proc->in = -1;
		}
		if (proc->out != -1) {
			_close(proc->out);
			proc->out = -1;
		}
	} else {
		if (proc->pipe_in && proc->in != -1) {
			_close(proc->in);
			proc->in = -1;
		}
		if (proc->pipe_out && proc->out != -1) {
			_close(proc->out);
			proc->out = -1;
		}
		if (proc->pipe_err && proc->err != -1) {
			_close(proc->err);
			proc->err = -1;
		}
	}

	WaitForSingleObject(hProcess, INFINITE);

	if (!GetExitCodeProcess(hProcess, &exitCode)) {
		err("GetExitCodeProcess failed (%lu)", GetLastError());
		CloseHandle(hProcess);
		return -1;
	}

	CloseHandle(hProcess);
	return (int)exitCode;
}

int pty_resize(struct process *proc, int rows, int cols)
{
	if (!proc->use_pty || !proc->_pty_internal) {
		errno = EINVAL;
		return -1;
	}
	if (rows <= 0 || cols <= 0) {
		errno = EINVAL;
		return -1;
	}
	if (!conpty_available()) {
		errno = ENOSYS;
		return -1;
	}
	COORD size;
	size.X = (SHORT)cols;
	size.Y = (SHORT)rows;
	HRESULT hr = p_ResizePseudoConsole((HPCON)proc->_pty_internal, size);
	if (FAILED(hr)) {
		errno = EIO;
		return -1;
	}
	return 0;
}

/**
 * @brief Read from a pipe with a timeout (Windows).
 *
 * Anonymous pipes created by CreatePipe() are not signalable objects,
 * so WaitForSingleObject() on the pipe handle does not block on data
 * availability.  The canonical workaround is PeekNamedPipe() in a
 * short poll loop: check whether any bytes are buffered, sleep if
 * not, retry until data arrives or @p timeout_ms elapses.  Once data
 * is known to be available, ReadFile() returns immediately.
 */
ssize_t pipe_read_timed(int fd, void *buf, size_t n, unsigned timeout_ms)
{
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	unsigned elapsed = 0;
	DWORD avail = 0;
	DWORD got = 0;

	if (h == INVALID_HANDLE_VALUE)
		return -1;

	for (;;) {
		if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
			return -1; /* broken pipe / EOF / error */
		if (avail > 0)
			break;
		if (elapsed >= timeout_ms)
			return 0;
		/* 20 ms poll granularity keeps wakeups cheap while
		 * staying responsive enough for a 10 Hz spinner. */
		DWORD step = timeout_ms - elapsed;
		if (step > 20)
			step = 20;
		Sleep(step);
		elapsed += step;
	}

	if (!ReadFile(h, buf, (DWORD)n, &got, NULL) || got == 0)
		return -1;
	return (ssize_t)got;
}

int kill_w(pid_t pid, int sig)
{
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	HANDLE hProcess = (HANDLE)(intptr_t)pid;

	/* Windows has no signal mechanism; the only thing the shim
	 * implements is "force this child to stop now."  SIGTERM is the
	 * conventional ask in POSIX call sites, so treat it as that.
	 * Unknown signals fail loudly rather than silently no-op. */
	if (sig != SIGTERM) {
		errno = EINVAL;
		return -1;
	}
	if (hProcess == NULL || hProcess == INVALID_HANDLE_VALUE) {
		errno = ESRCH;
		return -1;
	}
	if (!TerminateProcess(hProcess, 1)) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

unsigned long long mono_ms(void)
{
	return (unsigned long long)GetTickCount64();
}

void delay_ms(uint32_t ms) { Sleep((DWORD)ms); }

int self_pid(void) { return (int)GetCurrentProcessId(); }

const char *process_exe(void)
{
	static char buf[4096];
	static const char *result;
	static int initialized;
	wchar_t wbuf[4096];
	DWORD n;
	char *utf8;
	size_t len;

	if (initialized)
		return result;
	initialized = 1;

	n = GetModuleFileNameW(NULL, wbuf,
			       (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])));
	if (n == 0 || n >= (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])))
		return NULL;

	utf8 = wcs_to_mbs(wbuf);
	if (!utf8)
		return NULL;

	len = strlen(utf8);
	if (len < sizeof(buf)) {
		memcpy(buf, utf8, len + 1);
		result = buf;
	}
	free(utf8);
	return result;
}
