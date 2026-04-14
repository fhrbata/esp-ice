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
#include "../../ice.h"

/* clang-format off */
static const struct cmd_manual manual = {
	.description =
	H_PARA("Launches the ncurses @b{kconfig/menuconfig} UI -- the "
	       "same TUI @b{idf.py menuconfig} opens -- so you can "
	       "browse and edit @b{sdkconfig} interactively.  Runs with "
	       "stdio connected directly to the terminal; not scriptable.")
	H_PARA("When you save, sdkconfig changes invalidate only the "
	       "translation units that actually reference the affected "
	       "@b{CONFIG_*} symbols, thanks to the @b{ice configdep} "
	       "compiler wrapper.  A plain @b{ice build} picks the "
	       "changes up automatically."),

	.examples =
	H_EXAMPLE("ice menuconfig"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice configdep",
	       "Compiler wrapper that makes incremental builds "
	       "sdkconfig-aware.")
	H_ITEM("ice reconfigure",
	       "Re-run cmake if menuconfig's results need to propagate "
	       "to cache-time decisions."),
};
/* clang-format on */

int cmd_menuconfig(int argc, const char **argv)
{
	const char *usage[] = {"ice menuconfig", NULL};
	struct option opts[] = {OPT_END()};

	parse_options_manual(argc, argv, opts, usage, &manual);
	return run_cmake_target("menuconfig", "menuconfig", 1);
}
