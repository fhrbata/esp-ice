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
	H_PARA("@b{[<name>]} selects the project profile (default: "
	       "@b{default}); each profile has its own @b{sdkconfig}.")
	H_PARA("When you save, sdkconfig changes invalidate only the "
	       "translation units that actually reference the affected "
	       "@b{CONFIG_*} symbols, thanks to the @b{ice configdep} "
	       "compiler wrapper.  A plain @b{ice build} picks the "
	       "changes up automatically."),

	.examples =
	H_EXAMPLE("ice menuconfig")
	H_EXAMPLE("ice menuconfig production"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice configdep",
	       "Compiler wrapper that makes incremental builds "
	       "sdkconfig-aware.")
	H_ITEM("ice init",
	       "Re-bind the project from scratch if menuconfig's results "
	       "need to propagate to cache-time decisions."),
};
/* clang-format on */

static const struct option cmd_menuconfig_opts[] = {
    OPT_POSITIONAL("[<name>]", complete_profile_names),
    OPT_END(),
};

const struct cmd_desc cmd_menuconfig_desc = {
    .name = "menuconfig",
    .fn = cmd_menuconfig,
    .opts = cmd_menuconfig_opts,
    .manual = &menuconfig_manual,
};

int cmd_menuconfig(int argc, const char **argv)
{
	const char *name;

	argc = parse_options(argc, argv, &cmd_menuconfig_desc);
	if (argc > 1)
		die("too many arguments");
	name = argc >= 1 ? argv[0] : "default";

	load_profile(name);
	return run_cmake_target("menuconfig", "menuconfig", 1);
}
