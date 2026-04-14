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
 * `ice --help`).  With a subcommand name, dispatches to that
 * subcommand with `--help` appended, so `ice help config` and
 * `ice config --help` produce the same output.
 */
#include "../../ice.h"

int cmd_help(int argc, const char **argv)
{
	const char *usage[] = { "ice help [<command>]", NULL };
	struct option opts[] = { OPT_END() };
	const struct cmd_struct *cmd;
	const char *forward_argv[3];

	argc = parse_options(argc, argv, opts, usage);

	/* `ice help` -> identical to `ice --help`. */
	if (argc == 0) {
		print_manual("ice", &ice_root_manual, ice_global_opts,
			     ice_global_usage);
		return EXIT_SUCCESS;
	}

	if (argc != 1)
		die("usage: ice help [<command>]");

	cmd = NULL;
	for (const struct cmd_struct *c = ice_commands; c->name; c++) {
		if (!strcmp(c->name, argv[0])) {
			cmd = c;
			break;
		}
	}
	if (!cmd)
		die("'%s' is not a command", argv[0]);

	forward_argv[0] = cmd->name;
	forward_argv[1] = "--help";
	forward_argv[2] = NULL;
	return cmd->fn(2, forward_argv);
}
