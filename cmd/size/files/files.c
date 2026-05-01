/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/size/files/files.c
 * @brief `ice size files` -- per-source-file memory breakdown.
 *
 * Mirrors @b{idf.py size-files}.  Forwards to @c cmd_idf_size with
 * @c --files; map file and target are resolved by the shared helper
 * from the active profile.
 */
#include "cmd/size/size.h"

static const char *opt_format = "table";

static const struct option cmd_size_files_opts[] = {
    OPT_STRING(0, "format", &opt_format, "fmt",
	       "output format: table, tree, csv, json2, raw",
	       idf_size_complete_format),
    OPT_END(),
};

/* clang-format off */
static const struct cmd_manual size_files_manual = {
	.name = "ice size files",
	.summary = "per-source-file memory contributions",

	.description =
	H_PARA("Pivot the memory map by object file: every linked .obj's "
	       "contribution to each chip memory region (IRAM, DRAM, "
	       "Flash, ...) is shown as one row.  Equivalent to "
	       "@b{idf.py size-files}.")
	H_PARA("Map file and chip target are read from the active "
	       "profile's build directory; the profile must already have "
	       "been built with @b{ice build}."),

	.examples =
	H_EXAMPLE("ice size files")
	H_EXAMPLE("ice size files --format csv | sort -t, -k2 -nr"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice size",            "default summary view")
	H_ITEM("ice size components", "per-component breakdown")
	H_ITEM("ice size symbols",    "per-symbol within one component"),
};
/* clang-format on */

static int cmd_size_files(int argc, const char **argv);

const struct cmd_desc cmd_size_files_desc = {
    .name = "files",
    .fn = cmd_size_files,
    .opts = cmd_size_files_opts,
    .manual = &size_files_manual,
    .needs = PROJECT_BUILT,
};

static int cmd_size_files(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_size_files_desc);
	if (argc > 0)
		die("too many arguments");
	return size_invoke(opt_format, "--files", NULL);
}
