/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file chip.c
 * @brief ice chip identity — centralized metadata table.
 *
 * One entry per chip.  Collapses ice_supported_targets[],
 * ice_preview_targets[], chip_summaries[], ice_chip_name(), and
 * ice_chip_from_idf_name() into a single table.
 */
#include "chip.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Chip flags                                                         */

#define FLAG_SUPPORTED (1u << 0) /**< Listed in ice_supported_targets[]. */
#define FLAG_PREVIEW (1u << 1)	 /**< Listed in ice_preview_targets[]. */

/* ------------------------------------------------------------------ */
/*  Master table                                                       */

static const struct {
	const char *idf_name;
	const char *human_name;
	const char *summary;
	enum ice_chip id;
	unsigned flags;
} chip_table[] = {
    /* idf_name      human_name    summary id                   flags */
    {"esp8266", "ESP8266", NULL, ICE_CHIP_ESP8266, 0},
    {"esp32", "ESP32", "Xtensa dual-core, WiFi + BT", ICE_CHIP_ESP32,
     FLAG_SUPPORTED},
    {"esp32s2", "ESP32-S2", "Xtensa single-core, WiFi", ICE_CHIP_ESP32S2,
     FLAG_SUPPORTED},
    {"esp32s3", "ESP32-S3", "Xtensa dual-core, WiFi + BLE", ICE_CHIP_ESP32S3,
     FLAG_SUPPORTED},
    {"esp32c2", "ESP32-C2", "RISC-V, WiFi 4 + BLE (cost-optimized)",
     ICE_CHIP_ESP32C2, FLAG_SUPPORTED},
    {"esp32c3", "ESP32-C3", "RISC-V, WiFi 4 + BLE", ICE_CHIP_ESP32C3,
     FLAG_SUPPORTED},
    {"esp32c5", "ESP32-C5", "RISC-V, WiFi 6 dual-band + BLE", ICE_CHIP_ESP32C5,
     FLAG_SUPPORTED},
    {"esp32c6", "ESP32-C6", "RISC-V, WiFi 6 + BLE + 802.15.4", ICE_CHIP_ESP32C6,
     FLAG_SUPPORTED},
    {"esp32c61", "ESP32-C61", "RISC-V, WiFi 6 + BLE (cost-optimized)",
     ICE_CHIP_ESP32C61, FLAG_SUPPORTED},
    {"esp32h2", "ESP32-H2", "RISC-V, BLE + 802.15.4 (no WiFi)",
     ICE_CHIP_ESP32H2, FLAG_SUPPORTED},
    {"esp32p4", "ESP32-P4", "RISC-V dual-core + LP, HMI-focused",
     ICE_CHIP_ESP32P4, FLAG_SUPPORTED},
    {"linux", "Linux", "host simulation (preview)", ICE_CHIP_LINUX,
     FLAG_PREVIEW},
    {"esp32h21", "ESP32-H21", "(preview)", ICE_CHIP_ESP32H21, FLAG_PREVIEW},
    {"esp32h4", "ESP32-H4", "(preview)", ICE_CHIP_ESP32H4, FLAG_PREVIEW},
    {"esp32s31", "ESP32-S31", "(preview)", ICE_CHIP_ESP32S31, FLAG_PREVIEW},
};

#define CHIP_TABLE_SZ (sizeof(chip_table) / sizeof(chip_table[0]))

/* ------------------------------------------------------------------ */
/*  Lookup helpers                                                     */

const char *ice_chip_name(enum ice_chip chip)
{
	for (size_t i = 0; i < CHIP_TABLE_SZ; i++)
		if (chip_table[i].id == chip)
			return chip_table[i].human_name;
	return "unknown";
}

const char *ice_chip_idf_name(enum ice_chip chip)
{
	for (size_t i = 0; i < CHIP_TABLE_SZ; i++)
		if (chip_table[i].id == chip)
			return chip_table[i].idf_name;
	return NULL;
}

enum ice_chip ice_chip_from_idf_name(const char *name)
{
	if (!name || !*name)
		return ICE_CHIP_UNKNOWN;
	for (size_t i = 0; i < CHIP_TABLE_SZ; i++)
		if (!strcmp(chip_table[i].idf_name, name))
			return chip_table[i].id;
	return ICE_CHIP_UNKNOWN;
}

const char *ice_chip_summary(const char *idf_name)
{
	if (!idf_name || !*idf_name)
		return NULL;
	for (size_t i = 0; i < CHIP_TABLE_SZ; i++)
		if (!strcmp(chip_table[i].idf_name, idf_name))
			return chip_table[i].summary;
	return NULL;
}

/* ------------------------------------------------------------------ */
/*  Supported / preview lists (NULL-terminated IDF name arrays)       */
/*                                                                     */
/*  Mirrors esp-idf/tools/idf_py_actions/constants.py.                */

const char *const ice_supported_targets[] = {
    "esp32",   "esp32s2", "esp32c3", "esp32s3",	 "esp32c2", "esp32c6",
    "esp32h2", "esp32p4", "esp32c5", "esp32c61", NULL,
};

const char *const ice_preview_targets[] = {
    "linux", "esp32h21", "esp32h4", "esp32s31", NULL,
};
