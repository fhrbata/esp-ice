/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/repo/list/list.c
 * @brief `ice repo list` -- list available versions from the reference.
 */
#include "cmd/repo/repo.h"

int cmd_repo_list(int argc, const char **argv);

/* clang-format off */
static const struct cmd_manual repo_list_manual = {
	.name = "ice repo list",
	.summary = "list available versions (branches + tags)",

	.description =
	H_PARA("List supported release branches and tags in the "
	       "reference clone.  Versions older than "
	       "@b{v5.0} are hidden."),

	.examples =
	H_EXAMPLE("ice repo list")
	H_EXAMPLE("ice repo list | head -20"),
};
/* clang-format on */

static const struct option cmd_repo_list_opts[] = {OPT_END()};

const struct cmd_desc cmd_repo_list_desc = {
    .name = "list",
    .fn = cmd_repo_list,
    .opts = cmd_repo_list_opts,
    .manual = &repo_list_manual,
};

int cmd_repo_list(int argc, const char **argv)
{
	const char *base = repo_reference_path();
	struct sbuf out = SBUF_INIT;
	size_t pos;
	char *line;

	parse_options(argc, argv, &cmd_repo_list_desc);
	repo_ensure_reference();

	{
		const char *git_argv[] = {
		    "git",	     "branch",		 "-r", "--list",
		    "origin/master", "origin/release/*", NULL};
		struct sbuf branches = SBUF_INIT;

		if (git_capture(base, git_argv, &branches) == 0) {
			fprintf(stdout, "Branches:\n");
			pos = 0;
			while ((line = sbuf_getline(branches.buf, branches.len,
						    &pos))) {
				while (*line == ' ')
					line++;
				if (!strncmp(line, "origin/", 7))
					line += 7;
				if (!strcmp(line, "master") ||
				    (!strncmp(line, "release/v", 9) &&
				     repo_version_supported(line + 8) &&
				     !strchr(line + 9, '_')))
					fprintf(stdout, "  %s\n", line);
			}
		}
		sbuf_release(&branches);
	}

	{
		const char *git_argv[] = {
		    "git", "tag", "--sort=-version:refname", "-l", "v*", NULL};

		if (git_capture(base, git_argv, &out) == 0) {
			fprintf(stdout, "\nTags:\n");
			pos = 0;
			while ((line = sbuf_getline(out.buf, out.len, &pos))) {
				if (repo_version_supported(line) &&
				    !strstr(line, "-dev"))
					fprintf(stdout, "  %s\n", line);
			}
		}
		sbuf_release(&out);
	}

	return 0;
}
