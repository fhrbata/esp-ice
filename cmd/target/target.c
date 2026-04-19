/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file target.c
 * @brief `ice target` -- chip-bound operations (esptool replacement).
 *
 * Today only `list` lives here (static enumeration of supported
 * chips).  This namespace will grow esptool-replacement subcommands
 * (flash, erase, reset, info, ...) as the native rewrites land --
 * each operates on a connected chip via an explicit serial port and
 * does not consume project state.
 *
 * Setting the project's chip is done by `ice init <chip> <idf>`,
 * not from this namespace.
 */
#include "ice.h"

/*
 * Mirrors esp-idf/tools/idf_py_actions/constants.py.  Preview targets
 * require --preview to enable.  Exposed via ice.h so callers (init,
 * install, completion) can reuse the same lists.
 */
const char *const ice_supported_targets[] = {
    "esp32",   "esp32s2", "esp32c3", "esp32s3",	 "esp32c2", "esp32c6",
    "esp32h2", "esp32p4", "esp32c5", "esp32c61", NULL,
};
const char *const ice_preview_targets[] = {
    "linux", "esp32h21", "esp32h4", "esp32s31", NULL,
};

/* ------------------------------------------------------------------ */
/* ice target list                                                     */
/* ------------------------------------------------------------------ */

static int cmd_target_list(int argc, const char **argv);

static const struct cmd_manual target_list_manual = {
    .name = "ice target list",
};

static const struct option cmd_target_list_opts[] = {OPT_END()};

static const struct cmd_desc cmd_target_list_desc = {
    .name = "list",
    .fn = cmd_target_list,
    .opts = cmd_target_list_opts,
    .manual = &target_list_manual,
};

static int cmd_target_list(int argc, const char **argv)
{
	parse_options(argc, argv, &cmd_target_list_desc);

	printf("Supported targets:\n");
	for (const char *const *t = ice_supported_targets; *t; t++)
		printf("  %s\n", *t);

	printf("\nPreview targets (use 'ice init --preview' to enable):\n");
	for (const char *const *t = ice_preview_targets; *t; t++)
		printf("  %s\n", *t);

	return 0;
}

/* ------------------------------------------------------------------ */
/* ice target -- namespace dispatcher                                  */
/* ------------------------------------------------------------------ */

static const struct option cmd_target_opts[] = {OPT_END()};

/* clang-format off */
static const struct cmd_manual target_manual = {
	.name = "ice target",
	.summary = "chip-bound operations (esptool replacement)",

	.description =
	H_PARA("Operations on a connected chip via an explicit serial "
	       "port -- the future home of esptool-replacement commands "
	       "(flash, erase, reset, info, ...).  Today only @b{list} "
	       "lives here; the rest land as the native rewrites do.")
	H_PARA("Setting the project's chip is done by "
	       "@b{ice init <chip> <idf>}, not from this namespace."),

	.examples =
	H_EXAMPLE("ice target list"),
};
/* clang-format on */

static const struct cmd_desc *const target_subs[] = {
    &cmd_target_list_desc,
    NULL,
};

const struct cmd_desc cmd_target_desc = {
    .name = "target",
    .opts = cmd_target_opts,
    .manual = &target_manual,
    .subcommands = target_subs,
};
