/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/cmake/cmake.c
 * @brief The "ice cmake" subcommand -- invoke an arbitrary cmake target.
 *
 * Escape hatch for targets the higher-level wrappers don't cover
 * (size, partition-table, bootloader, app, erase-flash, ...).  The
 * target name is passed through verbatim; any unknown target is
 * rejected by cmake itself.
 *
 * Runs with stdio attached so the target's output (size reports,
 * menuconfig TUI, etc.) is visible directly.  Use "ice build" if you
 * want the captured progress display for a full build.
 */
#include "ice.h"

/* clang-format off */
static const struct cmd_manual manual = {
	.name = "ice cmake",
	.summary = "run an arbitrary cmake target",

	.description =
	H_PARA("Low-level escape hatch: configures the build directory "
	       "on demand and then runs any cmake @b{<target>} verbatim.  "
	       "An unknown target is rejected by cmake, not by ice.")
	H_PARA("Output streams straight to the terminal, which is the "
	       "right behaviour for interactive or report-style targets "
	       "(@b{menuconfig}, @b{size}, @b{partition-table}, "
	       "@b{bootloader}, @b{erase-flash}, ...).  Use @b{ice build} "
	       "instead when you want ice's captured progress display "
	       "around a full compilation."),

	.examples =
	H_EXAMPLE("ice cmake app")
	H_EXAMPLE("ice cmake bootloader")
	H_EXAMPLE("ice cmake partition-table")
	H_EXAMPLE("ice cmake erase-flash"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Captured progress display for the default target.")
	H_ITEM("ice menuconfig",
	       "Shortcut for @b{ice cmake menuconfig}.")
	H_ITEM("ice flash",
	       "Shortcut for @b{ice cmake flash}."),
};
/* clang-format on */

int cmd_cmake(int argc, const char **argv)
{
	struct option opts[] = {OPT_END()};

	argc = parse_options(argc, argv, opts, &manual);

	if (argc < 1)
		die("missing target argument");
	if (argc > 1)
		die("too many arguments");

	return run_cmake_target(argv[0], argv[0], 1);
}
