/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/repo/info/info.c
 * @brief `ice repo info` -- show reference and checkout status.
 */
#include "cmd/repo/repo.h"

int cmd_repo_info(int argc, const char **argv);

/* clang-format off */
static const struct cmd_manual repo_info_manual = {
	.name = "ice repo info",
	.summary = "show reference and checkout status",

	.description =
	H_PARA("Show the ice-managed reference path and its current "
	       "HEAD, followed by every named checkout under "
	       "@b{~/.ice/checkouts/} with its own HEAD.  Ends with a "
	       "reminder that the reference must not be used as a "
	       "working tree -- create a checkout instead."),

	.examples =
	H_EXAMPLE("ice repo info"),
};
/* clang-format on */

static const struct option cmd_repo_info_opts[] = {OPT_END()};

const struct cmd_desc cmd_repo_info_desc = {
    .name = "info",
    .fn = cmd_repo_info,
    .opts = cmd_repo_info_opts,
    .manual = &repo_info_manual,
};

int cmd_repo_info(int argc, const char **argv)
{
	const char *base = repo_reference_path();
	struct sbuf head = SBUF_INIT;
	struct svec names = SVEC_INIT;

	parse_options(argc, argv, &cmd_repo_info_desc);

	if (access(base, F_OK) != 0) {
		fprintf(stderr, "No ESP-IDF reference configured.\n");
		hint("run @b{ice repo clone}");
		return 1;
	}

	printf("Reference: %s\n", base);
	{
		const char *git_argv[] = {"git", "describe", "--tags",
					  "--always", NULL};
		if (git_capture(base, git_argv, &head) == 0) {
			sbuf_rtrim(&head);
			printf("HEAD:      %s\n", head.buf);
		}
	}
	sbuf_release(&head);

	printf("\nCheckouts under %s:\n", repo_checkouts_path());
	repo_collect_checkouts(&names);
	if (!names.nr) {
		printf("  (none)\n");
	} else {
		for (size_t i = 0; i < names.nr; i++) {
			struct sbuf full = SBUF_INIT;
			struct sbuf ver = SBUF_INIT;
			const char *git_argv[] = {"git", "describe", "--tags",
						  "--always", NULL};

			sbuf_addf(&full, "%s/%s", repo_checkouts_path(),
				  names.v[i]);
			if (git_capture(full.buf, git_argv, &ver) == 0) {
				sbuf_rtrim(&ver);
				printf("  %-20s %s\n", names.v[i], ver.buf);
			} else {
				printf("  %-20s (no HEAD)\n", names.v[i]);
			}
			sbuf_release(&full);
			sbuf_release(&ver);
		}
	}
	svec_clear(&names);

	printf("\nThe reference is ice-managed; do not work in it.\n"
	       "Use @b{ice repo checkout <ref> <name>} for a working copy.\n");

	return 0;
}
