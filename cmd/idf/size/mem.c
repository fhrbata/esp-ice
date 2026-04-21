/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mem.c
 * @brief ESP chip memory layout data.
 *
 * Static tables derived from esp-idf-size chip_info YAML files.
 * Each array entry is {name, primary_addr, length, secondary_addr}.
 */
#include "mem.h"
#include "ice.h"

/* ---- esp32 ---------------------------------------------------------- */

static const struct chip_mem_range esp32_ranges[] = {
    /* DRAM:  17*0x2000 + 4*0x8000 + 4*0x4000 = 0x52000 */
    {"DRAM", 0x3FFAE000, 0x52000, 0},
    {"IRAM", 0x40070000, 0x50000, 0},
    {"Flash Code", 0x400C2000, 0xB3E000, 0},
    {"Flash Data", 0x3F400000, 0x400000, 0},
    {"SPI DRAM", 0x3F800000, 0x400000, 0},
    {"RTC FAST", 0x3FF80000, 0x2000, 0x400C0000},
    {"RTC SLOW", 0x50000000, 0x2000, 0},
    {0},
};

/* ---- esp32c2 -------------------------------------------------------- */

static const struct chip_mem_range esp32c2_ranges[] = {
    {"DIRAM", 0x3FCA0000, 0x40000, 0x40380000},
    {"IRAM", 0x4037C000, 0x4000, 0},
    {"Flash Code", 0x42000000, 0x400000, 0},
    {"Flash Data", 0x3C000000, 0x400000, 0},
    {0},
};

/* ---- esp32c3 -------------------------------------------------------- */

static const struct chip_mem_range esp32c3_ranges[] = {
    {"DRAM", 0x3FC80000, 0x60000, 0x40380000},
    {"IRAM", 0x4037C000, 0x4000, 0},
    {"Flash Code", 0x42000000, 0x800000, 0},
    {"Flash Data", 0x3C000000, 0x800000, 0},
    {"RTC SLOW", 0x50000000, 0x2000, 0},
    {0},
};

/* ---- esp32c5 -------------------------------------------------------- */

static const struct chip_mem_range esp32c5_ranges[] = {
    {"HP SRAM", 0x40800000, 0x60000, 0x40800000},
    {"Flash", 0x42000000, 0x2000000, 0},
    {"CACHE_D", 0x42000000, 0x2000000, 0},
    {"LP SRAM", 0x50000000, 0x4000, 0},
    {0},
};

/* ---- esp32c6 -------------------------------------------------------- */

static const struct chip_mem_range esp32c6_ranges[] = {
    {"DIRAM", 0x40800000, 0x80000, 0x40800000},
    {"Flash Code", 0x42000000, 0x1000000, 0},
    {"Flash Data", 0x42000000, 0x1000000, 0},
    {"LP SRAM", 0x50000000, 0x4000, 0},
    {0},
};

/* ---- esp32c61 ------------------------------------------------------- */

static const struct chip_mem_range esp32c61_ranges[] = {
    {"DIRAM", 0x40800000, 0x50000, 0x40800000},
    {"Flash Code", 0x42000000, 0x2000000, 0},
    {"Flash Data", 0x42000000, 0x2000000, 0},
    {"LP SRAM", 0x50000000, 0x4000, 0},
    {0},
};

/* ---- esp32h2 -------------------------------------------------------- */

static const struct chip_mem_range esp32h2_ranges[] = {
    {"DIRAM", 0x40800000, 0x50000, 0x40800000},
    {"Flash Code", 0x42000000, 0x1000000, 0},
    {"Flash Data", 0x42000000, 0x1000000, 0},
    {"LP SRAM", 0x50000000, 0x1000, 0},
    {0},
};

/* ---- esp32h21 ------------------------------------------------------- */

static const struct chip_mem_range esp32h21_ranges[] = {
    {"DIRAM", 0x40800000, 0x50000, 0x40800000},
    {"Flash", 0x42000000, 0x1000000, 0},
    {"LP SRAM", 0x50000000, 0x1000, 0},
    {0},
};

/* ---- esp32h4 -------------------------------------------------------- */

static const struct chip_mem_range esp32h4_ranges[] = {
    {"DIRAM", 0x40808000, 0x60000, 0x40808000},
    {"Flash", 0x42000020, 0x2000000, 0},
    {0},
};

/* ---- esp32p4 -------------------------------------------------------- */

static const struct chip_mem_range esp32p4_ranges[] = {
    {"DIRAM", 0x4FF00000, 0xC0000, 0x4FF00000},
    {"Flash", 0x40000000, 0x4000000, 0},
    {"CACHE_D_1", 0x40000000, 0x4000000, 0},
    {"External RAM", 0x48000000, 0x4000000, 0},
    {"CACHE_D_2", 0x48000000, 0x4000000, 0},
    {"HP core RAM", 0x30100000, 0x2000, 0x30100000},
    {"LP RAM", 0x50108000, 0x8000, 0x50108000},
    {0},
};

/* ---- esp32s2 -------------------------------------------------------- */

static const struct chip_mem_range esp32s2_ranges[] = {
    /* DRAM:  3*0x2000 + 18*0x4000 = 0x4E000 */
    {"DIRAM", 0x3FFB2000, 0x4E000, 0x40022000},
    {"Flash Data", 0x3F000000, 0x400000, 0},
    {"SPI DRAM", 0x3F500000, 0xA80000, 0},
    {"Flash Code", 0x40080000, 0x780000, 0},
    {"RTC FAST", 0x40070000, 0x2000, 0x3FF9E000},
    {"RTC SLOW", 0x50000000, 0x2000, 0},
    {0},
};

/* ---- esp32s3 -------------------------------------------------------- */

static const struct chip_mem_range esp32s3_ranges[] = {
    {"IRAM", 0x40370000, 0x8000, 0},
    /* DRAM_1:  0x8000 + 6*0x10000 = 0x68000 */
    {"DIRAM", 0x3FC88000, 0x68000, 0x40378000},
    {"DRAM", 0x3FCF0000, 0x10000, 0},
    {"Flash Code", 0x42000000, 0x2000000, 0},
    {"Flash Data", 0x3C000000, 0x2000000, 0},
    {"RTC FAST", 0x3FF80000, 0x2000, 0x600FE000},
    {"RTC SLOW", 0x50000000, 0x2000, 0},
    {0},
};

/* ---- lookup table --------------------------------------------------- */

static const struct chip_info chips[] = {
    {"esp32", esp32_ranges},	 {"esp32c2", esp32c2_ranges},
    {"esp32c3", esp32c3_ranges}, {"esp32c5", esp32c5_ranges},
    {"esp32c6", esp32c6_ranges}, {"esp32c61", esp32c61_ranges},
    {"esp32h2", esp32h2_ranges}, {"esp32h21", esp32h21_ranges},
    {"esp32h4", esp32h4_ranges}, {"esp32p4", esp32p4_ranges},
    {"esp32s2", esp32s2_ranges}, {"esp32s3", esp32s3_ranges},
};

const struct chip_info *chip_find(const char *name)
{
	for (size_t i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
		if (!strcmp(name, chips[i].name))
			return &chips[i];
	}
	return NULL;
}
