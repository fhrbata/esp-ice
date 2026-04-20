/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pager.c
 * @brief Paging support -- spawn $PAGER, redirect stdout through it.
 */
#include "ice.h"

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#define dup _dup
#define dup2 _dup2
#endif

/*
 * POSIX functions not declared by ISO C99 <stdio.h> under
 * -std=c99 -pedantic.  Same pattern as putenv() in platform.h.
 */
#ifndef _WIN32
int fileno(FILE *stream);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int setenv(const char *name, const char *value, int overwrite);
FILE *popen(const char *command, const char *type);
int pclose(FILE *stream);
#endif

static FILE *pager_pipe;
static int saved_stdout_fd = -1;

int pager_active(void) { return pager_pipe != NULL; }

/*
 * Return 1 if the first whitespace-separated token of @p cmd resolves
 * to an executable -- either as a path (absolute/relative) or on PATH.
 * Used so we skip paging gracefully instead of watching the shell
 * complain "'less' is not recognized" on systems without a pager.
 */
static int pager_available(const char *cmd)
{
	struct sbuf prog = SBUF_INIT;
	const char *s = cmd;
	int ok;

	while (*s && *s != ' ' && *s != '\t')
		s++;
	sbuf_add(&prog, cmd, (size_t)(s - cmd));

	if (strchr(prog.buf, '/') || strchr(prog.buf, '\\'))
		ok = !access(prog.buf, F_OK);
	else
		ok = find_in_path(prog.buf);

	sbuf_release(&prog);
	return ok;
}

void pager_start(void)
{
	const char *cmd;

	if (pager_pipe)
		return;
	if (!isatty(STDOUT_FILENO))
		return;
	if (global_no_pager)
		return;

	cmd = config_get("core.pager");
	if (!cmd || !*cmd)
		return;
	if (!pager_available(cmd))
		return;

	/*
	 * Configure `less` sensibly when the user hasn't:
	 *   F -- exit immediately if output fits on one screen
	 *   R -- pass raw ANSI colour sequences through
	 *   X -- don't clear the screen on start/exit
	 */
#ifdef _WIN32
	if (!getenv("LESS"))
		putenv("LESS=FRX");
#else
	setenv("LESS", "FRX", 0);
#endif

	pager_pipe = popen(cmd, "w");
	if (!pager_pipe)
		return;

	fflush(stdout);
	saved_stdout_fd = dup(STDOUT_FILENO);
	dup2(fileno(pager_pipe), STDOUT_FILENO);

	atexit(pager_end);
}

void pager_end(void)
{
	if (!pager_pipe)
		return;

	fflush(stdout);
	if (saved_stdout_fd >= 0) {
		dup2(saved_stdout_fd, STDOUT_FILENO);
		close(saved_stdout_fd);
		saved_stdout_fd = -1;
	}
	pclose(pager_pipe);
	pager_pipe = NULL;
}
