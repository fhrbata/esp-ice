/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/repo/clone/clone.c
 * @brief `ice repo clone` -- create the reference clone at ~/.ice/esp-idf.
 */
#include "cmd/repo/repo.h"

static const char *clone_reference;
static int clone_dissociate;
static int clone_jobs = 8;

static const struct option clone_opts[] = {
    OPT_STRING(0, "reference", &clone_reference, "path",
	       "borrow objects from an existing esp-idf clone", NULL),
    OPT_BOOL(0, "dissociate", &clone_dissociate,
	     "copy borrowed objects locally, then stop borrowing"),
    OPT_INT('j', "jobs", &clone_jobs, "n",
	    "parallel submodule clones (default 8)", NULL),
    OPT_POSITIONAL_OPT("url", NULL),
    OPT_END(),
};

int cmd_repo_clone(int argc, const char **argv);
int cmd_repo___clone(int argc, const char **argv);

/* clang-format off */
static const struct cmd_manual repo_clone_manual = {
	.name = "ice repo clone",
	.summary = "create the reference clone at ~/.ice/esp-idf/",

	.description =
	H_PARA("Clone ESP-IDF into @b{~/.ice/esp-idf}.  The reference "
	       "is ice-managed; do not work in it directly.  Use "
	       "@b{ice repo checkout} to create per-version working "
	       "trees that share objects with the reference.")
	H_PARA("@b{--reference <path>} borrows objects from an existing "
	       "ESP-IDF clone to avoid redundant network transfer.  The "
	       "@b{-if-able} flavor is used under the hood, so a "
	       "partially populated or shallow source is tolerated.  "
	       "Pass @b{--dissociate} to copy borrowed objects locally "
	       "so the reference no longer depends on @b{<path>}."),

	.examples =
	H_EXAMPLE("ice repo clone")
	H_EXAMPLE("ice repo clone --reference ~/work/esp-idf")
	H_EXAMPLE("ice repo clone --reference ~/work/esp-idf --dissociate"),
};
/* clang-format on */

const struct cmd_desc cmd_repo_clone_desc = {
    .name = "clone",
    .fn = cmd_repo_clone,
    .opts = clone_opts,
    .manual = &repo_clone_manual,
};

/* Hidden plumbing: does the work.  The porcelain cmd_repo_clone above
 * invokes this via process_run_progress so the combined output lands
 * in a single log under ~/.ice/logs. */
const struct cmd_desc cmd_repo___clone_desc = {
    .name = "__clone",
    .fn = cmd_repo___clone,
    .opts = clone_opts,
    .manual = &repo_clone_manual,
};

/* Porcelain wrapper -- forwards to __clone with progress display. */
int cmd_repo_clone(int argc, const char **argv)
{
	struct svec cmd = SVEC_INIT;
	struct process proc = PROCESS_INIT;
	const char *exe = process_exe();
	int rc;

	/* Copy argv BEFORE parse_options runs, because it compacts argv
	 * in place (packs positionals to the front, discarding option
	 * tokens).  argv[0] is the leaf's own command name ("clone"),
	 * so skip it -- the child __clone parses fresh. */
	svec_push(&cmd, exe ? exe : "ice");
	svec_push(&cmd, "repo");
	svec_push(&cmd, "__clone");
	for (int i = 1; i < argc; i++)
		svec_push(&cmd, argv[i]);

	/* parse_options handles --help / -h / --ice-complete (exits)
	 * and dies on unknown options so the user gets a diagnostic
	 * before any spinner flashes.  Its returned argc/argv mutation
	 * is discarded -- we forward the saved copy above. */
	parse_options(argc, argv, &cmd_repo_clone_desc);

	proc.argv = cmd.v;
	rc = process_run_progress(&proc, "Cloning", "repo-clone", NULL);
	svec_clear(&cmd);
	return rc;
}

int cmd_repo___clone(int argc, const char **argv)
{
	const char *url;
	char jobs_str[16];
	struct svec args = SVEC_INIT;
	struct sbuf lock = SBUF_INIT;

	argc = parse_options(argc, argv, &cmd_repo___clone_desc);
	url = argc >= 1 ? argv[0] : IDF_CLONE_URL;

	sbuf_addf(&lock, "%s/esp-idf.lock", ice_home());
	if (lock_acquire(lock.buf, 2000) < 0)
		die_errno("lock '%s' (remove if no ice is running)", lock.buf);

	if (!access(repo_reference_path(), F_OK)) {
		hint("use @b{ice repo pull} to update");
		die("ESP-IDF already cloned at @b{%s}", repo_reference_path());
	}

	if (clone_jobs < 1)
		clone_jobs = 1;
	snprintf(jobs_str, sizeof(jobs_str), "%d", clone_jobs);

	if (mkdirp_for_file(repo_reference_path()) < 0)
		die_errno("cannot create parent of '%s'",
			  repo_reference_path());

	fprintf(stderr, "Cloning @b{%s} into @b{%s} ...\n", url,
		repo_reference_path());

	svec_push(&args, "git");
	svec_push(&args, "clone");
	if (clone_reference) {
		svec_push(&args, "--reference-if-able");
		svec_push(&args, clone_reference);
		svec_push(&args, "-c");
		svec_push(&args, "submodule.alternateLocation=superproject");
		svec_push(&args, "-c");
		svec_push(&args, "submodule.alternateErrorStrategy=info");
		if (clone_dissociate)
			svec_push(&args, "--dissociate");
	}
	svec_push(&args, url);
	svec_push(&args, repo_reference_path());

	if (repo_run_git(NULL, args.v) != 0)
		die("git clone failed");
	svec_clear(&args);

	/*
	 * Submodules are cloned in a separate step so we can pass
	 * --no-recommend-shallow: .gitmodules marks several binary blob
	 * submodules `shallow = true`, and a depth=1 clone drops older
	 * commits whenever a later checkout moves the submodule SHA --
	 * which forces a re-fetch the next time an earlier ref is
	 * checked out.  Full-depth submodules accumulate permanently, so
	 * any SHA once fetched stays for every subsequent checkout.
	 */
	{
		const char *argv[] = {
		    "git",    "submodule",   "update",
		    "--init", "--recursive", "--no-recommend-shallow",
		    "--jobs", jobs_str,	     NULL};
		if (repo_run_git(repo_reference_path(), argv) != 0)
			die("git submodule update failed");
	}

	lock_release(lock.buf);
	sbuf_release(&lock);
	fprintf(stderr, "@g{done}\n");
	return 0;
}
