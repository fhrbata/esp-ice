/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file idf.c
 * @brief `ice idf` -- manage the ESP-IDF source tree.
 *
 * Subcommands:
 *   clone   - clone esp-idf into ~/.ice/esp-idf/repo
 *   repo    - point ice at an existing esp-idf clone
 *   switch  - checkout a version (tag/branch) and update submodules
 *   list    - list available versions (git tags)
 *   pull    - fetch latest from upstream (managed repo only)
 *   info    - show current IDF path and version
 */
#include "ice.h"

#include "fs.h"

#define IDF_CLONE_URL "https://github.com/espressif/esp-idf.git"

/*
 * Minimum supported IDF major.minor -- tags and release branches
 * older than this are filtered from `ice idf list`.
 */
#define IDF_MIN_MAJOR 5
#define IDF_MIN_MINOR 0

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/** Return ~/.ice/esp-idf/base.git (bare object store). */
static const char *base_repo_path(void)
{
	static struct sbuf path = SBUF_INIT;

	if (!path.len)
		sbuf_addf(&path, "%s/esp-idf/base.git", ice_home());
	return path.buf;
}

/** Return ~/.ice/esp-idf/current (active working tree). */
static const char *current_tree_path(void)
{
	static struct sbuf path = SBUF_INIT;

	if (!path.len)
		sbuf_addf(&path, "%s/esp-idf/current", ice_home());
	return path.buf;
}

/**
 * Return the active IDF path.
 * idf.path config > managed current tree > NULL.
 */
static const char *idf_path(void)
{
	const char *configured = config_get("idf.path");

	if (configured && *configured)
		return configured;

	if (!access(current_tree_path(), F_OK))
		return current_tree_path();

	return NULL;
}

/** Check whether @p path looks like an esp-idf tree. */
static int is_idf_repo(const char *path)
{
	struct sbuf check = SBUF_INIT;
	int ok;

	sbuf_addf(&check, "%s/tools/tools.json", path);
	ok = !access(check.buf, F_OK);
	sbuf_release(&check);
	return ok;
}

/** Run a git command in @p dir.  Returns the exit code. */
static int run_git(const char *dir, const char **argv)
{
	struct process proc = PROCESS_INIT;

	proc.argv = argv;
	proc.dir = dir;
	return process_run(&proc);
}

/** Run a git command and capture stdout into @p out. */
static int run_git_capture(const char *dir, const char **argv, struct sbuf *out)
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

/**
 * Populate submodules in @p tree_path for version @p version.
 *
 * For each gitlink (160000) entry in the tree, create a --shared
 * clone from the base repo's .git/modules/<path>/ and checkout
 * the pinned commit.  Handles one level of nested submodules.
 *
 * @p base is the path to a repo that has .git/modules/ populated
 * (either the base.git bare repo or a user-provided repo).
 */
static int populate_submodules(const char *base, const char *tree_path,
			       const char *version)
{
	struct sbuf ls = SBUF_INIT;
	size_t pos;
	char *line;
	int errors = 0;

	/* Get all gitlink entries for this version. */
	{
		const char *argv[] = {"git", "ls-tree", "-r", version, NULL};
		if (run_git_capture(base, argv, &ls) != 0) {
			sbuf_release(&ls);
			return -1;
		}
	}

	pos = 0;
	while ((line = sbuf_getline(ls.buf, ls.len, &pos))) {
		char mode[8], type[8], sha[64], path[1024];

		if (sscanf(line, "%7s %7s %63s %1023[^\n]", mode, type, sha,
			   path) != 4)
			continue;
		if (strcmp(mode, "160000") != 0)
			continue;

		struct sbuf mod_git = SBUF_INIT;
		struct sbuf dest = SBUF_INIT;

		sbuf_addf(&mod_git, "%s/.git/modules/%s", base, path);
		sbuf_addf(&dest, "%s/%s", tree_path, path);

		/* If the module object store doesn't exist, skip. */
		if (!is_directory(mod_git.buf)) {
			/* Try bare repo layout (modules/ at top level). */
			sbuf_release(&mod_git);
			sbuf_addf(&mod_git, "%s/modules/%s", base, path);
			if (!is_directory(mod_git.buf)) {
				warn("submodule '%s': no local objects, "
				     "skipping",
				     path);
				errors++;
				goto next;
			}
		}

		/* Remove existing dir and clone --shared. */
		if (is_directory(dest.buf))
			rmtree(dest.buf, 0);
		mkdirp_for_file(dest.buf);

		{
			const char *clone_argv[] = {
			    "git",     "clone",	    "--shared", "--no-checkout",
			    "--quiet", mod_git.buf, dest.buf,	NULL};
			if (run_git(NULL, clone_argv) != 0) {
				warn("submodule '%s': clone failed", path);
				errors++;
				goto next;
			}
		}

		{
			const char *co_argv[] = {"git", "checkout", "--quiet",
						 sha, NULL};
			if (run_git(dest.buf, co_argv) != 0) {
				warn("submodule '%s': checkout %s failed", path,
				     sha);
				errors++;
				goto next;
			}
		}

		/* Handle nested submodules (one level deep). */
		{
			struct sbuf nested_ls = SBUF_INIT;
			const char *nested_argv[] = {"git", "ls-tree", sha,
						     NULL};

			if (run_git_capture(mod_git.buf, nested_argv,
					    &nested_ls) == 0) {
				size_t npos = 0;
				char *nline;

				while ((nline = sbuf_getline(nested_ls.buf,
							     nested_ls.len,
							     &npos))) {
					char nm[8], nt[8], ns[64], np[1024];
					if (sscanf(nline,
						   "%7s %7s %63s "
						   "%1023[^\n]",
						   nm, nt, ns, np) != 4)
						continue;
					if (strcmp(nm, "160000") != 0)
						continue;

					struct sbuf nmod = SBUF_INIT;
					struct sbuf ndest = SBUF_INIT;

					sbuf_addf(&nmod, "%s/modules/%s",
						  mod_git.buf, np);
					sbuf_addf(&ndest, "%s/%s", dest.buf,
						  np);

					if (!is_directory(nmod.buf)) {
						sbuf_release(&nmod);
						sbuf_release(&ndest);
						continue;
					}

					if (is_directory(ndest.buf))
						rmtree(ndest.buf, 0);
					mkdirp_for_file(ndest.buf);

					const char *nc[] = {
					    "git",	"clone",
					    "--shared", "--no-checkout",
					    "--quiet",	nmod.buf,
					    ndest.buf,	NULL};
					if (run_git(NULL, nc) == 0) {
						const char *nco[] = {
						    "git", "checkout",
						    "--quiet", ns, NULL};
						run_git(ndest.buf, nco);
					}

					sbuf_release(&nmod);
					sbuf_release(&ndest);
				}
			}
			sbuf_release(&nested_ls);
		}

	next:
		sbuf_release(&mod_git);
		sbuf_release(&dest);
	}

	sbuf_release(&ls);
	return errors ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Subcommands                                                         */
/* ------------------------------------------------------------------ */

static const char *clone_reference;

static const struct option clone_opts[] = {
    OPT_STRING(0, "reference", &clone_reference, "path",
	       "borrow objects from an existing esp-idf clone", NULL),
    OPT_END(),
};

static int cmd_idf_clone(int argc, const char **argv)
{
	const char *base = base_repo_path();
	const char *url = IDF_CLONE_URL;

	argc = parse_options(argc, argv, clone_opts);

	if (!access(base, F_OK)) {
		fprintf(stderr,
			"ESP-IDF already cloned at @b{%s}\n"
			"hint: use @b{ice idf pull} to update\n",
			base);
		return 0;
	}

	{
		struct sbuf parent = SBUF_INIT;
		sbuf_addf(&parent, "%s/esp-idf", ice_home());
		if (mkdirp(parent.buf) < 0)
			die_errno("cannot create '%s'", parent.buf);
		sbuf_release(&parent);
	}

	/* Step 1: Bare clone of the main repo. */
	fprintf(stderr, "Cloning ESP-IDF into @b{%s} ...\n", base);
	if (clone_reference) {
		const char *argv_ref[] = {
		    "git",	     "clone", "--bare", "--reference",
		    clone_reference, url,     base,	NULL};
		if (run_git(NULL, argv_ref) != 0)
			die("git clone failed");
	} else {
		const char *argv_clone[] = {"git", "clone", "--bare",
					    url,   base,    NULL};
		if (run_git(NULL, argv_clone) != 0)
			die("git clone failed");
	}

	/* Step 2: Initialize submodules into the bare repo.
	 *
	 * We need a temporary checkout to run `git submodule update`
	 * which populates base.git's modules/ directory.  After that
	 * every `ice idf switch` can use those local objects.
	 */
	fprintf(stderr, "Fetching submodules ...\n");
	{
		struct sbuf tmp = SBUF_INIT;
		sbuf_addf(&tmp, "%s/esp-idf/.clone-tmp", ice_home());

		const char *clone_argv[] = {"git", "clone", "--shared",
					    base,  tmp.buf, NULL};
		if (run_git(NULL, clone_argv) != 0)
			die("temporary checkout failed");

		const char *sub_argv[] = {"git",    "submodule",   "update",
					  "--init", "--recursive", "--jobs",
					  "8",	    NULL};
		if (run_git(tmp.buf, sub_argv) != 0)
			warn("some submodules failed to fetch");

		/* Unshallow all submodules and fetch all branches
		 * so every version's commits are available locally. */
		fprintf(stderr, "Fetching full submodule history ...\n");
		const char *unshal[] = {"git",
					"submodule",
					"foreach",
					"--recursive",
					"git fetch --unshallow 2>/dev/null; "
					"git fetch origin "
					"'+refs/heads/*:refs/remotes/origin/*' "
					"2>/dev/null; true",
					NULL};
		run_git(tmp.buf, unshal);

		/* Clean up temporary checkout. */
		rmtree(tmp.buf, 0);
		rmdir(tmp.buf);
		sbuf_release(&tmp);
	}

	fprintf(stderr, "@g{done}\n");
	return 0;
}

static int cmd_idf_repo(int argc, const char **argv)
{
	const char *path;
	struct config c;
	const char *cfg_path;

	if (argc < 2)
		die("usage: ice idf repo <path>");

	path = argv[1];

	if (!is_idf_repo(path))
		die("'%s' does not look like an ESP-IDF tree "
		    "(tools/tools.json not found)",
		    path);

	/* Write to user config. */
	config_init(&c);
	config_load_file(&c, CONFIG_SCOPE_USER, user_config_path());
	config_set(&c, "idf.path", path, CONFIG_SCOPE_USER);
	cfg_path = user_config_path();
	if (!cfg_path)
		die("cannot determine user config path");
	if (config_write_file(&c, CONFIG_SCOPE_USER, cfg_path))
		die_errno("cannot write '%s'", cfg_path);
	config_release(&c);

	fprintf(stderr, "idf.path set to @b{%s}\n", path);
	return 0;
}

static const char *switch_worktree;

static const struct option switch_opts[] = {
    OPT_STRING(0, "worktree", &switch_worktree, "path",
	       "create the working tree at a custom path", NULL),
    OPT_END(),
};

static int cmd_idf_switch(int argc, const char **argv)
{
	const char *base = base_repo_path();
	const char *current = current_tree_path();
	const char *version;

	argc = parse_options(argc, argv, switch_opts);
	if (argc < 1)
		die("expected a version argument");
	version = argv[0];

	/* Use the base bare repo for objects. If user set idf.path, use that
	 * as the object source instead. */
	const char *obj_source = idf_path();
	if (!obj_source)
		obj_source = base;

	if (access(obj_source, F_OK) != 0)
		die("no ESP-IDF configured\n"
		    "hint: run @b{ice idf clone} or @b{ice idf repo <path>}");

	const char *dest = switch_worktree ? switch_worktree : current;

	fprintf(stderr, "Switching to @b{%s} in @b{%s} ...\n", version, dest);

	/* Create or update the working tree. */
	if (is_directory(dest)) {
		/* Existing tree: just update it. */
		const char *fetch[] = {"git", "fetch", "origin", version, NULL};
		const char *checkout[] = {"git", "checkout", "--force", version,
					  NULL};

		run_git(dest, fetch);
		if (run_git(dest, checkout) != 0)
			die("git checkout '%s' failed", version);
	} else {
		/* New tree: clone --shared from the base/source repo. */
		mkdirp_for_file(dest);
		const char *clone_argv[] = {"git",	"clone", "--shared",
					    "--branch", version, "--quiet",
					    obj_source, dest,	 NULL};
		if (run_git(NULL, clone_argv) != 0)
			die("git clone '%s' failed", version);
	}

	/* Populate submodules from local objects. */
	fprintf(stderr, "Populating submodules ...\n");
	if (populate_submodules(obj_source, dest, version) < 0)
		warn("some submodules could not be populated");

	fprintf(stderr, "@g{Switched to %s}\n", version);
	return 0;
}

/**
 * Check if a version string (e.g. "v5.4.1", "5.3") meets the
 * minimum major.minor threshold.  Skips a leading 'v' or
 * "release/" prefix.
 */
static int version_supported(const char *ver)
{
	int major, minor;

	if (*ver == 'v')
		ver++;
	if (sscanf(ver, "%d.%d", &major, &minor) < 2)
		return 0;
	return major > IDF_MIN_MAJOR ||
	       (major == IDF_MIN_MAJOR && minor >= IDF_MIN_MINOR);
}

static int cmd_idf_list(int argc, const char **argv)
{
	const char *repo;
	struct sbuf out = SBUF_INIT;
	size_t pos;
	char *line;

	(void)argc;
	(void)argv;

	repo = idf_path();
	if (!repo) {
		/* Fall back to bare repo for listing. */
		repo = base_repo_path();
	}
	if (access(repo, F_OK) != 0)
		die("no ESP-IDF configured\n"
		    "hint: run @b{ice idf clone} or @b{ice idf repo <path>}");

	/* Branches: master + release branches */
	{
		const char *git_argv[] = {
		    "git",	     "branch",		 "-r", "--list",
		    "origin/master", "origin/release/*", NULL};
		struct sbuf branches = SBUF_INIT;

		if (run_git_capture(repo, git_argv, &branches) == 0) {
			fprintf(stdout, "Branches:\n");
			pos = 0;
			while ((line = sbuf_getline(branches.buf, branches.len,
						    &pos))) {
				/* strip leading whitespace and "origin/" */
				while (*line == ' ')
					line++;
				if (!strncmp(line, "origin/", 7))
					line += 7;
				if (!strcmp(line, "master") ||
				    (!strncmp(line, "release/v", 9) &&
				     version_supported(line + 8) &&
				     !strchr(line + 9, '_')))
					fprintf(stdout, "  %s\n", line);
			}
		}
		sbuf_release(&branches);
	}

	/* Tags: v* sorted by version, newest first */
	{
		const char *git_argv[] = {
		    "git", "tag", "--sort=-version:refname", "-l", "v*", NULL};

		if (run_git_capture(repo, git_argv, &out) == 0) {
			fprintf(stdout, "\nTags:\n");
			pos = 0;
			while ((line = sbuf_getline(out.buf, out.len, &pos))) {
				if (version_supported(line) &&
				    !strstr(line, "-dev"))
					fprintf(stdout, "  %s\n", line);
			}
		}
		sbuf_release(&out);
	}

	return 0;
}

static int cmd_idf_pull(int argc, const char **argv)
{
	const char *base = base_repo_path();

	(void)argc;
	(void)argv;

	if (access(base, F_OK) != 0)
		die("no managed ESP-IDF repo found\n"
		    "hint: run @b{ice idf clone} first");

	/* Fetch new tags and branches into the bare repo. */
	fprintf(stderr, "Fetching updates into @b{%s} ...\n", base);
	{
		const char *fetch[] = {"git", "fetch", "--all", "--tags", NULL};
		if (run_git(base, fetch) != 0)
			die("git fetch failed");
	}

	/* Update submodule objects: re-fetch into the temp checkout's
	 * modules dir.  For now, just advise the user to re-run switch. */
	fprintf(stderr, "@g{done}\n"
			"hint: run @b{ice idf switch <version>} to update "
			"your working tree\n");
	return 0;
}

static int cmd_idf_info(int argc, const char **argv)
{
	const char *repo;
	struct sbuf head = SBUF_INIT;
	const char *git_argv[] = {"git", "describe", "--tags", "--always",
				  NULL};

	(void)argc;
	(void)argv;

	repo = idf_path();
	if (!repo) {
		fprintf(stderr, "No ESP-IDF configured.\n"
				"hint: run @b{ice idf clone} or "
				"@b{ice idf repo <path>}\n");
		return 1;
	}

	fprintf(stdout, "Path:     %s\n", repo);
	fprintf(stdout, "Base:     %s\n", base_repo_path());

	if (config_get("idf.path"))
		fprintf(stdout, "Source:   external (set via idf.path)\n");
	else
		fprintf(stdout, "Source:   managed (ice idf clone)\n");

	if (run_git_capture(repo, git_argv, &head) == 0) {
		sbuf_rtrim(&head);
		fprintf(stdout, "Version:  %s\n", head.buf);
	}

	sbuf_release(&head);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

static subcmd_fn idf_fn;

static const struct option cmd_idf_opts[] = {
    OPT_SUBCOMMAND("clone", &idf_fn, cmd_idf_clone,
		   "clone ESP-IDF into ~/.ice/esp-idf/"),
    OPT_SUBCOMMAND("repo", &idf_fn, cmd_idf_repo,
		   "point ice at an existing ESP-IDF clone"),
    OPT_SUBCOMMAND("switch", &idf_fn, cmd_idf_switch,
		   "checkout a version and update submodules"),
    OPT_SUBCOMMAND("list", &idf_fn, cmd_idf_list,
		   "list available versions (tags)"),
    OPT_SUBCOMMAND("pull", &idf_fn, cmd_idf_pull,
		   "fetch latest from upstream (managed repo)"),
    OPT_SUBCOMMAND("info", &idf_fn, cmd_idf_info,
		   "show current IDF path and version"),
    OPT_END(),
};

/* clang-format off */
static const struct cmd_manual manual = {
	.description =
	H_PARA("Manage the ESP-IDF source tree.  Use @b{ice idf clone} to "
	       "get ESP-IDF, or @b{ice idf repo <path>} to point ice at "
	       "an existing clone.  Then @b{ice idf switch <version>} to "
	       "select a release.")
	H_PARA("Run @b{ice idf <subcommand> --help} for details."),

	.examples =
	H_EXAMPLE("ice idf clone")
	H_EXAMPLE("ice idf switch v5.4")
	H_EXAMPLE("ice idf list | head -20")
	H_EXAMPLE("ice idf repo ~/work/esp-idf")
	H_EXAMPLE("ice idf info"),
};
/* clang-format on */

int cmd_idf(int argc, const char **argv)
{
	argc = parse_options_manual(argc, argv, cmd_idf_opts, &manual);
	if (idf_fn)
		return idf_fn(argc, argv);

	die("expected a subcommand. See 'ice idf --help'.");
}
