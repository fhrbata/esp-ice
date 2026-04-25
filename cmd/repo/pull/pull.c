/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/repo/pull/pull.c
 * @brief `ice repo pull` -- refresh the reference to latest master.
 */
#include "cmd/repo/repo.h"

static int pull_jobs = 8;

static const struct option pull_opts[] = {
    OPT_INT('j', "jobs", &pull_jobs, "n",
	    "parallel submodule updates (default 8)", NULL),
    OPT_END(),
};

int cmd_repo_pull(int argc, const char **argv);
int cmd_repo___pull(int argc, const char **argv);

/* clang-format off */
static const struct cmd_manual repo_pull_manual = {
	.name = "ice repo pull",
	.summary = "refresh the reference to latest master",

	.description =
	H_PARA("Clean any working-tree state left by a previous "
	       "@b{ice repo checkout}, switch the reference to "
	       "@b{master}, fast-forward to @b{origin/master}, and "
	       "recursively update submodules.  Existing checkouts "
	       "under @b{~/.ice/checkouts/} are left untouched -- use "
	       "@b{ice repo checkout} again to build a new working tree "
	       "against the refreshed reference."),

	.examples =
	H_EXAMPLE("ice repo pull"),
};
/* clang-format on */

const struct cmd_desc cmd_repo_pull_desc = {
    .name = "pull",
    .fn = cmd_repo_pull,
    .opts = pull_opts,
    .manual = &repo_pull_manual,
};

const struct cmd_desc cmd_repo___pull_desc = {
    .name = "__pull",
    .fn = cmd_repo___pull,
    .opts = pull_opts,
    .manual = &repo_pull_manual,
};

int cmd_repo_pull(int argc, const char **argv)
{
	struct svec cmd = SVEC_INIT;
	struct process proc = PROCESS_INIT;
	const char *exe = process_exe();
	int rc;

	svec_push(&cmd, exe ? exe : "ice");
	svec_push(&cmd, "repo");
	svec_push(&cmd, "__pull");
	for (int i = 1; i < argc; i++)
		svec_push(&cmd, argv[i]);

	parse_options(argc, argv, &cmd_repo_pull_desc);

	proc.argv = cmd.v;
	rc = process_run_progress(&proc, "Pulling", "repo-pull", NULL);
	svec_clear(&cmd);
	return rc;
}

int cmd_repo___pull(int argc, const char **argv)
{
	const char *base = repo_reference_path();
	char jobs_str[16];
	struct sbuf lock = SBUF_INIT;

	parse_options(argc, argv, &cmd_repo___pull_desc);
	repo_ensure_reference();

	sbuf_addf(&lock, "%s/esp-idf.lock", ice_home());
	if (lock_acquire(lock.buf, 2000) < 0)
		die_errno("lock '%s' (remove if no ice is running)", lock.buf);

	if (pull_jobs < 1)
		pull_jobs = 1;
	snprintf(jobs_str, sizeof(jobs_str), "%d", pull_jobs);

	fprintf(stderr, "Refreshing reference at @b{%s} ...\n", base);

	{
		const char *argv[] = {"git", "clean", "-ffdx", NULL};
		git_run(base, argv);
	}

	{
		const char *argv[] = {"git", "checkout", "--force", "master",
				      NULL};
		if (git_run(base, argv) != 0)
			die("git checkout master failed");
	}

	{
		const char *argv[] = {"git", "pull", "--ff-only", NULL};
		if (git_run(base, argv) != 0)
			die("git pull failed");
	}

	{
		const char *argv[] = {"git",
				      "submodule",
				      "update",
				      "--init",
				      "--recursive",
				      "--force",
				      "--no-recommend-shallow",
				      "--jobs",
				      jobs_str,
				      NULL};
		if (git_run(base, argv) != 0)
			warn("some submodules failed to update");
	}

	lock_release(lock.buf);
	sbuf_release(&lock);
	fprintf(stderr, "@g{done}\n");
	return 0;
}
