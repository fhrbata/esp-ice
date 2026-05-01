/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/size/size.c
 * @brief `ice size` namespace + default summary view + shared helpers.
 *
 * The bare @b{ice size} produces the same memory-type summary as
 * @b{idf.py size}.  The pivoted views live in subdirectories:
 *
 *   ice size files        -- per-source-file breakdown
 *   ice size components   -- per-component (archive) breakdown
 *   ice size symbols COMP -- per-symbol within one component
 *   ice size deps         -- archive dependencies (Cross Reference Table)
 *
 * All paths resolve the map file + chip target from the active
 * profile and forward to @c cmd_idf_size, which does the actual
 * parsing and rendering.  This file holds the namespace dispatcher,
 * the summary handler, and two completion helpers (--format choices
 * and component names) that the subcommands share.
 */
#include "size.h"

/* ------------------------------------------------------------------ */
/* Shared helpers                                                     */
/* ------------------------------------------------------------------ */

int size_invoke(const char *fmt, const char *extra_flag, const char *extra_arg)
{
	const char *mapfile;
	const char *target;
	const char *argv[10];
	int n = 0;

	mapfile = config_get("_project.mapfile");
	if (!mapfile)
		die("no map file for this profile; run @b{ice build} first");
	target = config_get("_project.target");

	argv[n++] = "ice size";
	if (target) {
		argv[n++] = "--target";
		argv[n++] = target;
	}
	argv[n++] = "--format";
	argv[n++] = fmt;
	if (extra_flag) {
		argv[n++] = extra_flag;
		if (extra_arg)
			argv[n++] = extra_arg;
	}
	argv[n++] = mapfile;
	argv[n] = NULL;

	return cmd_idf_size(n, argv);
}

/* ------------------------------------------------------------------ */
/* Default summary view (no subcommand given)                         */
/* ------------------------------------------------------------------ */

static const char *opt_format = "table";

static const struct option cmd_size_opts[] = {
    OPT_STRING(0, "format", &opt_format, "fmt",
	       "output format: table, tree, csv, json2, raw",
	       idf_size_complete_format),
    OPT_END(),
};

/* clang-format off */
static const struct cmd_manual size_manual = {
	.name = "ice size",
	.summary = "analyse firmware memory usage",

	.description =
	H_PARA("Print memory usage broken down by chip memory region "
	       "(IRAM, DRAM, Flash, RTC fast/slow, ...) with a per-section "
	       "breakdown.  Map file and chip target are read from the "
	       "active profile's build directory.")
	H_PARA("The active profile is selected via @b{--profile}, the "
	       "@b{ICE_PROFILE} env var, or @b{project.default-profile} in "
	       "config (in that order).  The profile must already have been "
	       "built with @b{ice build}.")
	H_PARA("Pivoted views live under subcommands -- "
	       "@b{ice size files} (per-source-file), "
	       "@b{ice size components} (per-component), "
	       "@b{ice size symbols COMPONENT}, and "
	       "@b{ice size deps} (archive dependency graph)."),

	.examples =
	H_EXAMPLE("ice size")
	H_EXAMPLE("ice size --format tree")
	H_EXAMPLE("ice --profile production size")
	H_EXAMPLE("ice size files")
	H_EXAMPLE("ice size symbols esp_system"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice idf size",
	       "Plumbing: run with an explicit map file and chip target.")
	H_ITEM("ice build",
	       "Build the firmware to produce the map file."),
};
/* clang-format on */

static int cmd_size_summary(int argc, const char **argv);

static const struct cmd_desc *const size_subs[] = {
    &cmd_size_files_desc,
    &cmd_size_components_desc,
    &cmd_size_symbols_desc,
    &cmd_size_deps_desc,
    NULL,
};

const struct cmd_desc cmd_size_desc = {
    .name = "size",
    .fn = cmd_size_summary,
    .opts = cmd_size_opts,
    .manual = &size_manual,
    .subcommands = size_subs,
    .needs = PROJECT_BUILT,
};

static int cmd_size_summary(int argc, const char **argv)
{
	/*
	 * The namespace dispatcher already ran parse_options() with this
	 * descriptor before falling through to .fn -- opt_format is set,
	 * options stripped from argv.  Anything left here is a positional
	 * we don't know about (an unknown subcommand attempt); reject it
	 * with the same shape of error the dispatcher's no-fn branch
	 * would have produced.
	 */
	if (argc > 0)
		die("'%s' is not a valid 'ice size' subcommand. "
		    "See 'ice size --help'.",
		    argv[0]);
	return size_invoke(opt_format, NULL, NULL);
}
