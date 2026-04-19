/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief Program entry point for the `ice` binary.
 *
 * This file is the only source NOT included in libice.a so that unit
 * tests (and any future external libice consumer) can supply their
 * own main().  Startup (config loading, colour, -C chdir) happens
 * here; profile-aware setup (PATH, project-state derivation) happens
 * in load_profile() inside each cmake-using subcommand.  Dispatch
 * lives in cmd_ice() in ice.c.
 */
#include "ice.h"

/*
 * Tiny hand-rolled scan for -C before the full parse.  -C is needed
 * early so the local config is loaded from the right cwd; the full
 * parse_options() pass handles all other flags.  -C is removed from
 * argv to avoid a second chdir attempt against the already-changed
 * cwd.
 */
static void pre_parse_chdir(int *argcp, const char **argv,
			    const char **out_chdir)
{
	int argc = *argcp;
	int dst = 1;
	int i = 1;

	*out_chdir = NULL;

	while (i < argc) {
		const char *a = argv[i];
		int drop = 0;
		int step = 1;

		if (!strcmp(a, "--") || a[0] != '-' || a[1] == '\0')
			break;

		if (!strcmp(a, "-C") && i + 1 < argc) {
			*out_chdir = argv[i + 1];
			drop = 1;
			step = 2;
		} else if (!strncmp(a, "-C", 2) && a[2]) {
			*out_chdir = a + 2;
			drop = 1;
		}

		if (!drop) {
			for (int j = 0; j < step; j++) {
				if (dst != i + j)
					argv[dst] = argv[i + j];
				dst++;
			}
		}
		i += step;
	}

	while (i < argc) {
		if (dst != i)
			argv[dst] = argv[i];
		dst++;
		i++;
	}
	argv[dst] = NULL;
	*argcp = dst;
}

int main(int argc, const char **argv)
{
	const char *chdir_to = NULL;

	/* Enable color early so die() in parse_options is colored. */
	color_init(STDERR_FILENO);

	config_load_defaults(&config);
	config_load_file(&config, CONFIG_SCOPE_USER, user_config_path());

	pre_parse_chdir(&argc, argv, &chdir_to);
	if (chdir_to && chdir(chdir_to))
		die_errno("cannot change to '%s'", chdir_to);

	config_load_file(&config, CONFIG_SCOPE_LOCAL, local_config_path());

	return cmd_ice(argc, argv);
}
