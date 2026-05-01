/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/size/components/components.c
 * @brief `ice size components` -- per-component memory breakdown.
 *
 * Mirrors @b{idf.py size-components}.  Each row is one IDF component
 * (one .a archive in linker terms) with its contribution to every
 * memory region.  Forwards to @c cmd_idf_size with @c --archives;
 * the porcelain says "components" because that's the IDF-level term
 * users think in.
 */
#include "cmd/size/size.h"

static const char *opt_format = "table";

static const struct option cmd_size_components_opts[] = {
    OPT_STRING(0, "format", &opt_format, "fmt",
	       "output format: table, tree, csv, json2, raw",
	       idf_size_complete_format),
    OPT_END(),
};

/* clang-format off */
static const struct cmd_manual size_components_manual = {
	.name = "ice size components",
	.summary = "per-component memory contributions",

	.description =
	H_PARA("Pivot the memory map by component (linker archive): each "
	       "row is one IDF component with its contribution to every "
	       "chip memory region.  Equivalent to @b{idf.py size-components}.")
	H_PARA("Map file and chip target are read from the active "
	       "profile's build directory; the profile must already have "
	       "been built with @b{ice build}."),

	.examples =
	H_EXAMPLE("ice size components")
	H_EXAMPLE("ice size components --format tree")
	H_EXAMPLE("ice size components --format csv > sizes.csv"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice size",         "default summary view")
	H_ITEM("ice size files",   "per-source-file breakdown")
	H_ITEM("ice size symbols", "per-symbol within one component"),
};
/* clang-format on */

static int cmd_size_components(int argc, const char **argv);

const struct cmd_desc cmd_size_components_desc = {
    .name = "components",
    .fn = cmd_size_components,
    .opts = cmd_size_components_opts,
    .manual = &size_components_manual,
    .needs = PROJECT_BUILT,
};

static int cmd_size_components(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_size_components_desc);
	if (argc > 0)
		die("too many arguments");
	return size_invoke(opt_format, "--archives", NULL);
}
