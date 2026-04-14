/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/win/process.c
 * @brief Windows implementation of child process spawning.
 *
 * Implements process_start() and process_finish() using
 * CreateProcessW()/WaitForSingleObject() with optional pipe redirection.
 */
#include <errhandlingapi.h>
#include <fcntl.h>
#include <io.h>
#include <processenv.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>
#include <winerror.h>

#include "../../ice.h"
#include "wconv.h"

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

	wcmdl = build_cmdline(proc->argv);
	if (!wcmdl)
		return -1;

	if (proc->dir) {
		wdir = mbs_to_wcs(proc->dir);
		if (!wdir)
			goto err;
	}

	if (proc->pipe_in && create_pipe(&pipe_in[0], &pipe_in[1]))
		goto err;
	if (proc->pipe_out && create_pipe(&pipe_out[0], &pipe_out[1]))
		goto err;
	if (proc->pipe_err && create_pipe(&pipe_err[0], &pipe_err[1]))
		goto err;

	/* Set extra environment variables (inherited by child). */
	if (proc->env) {
		for (int i = 0; proc->env[i]; i++)
			putenv((char *)proc->env[i]);
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

	WaitForSingleObject(hProcess, INFINITE);

	if (!GetExitCodeProcess(hProcess, &exitCode)) {
		err("GetExitCodeProcess failed (%lu)", GetLastError());
		CloseHandle(hProcess);
		return -1;
	}

	CloseHandle(hProcess);
	return (int)exitCode;
}
