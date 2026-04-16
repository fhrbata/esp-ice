/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ice.c
 * @brief Subcommand dispatch table, global option table, and root manual.
 *
 * This file is part of libice.a so tests and any future external
 * libice consumer can reach @c ice_commands[] and friends without
 * pulling in the program's main() -- which lives in main.c.
 */
#include "ice.h"

const struct cmd_struct ice_commands[] = {
    {.name = "build", .fn = cmd_build, .summary = "build the default target"},
    {.name = "clean", .fn = cmd_clean, .summary = "remove build artifacts"},
    {.name = "cmake",
     .fn = cmd_cmake,
     .summary = "run an arbitrary cmake target"},
    {.name = "completion",
     .fn = cmd_completion,
     .summary = "print shell completion script"},
    {.name = "config",
     .fn = cmd_config,
     .summary = "inspect and modify configuration entries",
     .opts = cmd_config_opts},
    {.name = "configdep",
     .fn = cmd_configdep,
     .summary = "sdkconfig-aware compiler wrapper"},
    {.name = "flash",
     .fn = cmd_flash,
     .summary = "flash firmware to the device"},
    {.name = "fullclean",
     .fn = cmd_fullclean,
     .summary = "wipe the build directory"},
    {.name = "help", .fn = cmd_help, .summary = "show help for a subcommand"},
    {.name = "idf", .fn = cmd_idf, .summary = "manage the ESP-IDF source tree"},
    {.name = "image",
     .fn = cmd_image,
     .summary = "host-only image manipulation (elf2image, ...)"},
    {.name = "install",
     .fn = cmd_install,
     .summary = "install ESP-IDF tools from a manifest",
     .opts = cmd_install_opts},
    {.name = "ldgen",
     .fn = cmd_ldgen,
     .summary = "analyse linker fragment (.lf) files",
     .opts = cmd_ldgen_opts},
    {.name = "monitor",
     .fn = cmd_monitor,
     .summary = "display serial output from the device",
     .opts = cmd_monitor_opts},
    {.name = "menuconfig",
     .fn = cmd_menuconfig,
     .summary = "open the project configuration UI"},
    {.name = "partition-table",
     .fn = cmd_partition_table,
     .summary = "generate partition table binary from CSV",
     .opts = cmd_partition_table_opts},
    {.name = "reconfigure",
     .fn = cmd_reconfigure,
     .summary = "regenerate the build system"},
    {.name = "set-target",
     .fn = cmd_set_target,
     .summary = "switch the chip target",
     .opts = cmd_set_target_opts},
    {.name = "size",
     .fn = cmd_size,
     .summary = "analyse firmware memory usage by region",
     .opts = cmd_size_opts},
    /*
     * Hidden backend invoked by the bash/zsh/fish glue printed by
     * `ice completion`.  Kept out of help listings and top-level
     * completion output; dispatchable like any other command.
     */
    {.name = "__complete",
     .fn = cmd_complete,
     .summary = "shell completion backend (internal)",
     .hidden = 1},
    {0},
};

const char *ice_cmd_summary(const char *name)
{
	for (const struct cmd_struct *c = ice_commands; c->name; c++)
		if (!strcmp(name, c->name))
			return c->summary;
	return NULL;
}

const char *ice_global_usage[] = {
    "ice [-B <path>] [-G <name>] [-D <key=val>] [-v] [-C <dir>] [--no-color] "
    "<command> [<args>]",
    NULL,
};

/*
 * Written to by parse_options_manual(); checked in main() (in main.c)
 * after parsing.  File-scope with external linkage so the option
 * table below has stable addresses to reference and main.c can read
 * the post-parse values via extern declarations.
 */
int global_no_color;
int global_version;

const struct option ice_global_opts[] = {
    OPT_CONFIG('B', "build-dir", "core.build-dir", "path",
	       "build directory (default: build)"),
    OPT_CONFIG_LIST('D', "define", "cmake.define", "key=val",
		    "cmake cache entry (repeatable)"),
    OPT_CONFIG('G', "generator", "core.generator", "name",
	       "cmake generator (default: Ninja)"),
    OPT_BOOL(0, "no-color", &global_no_color, "disable colored output"),
    OPT_CONFIG_BOOL('v', "verbose", "core.verbose", "show full command output"),
    OPT_BOOL(0, "version", &global_version, "show version"),
    OPT_END(),
};

/* clang-format off */
const struct cmd_manual ice_root_manual = {
	.summary = "frontend for ESP-IDF projects",

	.description =
	H_PARA("@b{ice} drives the build, flash, configuration and size "
	       "tooling for ESP-IDF projects.  It replaces @b{idf.py} with "
	       "a single self-contained binary.")
	H_PARA("Subcommands wrap the underlying CMake / flashing / linker "
	       "utilities and share a common configuration system with "
	       "@b{cli > env > project > local > user > defaults} "
	       "precedence."),

	.list_commands = 1,
	.list_aliases  = 1,

	.examples =
	H_EXAMPLE("ice reconfigure")
	H_EXAMPLE("ice build")
	H_EXAMPLE("ice -C /path/to/project flash")
	H_EXAMPLE("ice help config"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice help <command>",
	       "Show the manual page for a specific command.")
	H_ITEM("ice config --help",
	       "How configuration entries and scopes work."),
};
/* clang-format on */
