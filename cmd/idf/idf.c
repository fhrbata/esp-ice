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

#include <string.h>

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

/**
 * Return the managed repo path (~/.ice/esp-idf/repo).
 * Backed by static storage.
 */
static const char *managed_repo_path(void)
{
	static struct sbuf path = SBUF_INIT;

	if (!path.len)
		sbuf_addf(&path, "%s/esp-idf/repo", ice_home());
	return path.buf;
}

/**
 * Return the active IDF path: idf.path config > managed repo fallback.
 * Returns NULL if neither is available.
 */
static const char *idf_path(void)
{
	const char *configured = config_get("idf.path");

	if (configured && *configured)
		return configured;

	/* Fall back to managed repo if it exists. */
	if (!access(managed_repo_path(), F_OK))
		return managed_repo_path();

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

/* ------------------------------------------------------------------ */
/* Subcommands                                                         */
/* ------------------------------------------------------------------ */

static int cmd_idf_clone(int argc, const char **argv)
{
	const char *dest = managed_repo_path();
	const char *url = IDF_CLONE_URL;

	(void)argc;
	(void)argv;

	if (!access(dest, F_OK)) {
		fprintf(stderr,
			"ESP-IDF already cloned at @b{%s}\n"
			"hint: use @b{ice idf pull} to update\n",
			dest);
		return 0;
	}

	fprintf(stderr, "Cloning ESP-IDF into @b{%s} ...\n", dest);

	{
		struct sbuf parent = SBUF_INIT;
		sbuf_addf(&parent, "%s/esp-idf", ice_home());
		if (mkdirp(parent.buf) < 0)
			die_errno("cannot create '%s'", parent.buf);
		sbuf_release(&parent);
	}

	const char *git_argv[] = {"git", "clone", "--filter=blob:none",
				  url,	 dest,	  NULL};
	int rc = run_git(NULL, git_argv);
	if (rc != 0)
		die("git clone failed");

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

static int cmd_idf_switch(int argc, const char **argv)
{
	const char *repo;
	const char *version;
	int rc;

	if (argc < 2)
		die("usage: ice idf switch <version>");

	version = argv[1];
	repo = idf_path();
	if (!repo)
		die("no ESP-IDF configured\n"
		    "hint: run @b{ice idf clone} or @b{ice idf repo <path>}");

	fprintf(stderr, "Switching @b{%s} to @b{%s} ...\n", repo, version);

	{
		const char *checkout[] = {"git", "checkout", version, NULL};
		rc = run_git(repo, checkout);
		if (rc != 0)
			die("git checkout '%s' failed", version);
	}

	fprintf(stderr, "Updating submodules ...\n");
	{
		const char *submodule[] = {"git",    "submodule",   "update",
					   "--init", "--recursive", "--depth",
					   "1",	     NULL};
		rc = run_git(repo, submodule);
		if (rc != 0)
			die("git submodule update failed");
	}

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
	if (!repo)
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
	const char *managed = managed_repo_path();
	const char *repo;
	int rc;

	(void)argc;
	(void)argv;

	repo = idf_path();
	if (!repo)
		die("no ESP-IDF configured\n"
		    "hint: run @b{ice idf clone} or @b{ice idf repo <path>}");

	if (strcmp(repo, managed) != 0)
		die("pull only works on the managed repo at '%s'\n"
		    "hint: your idf.path points to '%s'; manage it yourself",
		    managed, repo);

	fprintf(stderr, "Fetching updates ...\n");
	{
		const char *fetch[] = {"git", "fetch", "--tags", NULL};
		rc = run_git(repo, fetch);
		if (rc != 0)
			die("git fetch failed");
	}

	fprintf(stderr, "@g{done}\n");
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

	fprintf(stdout, "Path:    %s\n", repo);

	if (strcmp(repo, managed_repo_path()) == 0)
		fprintf(stdout, "Managed: yes\n");
	else
		fprintf(stdout, "Managed: no (set via idf.path)\n");

	if (run_git_capture(repo, git_argv, &head) == 0) {
		sbuf_rtrim(&head);
		fprintf(stdout, "Version: %s\n", head.buf);
	}

	sbuf_release(&head);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

struct idf_sub {
	const char *name;
	int (*fn)(int argc, const char **argv);
	const char *summary;
};

static const struct idf_sub idf_subs[] = {
    {"clone", cmd_idf_clone, "clone ESP-IDF into ~/.ice/esp-idf/repo"},
    {"repo", cmd_idf_repo, "point ice at an existing ESP-IDF clone"},
    {"switch", cmd_idf_switch, "checkout a version and update submodules"},
    {"list", cmd_idf_list, "list available versions (tags)"},
    {"pull", cmd_idf_pull, "fetch latest from upstream (managed repo)"},
    {"info", cmd_idf_info, "show current IDF path and version"},
    {NULL, NULL, NULL},
};

static const char *idf_usage[] = {
    "ice idf <subcommand> [<args>]",
    NULL,
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

	.extras =
	H_SECTION("SUBCOMMANDS")
	H_ITEM("clone",
	       "Clone ESP-IDF into @b{~/.ice/esp-idf/repo}.  Uses "
	       "@b{--filter=blob:none} for a faster initial clone.")
	H_ITEM("repo <path>",
	       "Tell ice to use an existing ESP-IDF clone at @b{<path>}.  "
	       "Saves the path in the user config (@b{~/.iceconfig}).")
	H_ITEM("switch <version>",
	       "Check out a tag or branch and run "
	       "@b{git submodule update --init --recursive}.")
	H_ITEM("list",
	       "List version tags from the configured repo, newest first.")
	H_ITEM("pull",
	       "Fetch the latest tags and commits from upstream.  "
	       "Only works on the managed repo (cloned by ice).")
	H_ITEM("info",
	       "Show the active ESP-IDF path, whether it is managed by "
	       "ice, and the current version.")
};
/* clang-format on */

static void print_subs(FILE *fp)
{
	fprintf(fp, "Subcommands:\n");
	for (const struct idf_sub *s = idf_subs; s->name; s++)
		fprintf(fp, "  %-12s  %s\n", s->name, s->summary);
}

int cmd_idf(int argc, const char **argv)
{
	if (argc >= 2 && (!strcmp(argv[1], "--help") ||
			  !strcmp(argv[1], "-h") || !strcmp(argv[1], "help"))) {
		print_manual(argv[0], &manual, NULL, idf_usage);
		return 0;
	}

	if (argc < 2) {
		fprintf(stderr, "usage: ice idf <subcommand> [<args>]\n");
		print_subs(stderr);
		return 1;
	}

	for (const struct idf_sub *s = idf_subs; s->name; s++) {
		if (!strcmp(argv[1], s->name))
			return s->fn(argc - 1, argv + 1);
	}

	fprintf(stderr,
		"ice idf: '%s' is not a subcommand. "
		"See 'ice idf --help'.\n",
		argv[1]);
	print_subs(stderr);
	return 1;
}
