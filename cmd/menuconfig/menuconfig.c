/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/menuconfig/menuconfig.c
 * @brief The "ice menuconfig" subcommand -- invoke the cmake "menuconfig"
 * target.
 */
#include "ice.h"

/* clang-format off */
static const struct cmd_manual menuconfig_manual = {
	.name = "ice menuconfig",
	.summary = "open the project configuration UI",

	.description =
	H_PARA("Launches the ncurses @b{kconfig/menuconfig} UI -- the "
	       "same TUI @b{idf.py menuconfig} opens -- so you can "
	       "browse and edit @b{sdkconfig} interactively.  Runs with "
	       "stdio connected directly to the terminal; not scriptable.")
	H_PARA("The active profile is selected via @b{--profile}, the "
	       "@b{ICE_PROFILE} env var, or @b{project.default-profile} "
	       "in config (in that order); each profile has its own "
	       "@b{sdkconfig}.")
	H_PARA("When you save, sdkconfig changes invalidate only the "
	       "translation units that actually reference the affected "
	       "@b{CONFIG_*} symbols, thanks to the @b{ice idf configdep} "
	       "compiler wrapper.  A plain @b{ice build} picks the "
	       "changes up automatically."),

	.examples =
	H_EXAMPLE("ice menuconfig")
	H_EXAMPLE("ice --profile production menuconfig"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice idf configdep",
	       "Compiler wrapper that makes incremental builds "
	       "sdkconfig-aware.")
	H_ITEM("ice init",
	       "Re-bind the project from scratch if menuconfig's results "
	       "need to propagate to cache-time decisions."),
};
/* clang-format on */

static const struct option cmd_menuconfig_opts[] = {OPT_END()};

const struct cmd_desc cmd_menuconfig_desc = {
    .name = "menuconfig",
    .fn = cmd_menuconfig,
    .opts = cmd_menuconfig_opts,
    .manual = &menuconfig_manual,
    .needs = PROJECT_CONFIGURED,
};

int cmd_menuconfig(int argc, const char **argv)
{
	const char *build_dir;
	struct process proc = PROCESS_INIT;
	const char *cmake_argv[6];

	argc = parse_options(argc, argv, &cmd_menuconfig_desc);
	if (argc > 0)
		die("too many arguments");

	build_dir = config_get("_project.build-dir");
	cmake_argv[0] = "cmake";
	cmake_argv[1] = "--build";
	cmake_argv[2] = build_dir;
	cmake_argv[3] = "--target";
	cmake_argv[4] = "menuconfig";
	cmake_argv[5] = NULL;

	proc.argv = cmake_argv;
	return process_run(&proc);
}
