/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/clean/clean.c
 * @brief The "ice clean" subcommand -- invoke the cmake "clean" target.
 */
#include "ice.h"

/* clang-format off */
static const struct cmd_manual clean_manual = {
	.name = "ice clean",
	.summary = "remove build artifacts",

	.description =
	H_PARA("Invokes cmake's @b{clean} target to remove intermediate "
	       "object files and build outputs from the build directory "
	       "while keeping its cmake configuration "
	       "(@b{CMakeCache.txt}, generator files, cached variables) "
	       "intact.  Safe to run at any time; the next @b{ice build} "
	       "recompiles from source without reconfiguring.")
	H_PARA("The active profile is selected via @b{--profile}, the "
	       "@b{ICE_PROFILE} env var, or @b{project.default-profile} "
	       "in config (in that order)."),

	.examples =
	H_EXAMPLE("ice clean")
	H_EXAMPLE("ice --profile production clean"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice init",
	       "Re-bind the project (wipes the build dir and runs cmake "
	       "from scratch -- full reset)."),
};
/* clang-format on */

static const struct option cmd_clean_opts[] = {OPT_END()};

const struct cmd_desc cmd_clean_desc = {
    .name = "clean",
    .fn = cmd_clean,
    .opts = cmd_clean_opts,
    .manual = &clean_manual,
    .needs = PROJECT_CONFIGURED,
};

int cmd_clean(int argc, const char **argv)
{
	const char *build_dir;
	struct process proc = PROCESS_INIT;
	const char *cmake_argv[6];

	argc = parse_options(argc, argv, &cmd_clean_desc);
	if (argc > 0)
		die("too many arguments");

	build_dir = config_get("_project.build-dir");
	cmake_argv[0] = "cmake";
	cmake_argv[1] = "--build";
	cmake_argv[2] = build_dir;
	cmake_argv[3] = "--target";
	cmake_argv[4] = "clean";
	cmake_argv[5] = NULL;

	proc.argv = cmake_argv;
	return process_run(&proc);
}
