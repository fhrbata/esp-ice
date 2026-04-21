/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/target.c
 * @brief `ice target` -- chip-bound operations (esptool replacement).
 *
 * Operates on a connected chip via an explicit serial port and does
 * not consume project state.  Today @b{list} and @b{monitor} live
 * here; the esptool-replacement verbs (flash, erase, reset, info,
 * ...) will land as the native rewrites do.
 *
 * Setting the project's chip is done by `ice init <chip> <idf>`,
 * not from this namespace.
 */
#include "ice.h"

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
	       "port -- plumbing commands that require no project state.")
	H_PARA("Setting the project's chip is done by "
	       "@b{ice init <chip> <idf>}, not from this namespace."),

	.examples =
	H_EXAMPLE("ice target list")
	H_EXAMPLE("ice target monitor -p /dev/ttyUSB0")
	H_EXAMPLE("ice target flash --port /dev/ttyUSB0 "
		  "0x0=bootloader.bin 0x8000=partition-table.bin "
		  "0x10000=app.bin"),
};
/* clang-format on */

extern const struct cmd_desc cmd_target_flash_desc;
extern const struct cmd_desc cmd_target_list_desc;
extern const struct cmd_desc cmd_target_monitor_desc;

static const struct cmd_desc *const target_subs[] = {
    &cmd_target_flash_desc,
    &cmd_target_monitor_desc,
    &cmd_target_list_desc,
    NULL,
};

const struct cmd_desc cmd_target_desc = {
    .name = "target",
    .opts = cmd_target_opts,
    .manual = &target_manual,
    .subcommands = target_subs,
};
