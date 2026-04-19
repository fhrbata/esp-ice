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
static const struct cmd_manual manual = {
	.name = "ice clean",
	.summary = "remove build artifacts",

	.description =
	H_PARA("Invokes cmake's @b{clean} target to remove intermediate "
	       "object files and build outputs from the build directory "
	       "while keeping its cmake configuration "
	       "(@b{CMakeCache.txt}, generator files, cached variables) "
	       "intact.  Safe to run at any time; the next @b{ice build} "
	       "recompiles from source without reconfiguring."),

	.examples =
	H_EXAMPLE("ice clean"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice reconfigure",
	       "Force cmake to re-run configuration from scratch "
	       "(full reset when combined with a fresh @b{core.build-dir})."),
};
/* clang-format on */

int cmd_clean(int argc, const char **argv)
{
	struct option opts[] = {OPT_END()};
	struct cmd_desc cmd_desc = {.opts = opts, .manual = &manual};

	parse_options(argc, argv, &cmd_desc);
	return run_cmake_target("clean", "clean", 0);
}
