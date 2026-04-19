/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/build/build.c
 * @brief The "ice build" subcommand -- build the default "all" target.
 */
#include "ice.h"

/* clang-format off */
static const struct cmd_manual build_manual = {
	.name = "ice build",
	.summary = "build the default target",

	.description =
	H_PARA("Compiles the project's @b{all} target through cmake.  "
	       "Progress is shown on a single status line and the full "
	       "command output is captured to "
	       "@b{<build-dir>/log/build.log}, surfaced on failure or "
	       "when @b{-v} / @b{core.verbose} is set.")
	H_PARA("@b{[<name>]} selects the project profile (default: "
	       "@b{default}).  The profile must already have been bound "
	       "with @b{ice init <chip> <idf> [<name>]}.  cmake re-runs "
	       "automatically when its own tracked dependencies "
	       "(top-level @b{CMakeLists.txt}, included @b{*.cmake} "
	       "files) change.  For a from-scratch rebuild, re-run "
	       "@b{ice init}."),

	.examples =
	H_EXAMPLE("ice build")
	H_EXAMPLE("ice build production")
	H_EXAMPLE("ice -v build"),

	.extras =
	H_SECTION("CONFIG")
	H_ITEM("core.verbose",
	       "If true, show full command output instead of the "
	       "progress line.")
	H_ITEM("project.<name>.build-dir",
	       "Build directory for profile @b{<name>} "
	       "(written by @b{ice init}).")

	H_SECTION("SEE ALSO")
	H_ITEM("ice init",
	       "Bind/rebind the project (also re-runs cmake from scratch).")
	H_ITEM("ice clean",
	       "Remove build artifacts without touching the cmake "
	       "configuration."),
};
/* clang-format on */

static const struct option cmd_build_opts[] = {
    OPT_POSITIONAL("[<name>]", complete_profile_names),
    OPT_END(),
};

const struct cmd_desc cmd_build_desc = {
    .name = "build",
    .fn = cmd_build,
    .opts = cmd_build_opts,
    .manual = &build_manual,
};

int cmd_build(int argc, const char **argv)
{
	const char *name;
	const char *build_dir;
	struct process proc = PROCESS_INIT;
	const char *cmake_argv[6];

	argc = parse_options(argc, argv, &cmd_build_desc);
	if (argc > 1)
		die("too many arguments");
	name = argc >= 1 ? argv[0] : "default";

	load_profile(name);
	require_project_initialized();

	build_dir = config_get("project.build-dir");
	cmake_argv[0] = "cmake";
	cmake_argv[1] = "--build";
	cmake_argv[2] = build_dir;
	cmake_argv[3] = "--target";
	cmake_argv[4] = "all";
	cmake_argv[5] = NULL;

	proc.argv = cmake_argv;
	return process_run(&proc);
}
