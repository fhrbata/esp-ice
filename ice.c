/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ice.c
 * @brief Main entry point and subcommand dispatch.
 *
 * Parses global options, then maps subcommand names (e.g. "build")
 * to handler functions, similar to how git.c dispatches "commit", etc.
 */
#include "ice.h"

const char *opt_build_dir = "build";
const char *opt_generator = "Ninja";
struct svec opt_define = SVEC_INIT;
int opt_verbose;

struct cmd_struct {
	const char *name;
	int (*fn)(int argc, const char **argv);
};

static struct cmd_struct commands[] = {
	{"build", cmd_build},
	{"clean", cmd_clean},
	{"configdep", cmd_configdep},
	{"flash", cmd_flash},
	{"ldgen", cmd_ldgen},
	{"menuconfig", cmd_menuconfig},
	{"reconfigure", cmd_reconfigure},
	{"size", cmd_size},
};

static struct cmd_struct *find_command(const char *name)
{
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (!strcmp(name, commands[i].name))
			return &commands[i];
	}
	return NULL;
}

static void list_commands(void)
{
	fprintf(stderr, "\navailable commands:\n");
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		fprintf(stderr, "   %s\n", commands[i].name);
}

static const char *global_usage[] = {
	"ice [-B <path>] [-G <name>] [-D <key=val>] [-v] [-C <dir>] [--no-color] <command> [<args>]",
	NULL,
};

int main(int argc, const char **argv)
{
	const char *dir = NULL;
	int no_color = 0;
	struct cmd_struct *cmd;

	int version = 0;

	struct option global_opts[] = {
		OPT_STRING('B', "build-dir", &opt_build_dir, "path",
			   "build directory (default: build)"),
		OPT_STRING('C', NULL, &dir, "dir",
			   "change to directory before doing anything"),
		OPT_STRING_LIST('D', "define", &opt_define, "key=val",
			   "cmake cache entry (repeatable)"),
		OPT_STRING('G', "generator", &opt_generator, "name",
			   "cmake generator (default: Ninja)"),
		OPT_BOOL(0, "no-color", &no_color,
			 "disable colored output"),
		OPT_BOOL('v', "verbose", &opt_verbose,
			 "show full command output"),
		OPT_BOOL(0, "version", &version,
			 "show version"),
		OPT_END(),
	};

	/* Enable color early so die() in parse_options is colored. */
	color_init(STDERR_FILENO);

	argc = parse_options(argc, argv, global_opts, global_usage);

	if (no_color)
		use_color = 0;

	if (version) {
		printf("%sice %s\n", use_vt ? "\xf0\x9f\xa7\x8a " : "",
		       VERSION);
		return EXIT_SUCCESS;
	}

	if (dir && chdir(dir))
		die_errno("cannot change to '%s'", dir);

	if (argc < 1) {
		list_commands();
		return EXIT_FAILURE;
	}

	cmd = find_command(argv[0]);
	if (!cmd) {
		fprintf(stderr, "ice: '%s' is not a command\n", argv[0]);
		list_commands();
		return EXIT_FAILURE;
	}

	return cmd->fn(argc, argv);
}
