/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * System-git wrappers -- see git.h for the contract.  Moved here
 * from cmd/repo/repo.c so consumers outside the repo namespace (e.g.
 * @c ice idf component for git-sourced components) don't have to
 * reach across a namespace boundary.
 */
#include "git.h"

#include <unistd.h>

#include "ice.h"

int git_run(const char *dir, const char **argv)
{
	struct process proc = PROCESS_INIT;

	proc.argv = argv;
	proc.dir = dir;
	return process_run(&proc);
}

int git_capture(const char *dir, const char **argv, struct sbuf *out)
{
	struct process proc = PROCESS_INIT;
	char buf[4096];
	ssize_t n;
	int rc;

	proc.argv = argv;
	proc.dir = dir;
	proc.pipe_out = 1;
	if (process_start(&proc))
		return -1;

	while ((n = read(proc.out, buf, sizeof(buf))) > 0)
		sbuf_add(out, buf, (size_t)n);

	rc = process_finish(&proc);
	return rc;
}
