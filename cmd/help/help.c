/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/help/help.c
 * @brief The "ice help" subcommand.
 *
 * With no argument, shows the top-level manual (equivalent to
 * `ice --help`).  With a subcommand name, forwards to cmd_ice()
 * with `--help` appended, so `ice help config` and `ice config --help`
 * produce the same output.  Nested: `ice help idf clone` works too.
 */
#include "ice.h"

static void complete_help_commands(void)
{
	for (const struct cmd_desc *const *p = ice_root_desc.subcommands; *p;
	     p++) {
		if ((*p)->name[0] != '_')
			printf("%s\n", (*p)->name);
	}
}

static const struct cmd_manual help_manual = {.name = "ice help"};

static const struct option cmd_help_opts[] = {
    OPT_POSITIONAL("command", complete_help_commands),
    OPT_END(),
};

const struct cmd_desc cmd_help_desc = {
    .name = "help",
    .fn = cmd_help,
    .opts = cmd_help_opts,
    .manual = &help_manual,
};

int cmd_help(int argc, const char **argv)
{
	struct svec av = SVEC_INIT;

	argc = parse_options(argc, argv, &cmd_help_desc);

	if (argc == 0) {
		print_manual(ice_root_desc.name, &ice_root_desc);
		return EXIT_SUCCESS;
	}

	/* Forward: ice help idf clone → cmd_ice("ice", "idf", "clone",
	 * "--help") */
	svec_push(&av, "ice");
	for (int i = 0; i < argc; i++)
		svec_push(&av, argv[i]);
	svec_push(&av, "--help");

	cmd_ice((int)av.nr, (const char **)av.v);

	/* cmd_ice -> ... -> parse_options -> exit(0); normally unreached. */
	svec_clear(&av);
	return EXIT_SUCCESS;
}
