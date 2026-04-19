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
	for (const struct option *o = ice_global_opts; o->type != OPTION_END;
	     o++) {
		if (o->type == OPTION_SUBCOMMAND && o->long_opt[0] != '_')
			printf("%s\n", o->long_opt);
	}
}

int cmd_help(int argc, const char **argv)
{
	static const struct cmd_manual manual = {.name = "ice help"};
	struct option opts[] = {
	    OPT_END_COMPLETE("command", complete_help_commands)};
	struct cmd_desc cmd_desc = {.opts = opts, .manual = &manual};
	struct svec av = SVEC_INIT;

	argc = parse_options(argc, argv, &cmd_desc);

	if (argc == 0) {
		print_manual(ice_root_manual.name, &ice_root_manual,
			     ice_global_opts);
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
