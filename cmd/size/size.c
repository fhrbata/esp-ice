/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/size/size.c
 * @brief The "ice size" subcommand -- porcelain wrapper around
 * `ice idf size`.
 *
 * Resolves the map file and target chip from the active profile, then
 * delegates to cmd_idf_size() for the actual parsing and formatting.
 * Unlike build / flash, this does not go through process_run_progress:
 * the size report is a table meant to be read on the terminal, not
 * captured into a log.
 */
#include "ice.h"

/* Plumbing entry point declared in cmd/idf/size/size.c. */
int cmd_idf_size(int argc, const char **argv);

int cmd_size(int argc, const char **argv);

static const char *opt_format = "table";

/* clang-format off */
static const struct option cmd_size_opts[] = {
	OPT_STRING(0, "format", &opt_format, "fmt",
		   "output format (table)", NULL),
	OPT_END(),
};

static const struct cmd_manual manual = {
	.name = "ice size",
	.summary = "analyse firmware memory usage",

	.description =
	H_PARA("Prints a table of memory usage per chip memory region "
	       "(IRAM, DRAM, Flash, RTC fast/slow, CACHE, ...), with a "
	       "per-section breakdown (@b{.text}, @b{.data}, @b{.bss}, "
	       "@b{.rodata}, ...).  The map file and target chip are "
	       "read from the active profile's build directory.")
	H_PARA("The active profile is selected via @b{--profile}, the "
	       "@b{ICE_PROFILE} env var, or @b{project.default-profile} "
	       "in config (in that order).  The profile must already "
	       "have been built with @b{ice build}."),

	.examples =
	H_EXAMPLE("ice size")
	H_EXAMPLE("ice --profile production size")
	H_EXAMPLE("ice size --format table"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice idf size",
	       "Plumbing: run with an explicit map file and target.")
	H_ITEM("ice build",
	       "Build the firmware to produce the map file."),
};
/* clang-format on */

const struct cmd_desc cmd_size_desc = {
    .name = "size",
    .fn = cmd_size,
    .opts = cmd_size_opts,
    .manual = &manual,
    .needs = PROJECT_BUILT,
};

int cmd_size(int argc, const char **argv)
{
	const char *mapfile;
	const char *target;
	const char *size_argv[8];
	int fa = 0;

	argc = parse_options(argc, argv, &cmd_size_desc);
	if (argc > 0)
		die("too many arguments");

	mapfile = config_get("_project.mapfile");
	if (!mapfile)
		die("no map file for this profile; run @b{ice build} first");
	target = config_get("_project.target");

	size_argv[fa++] = "ice size";
	if (target) {
		size_argv[fa++] = "--target";
		size_argv[fa++] = target;
	}
	size_argv[fa++] = "--format";
	size_argv[fa++] = opt_format;
	size_argv[fa++] = mapfile;
	size_argv[fa] = NULL;

	return cmd_idf_size(fa, size_argv);
}
