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

static struct process pager_proc;
static int pager_started;
static int saved_stdout_fd = -1;

int pager_active(void) { return pager_started; }

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
	const char *argv[2];

	if (pager_started)
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
	 *
	 * overwrite=0 preserves the user's own LESS if they've set one.
	 */
	setenv("LESS", "FRX", 0);

	argv[0] = cmd;
	argv[1] = NULL;
	pager_proc.argv = argv;
	pager_proc.use_shell = 1;
	pager_proc.pipe_in = 1;
	if (process_start(&pager_proc))
		return;

	fflush(stdout);
	saved_stdout_fd = dup(STDOUT_FILENO);
	dup2(pager_proc.in, STDOUT_FILENO);
	close(pager_proc.in);
	pager_proc.in = -1;

	pager_started = 1;
	atexit(pager_end);
}

void pager_end(void)
{
	if (!pager_started)
		return;

	fflush(stdout);
	if (saved_stdout_fd >= 0) {
		dup2(saved_stdout_fd, STDOUT_FILENO);
		close(saved_stdout_fd);
		saved_stdout_fd = -1;
	}
	process_finish(&pager_proc);
	pager_started = 0;
}
