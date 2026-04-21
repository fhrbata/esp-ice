/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/repo/repo.c
 * @brief `ice repo` -- manage the ESP-IDF source tree (dispatcher +
 *        shared helpers).
 *
 * ice maintains a single ice-managed reference clone at
 * ~/.ice/esp-idf and creates per-version working checkouts under
 * ~/.ice/checkouts/<name>/ that borrow objects from the reference
 * via git's own --reference / submodule.alternateLocation machinery.
 *
 * Subcommand implementations live under cmd/repo/<name>/; this file
 * holds the namespace dispatcher plus the helpers used by more than
 * one of them (declared in repo.h with a repo_ prefix).
 */
#include "repo.h"

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

const char *repo_reference_path(void)
{
	static struct sbuf path = SBUF_INIT;

	if (!path.len)
		sbuf_addf(&path, "%s/esp-idf", ice_home());
	return path.buf;
}

const char *repo_checkouts_path(void)
{
	static struct sbuf path = SBUF_INIT;

	if (!path.len)
		sbuf_addf(&path, "%s/checkouts", ice_home());
	return path.buf;
}

int repo_run_git(const char *dir, const char **argv)
{
	struct process proc = PROCESS_INIT;

	proc.argv = argv;
	proc.dir = dir;
	return process_run(&proc);
}

int repo_run_git_capture(const char *dir, const char **argv, struct sbuf *out)
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

void repo_ensure_reference(void)
{
	if (access(repo_reference_path(), F_OK) != 0) {
		hint("run @b{ice repo clone} first");
		die("no ESP-IDF reference at @b{%s}", repo_reference_path());
	}
}

int repo_version_supported(const char *ver)
{
	int major, minor;

	if (*ver == 'v')
		ver++;
	if (sscanf(ver, "%d.%d", &major, &minor) < 2)
		return 0;
	return major > IDF_MIN_MAJOR ||
	       (major == IDF_MIN_MAJOR && minor >= IDF_MIN_MINOR);
}

static int collect_dir(const char *name, void *ud)
{
	svec_push((struct svec *)ud, name);
	return 0;
}

void repo_collect_checkouts(struct svec *out)
{
	struct svec raw = SVEC_INIT;

	if (access(repo_checkouts_path(), F_OK) != 0)
		return;
	dir_foreach(repo_checkouts_path(), collect_dir, &raw);
	svec_sort(&raw);
	for (size_t i = 0; i < raw.nr; i++) {
		struct sbuf full = SBUF_INIT;

		sbuf_addf(&full, "%s/%s", repo_checkouts_path(), raw.v[i]);
		if (is_directory(full.buf))
			svec_push(out, raw.v[i]);
		sbuf_release(&full);
	}
	svec_clear(&raw);
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

extern const struct cmd_desc cmd_repo_clone_desc;
extern const struct cmd_desc cmd_repo___clone_desc;
extern const struct cmd_desc cmd_repo_pull_desc;
extern const struct cmd_desc cmd_repo___pull_desc;
extern const struct cmd_desc cmd_repo_list_desc;
extern const struct cmd_desc cmd_repo_checkout_desc;
extern const struct cmd_desc cmd_repo___checkout_desc;
extern const struct cmd_desc cmd_repo_info_desc;

static const struct option cmd_repo_opts[] = {OPT_END()};

/* clang-format off */
static const struct cmd_manual repo_manual = {
	.name = "ice repo",
	.summary = "manage the ESP-IDF source tree",

	.description =
	H_PARA("Manage the ESP-IDF source tree.  @b{ice} keeps a single "
	       "reference clone at @b{~/.ice/esp-idf} and creates per-version "
	       "working checkouts under @b{~/.ice/checkouts/<name>/} that "
	       "borrow objects from the reference.")
	H_PARA("Run @b{ice repo <subcommand> --help} for details.")
	H_PARA("The reference is ice-managed; do not work in it.  Create "
	       "checkouts with @b{ice repo checkout} and work there instead."),

	.examples =
	H_EXAMPLE("ice repo clone")
	H_EXAMPLE("ice repo checkout v5.4 v5.4")
	H_EXAMPLE("ice repo list | head -20")
	H_EXAMPLE("ice repo info"),
};
/* clang-format on */

static const struct cmd_desc *const repo_subs[] = {
    &cmd_repo_clone_desc,
    &cmd_repo_pull_desc,
    &cmd_repo_list_desc,
    &cmd_repo_checkout_desc,
    &cmd_repo_info_desc,
    /* Hidden plumbing (name starts with '_'); help/completion skip. */
    &cmd_repo___clone_desc,
    &cmd_repo___pull_desc,
    &cmd_repo___checkout_desc,
    NULL,
};

const struct cmd_desc cmd_repo_desc = {
    .name = "repo",
    .opts = cmd_repo_opts,
    .manual = &repo_manual,
    .subcommands = repo_subs,
};
