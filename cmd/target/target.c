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

extern const struct cmd_desc cmd_target_list_desc;
extern const struct cmd_desc cmd_target_monitor_desc;

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

/*
 * One-line summaries for the completion menu.  Kept deliberately
 * terse -- full specs live in the ESP product matrix, not here.
 */
static const struct {
	const char *name;
	const char *summary;
} chip_summaries[] = {
    {"esp32", "Xtensa dual-core, WiFi + BT"},
    {"esp32s2", "Xtensa single-core, WiFi"},
    {"esp32s3", "Xtensa dual-core, WiFi + BLE"},
    {"esp32c2", "RISC-V, WiFi 4 + BLE (cost-optimized)"},
    {"esp32c3", "RISC-V, WiFi 4 + BLE"},
    {"esp32c5", "RISC-V, WiFi 6 dual-band + BLE"},
    {"esp32c6", "RISC-V, WiFi 6 + BLE + 802.15.4"},
    {"esp32c61", "RISC-V, WiFi 6 + BLE (cost-optimized)"},
    {"esp32h2", "RISC-V, BLE + 802.15.4 (no WiFi)"},
    {"esp32p4", "RISC-V dual-core + LP, HMI-focused"},
    {"linux", "host simulation (preview)"},
    {"esp32h21", "(preview)"},
    {"esp32h4", "(preview)"},
    {"esp32s31", "(preview)"},
    {NULL, NULL},
};

const char *ice_chip_summary(const char *name)
{
	for (size_t i = 0; chip_summaries[i].name; i++)
		if (!strcmp(chip_summaries[i].name, name))
			return chip_summaries[i].summary;
	return NULL;
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
