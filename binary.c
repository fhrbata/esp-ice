/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file binary.c
 * @brief Shared tables and helpers for the ESP flash image format.
 *
 * Public surface is documented in @ref binary.h.  All per-chip data
 * here is transcribed from esptool's @c esptool/targets/esp*.py and
 * cross-checked against @c esptool/bin_image.py; any drift between
 * esptool and the tables below would show up in the golden-image
 * diff test (t/0012-elf2image-esptool.t).
 */
#include "binary.h"
#include "ice.h"

/* ------------------------------------------------------------------ */
/* Per-chip address range tables                                      */
/* ------------------------------------------------------------------ */

struct seg_range {
	uint32_t vaddr_lo;
	uint32_t vaddr_end; /* exclusive, matches esptool MAP_END convention */
	enum bin_seg_type type;
};

#define SEG_END {0, 0, BIN_SEG_UNKNOWN}

static const struct seg_range r_esp32[] = {
    {0x3F400000u, 0x3F800000u, BIN_SEG_DROM},
    {0x3FF80000u, 0x3FF82000u, BIN_SEG_RTC_DATA},
    {0x3FFAE000u, 0x40000000u, BIN_SEG_DRAM},
    {0x40070000u, 0x40080000u, BIN_SEG_IRAM},
    {0x40080000u, 0x400A0000u, BIN_SEG_IRAM},
    {0x400D0000u, 0x40400000u, BIN_SEG_IROM},
    {0x400C0000u, 0x400C2000u, BIN_SEG_RTC_DATA},
    {0x50000000u, 0x50002000u, BIN_SEG_RTC_DATA},
    SEG_END,
};

static const struct seg_range r_esp32s2[] = {
    {0x3F000000u, 0x3F3F0000u, BIN_SEG_DROM},
    {0x3FFB0000u, 0x40000000u, BIN_SEG_DRAM},
    {0x40020000u, 0x40070000u, BIN_SEG_IRAM},
    {0x40080000u, 0x40B80000u, BIN_SEG_IROM},
    {0x50000000u, 0x50002000u, BIN_SEG_RTC_DATA},
    SEG_END,
};

static const struct seg_range r_esp32s3[] = {
    {0x3C000000u, 0x3E000000u, BIN_SEG_DROM},
    {0x3FC88000u, 0x3FD00000u, BIN_SEG_DRAM},
    {0x40370000u, 0x403E0000u, BIN_SEG_IRAM},
    {0x42000000u, 0x44000000u, BIN_SEG_IROM},
    {0x50000000u, 0x50002000u, BIN_SEG_RTC_DATA},
    {0x600FE000u, 0x60100000u, BIN_SEG_RTC_DATA},
    SEG_END,
};

static const struct seg_range r_esp32c2[] = {
    {0x3C000000u, 0x3C400000u, BIN_SEG_DROM},
    {0x3FCA0000u, 0x3FCE0000u, BIN_SEG_DRAM},
    {0x4037C000u, 0x403C0000u, BIN_SEG_IRAM},
    {0x42000000u, 0x42400000u, BIN_SEG_IROM},
    SEG_END,
};

static const struct seg_range r_esp32c3[] = {
    {0x3C000000u, 0x3C800000u, BIN_SEG_DROM},
    {0x3FC80000u, 0x3FCE0000u, BIN_SEG_DRAM},
    {0x4037C000u, 0x403E0000u, BIN_SEG_IRAM},
    {0x42000000u, 0x42800000u, BIN_SEG_IROM},
    {0x50000000u, 0x50002000u, BIN_SEG_RTC_DATA},
    SEG_END,
};

/* C5 / P4: unified DROM==IROM and DRAM==IRAM windows. */
static const struct seg_range r_esp32c5[] = {
    {0x42000000u, 0x44000000u, BIN_SEG_DROM},
    {0x40800000u, 0x40860000u, BIN_SEG_DRAM},
    {0x50000000u, 0x50004000u, BIN_SEG_RTC_DATA},
    SEG_END,
};

static const struct seg_range r_esp32c6[] = {
    {0x42000000u, 0x42800000u, BIN_SEG_IROM},
    {0x42800000u, 0x43000000u, BIN_SEG_DROM},
    {0x40800000u, 0x40880000u, BIN_SEG_DRAM},
    {0x50000000u, 0x50004000u, BIN_SEG_RTC_DATA},
    SEG_END,
};

/* H2 inherits C6's memory map in esptool. */
static const struct seg_range r_esp32h2[] = {
    {0x42000000u, 0x42800000u, BIN_SEG_IROM},
    {0x42800000u, 0x43000000u, BIN_SEG_DROM},
    {0x40800000u, 0x40880000u, BIN_SEG_DRAM},
    {0x50000000u, 0x50004000u, BIN_SEG_RTC_DATA},
    SEG_END,
};

static const struct seg_range r_esp32p4[] = {
    {0x40000000u, 0x4C000000u, BIN_SEG_DROM},
    {0x4FF00000u, 0x4FFA0000u, BIN_SEG_DRAM},
    {0x50108000u, 0x50110000u, BIN_SEG_RTC_DATA},
    SEG_END,
};

/* ------------------------------------------------------------------ */
/* Per-chip flash-freq tables                                         */
/* ------------------------------------------------------------------ */

struct kv_u8 {
	const char *k;
	uint8_t v;
};

static const struct kv_u8 flash_modes[] = {
    {"qio", 0}, {"qout", 1}, {"dio", 2}, {"dout", 3}, {NULL, 0},
};

static const struct kv_u8 flash_sizes[] = {
    {"1MB", 0x00},  {"2MB", 0x10},   {"4MB", 0x20},
    {"8MB", 0x30},  {"16MB", 0x40},  {"32MB", 0x50},
    {"64MB", 0x60}, {"128MB", 0x70}, {NULL, 0},
};

static const struct kv_u8 freq_esp32[] = {
    {"80m", 0xF}, {"40m", 0x0}, {"26m", 0x1}, {"20m", 0x2}, {NULL, 0},
};
/* Chips that share ESP32's freq table. */
#define freq_esp32s2 freq_esp32
#define freq_esp32s3 freq_esp32
#define freq_esp32c3 freq_esp32
#define freq_esp32p4 freq_esp32

static const struct kv_u8 freq_esp32c2[] = {
    {"60m", 0xF}, {"30m", 0x0}, {"20m", 0x1}, {"15m", 0x2}, {NULL, 0},
};

static const struct kv_u8 freq_esp32c5[] = {
    {"80m", 0xF},
    {"40m", 0x0},
    {"20m", 0x2},
    {NULL, 0},
};

/* ESP32-C6 ROM workaround: 80m and 40m both map to 0x0. */
static const struct kv_u8 freq_esp32c6[] = {
    {"80m", 0x0},
    {"40m", 0x0},
    {"20m", 0x2},
    {NULL, 0},
};

static const struct kv_u8 freq_esp32h2[] = {
    {"48m", 0xF}, {"24m", 0x0}, {"16m", 0x1}, {"12m", 0x2}, {NULL, 0},
};

/* ------------------------------------------------------------------ */
/* Top-level chip table                                               */
/* ------------------------------------------------------------------ */

struct chip_info {
	const char *name;
	uint16_t chip_id;
	const struct seg_range *ranges;
	const struct kv_u8 *freqs;
};

static const struct chip_info chips[BIN_CHIP_MAX] = {
    [BIN_CHIP_ESP32] = {"esp32", 0x0000, r_esp32, freq_esp32},
    [BIN_CHIP_ESP32S2] = {"esp32s2", 0x0002, r_esp32s2, freq_esp32s2},
    [BIN_CHIP_ESP32S3] = {"esp32s3", 0x0009, r_esp32s3, freq_esp32s3},
    [BIN_CHIP_ESP32C2] = {"esp32c2", 0x000C, r_esp32c2, freq_esp32c2},
    [BIN_CHIP_ESP32C3] = {"esp32c3", 0x0005, r_esp32c3, freq_esp32c3},
    [BIN_CHIP_ESP32C5] = {"esp32c5", 0x0017, r_esp32c5, freq_esp32c5},
    [BIN_CHIP_ESP32C6] = {"esp32c6", 0x000D, r_esp32c6, freq_esp32c6},
    [BIN_CHIP_ESP32H2] = {"esp32h2", 0x0010, r_esp32h2, freq_esp32h2},
    [BIN_CHIP_ESP32P4] = {"esp32p4", 0x0012, r_esp32p4, freq_esp32p4},
};

/* ------------------------------------------------------------------ */
/* Chip-name helpers                                                  */
/* ------------------------------------------------------------------ */

enum bin_chip bin_chip_by_name(const char *name)
{
	if (name == NULL)
		return BIN_CHIP_MAX;
	for (int i = 0; i < BIN_CHIP_MAX; i++)
		if (!strcmp(chips[i].name, name))
			return (enum bin_chip)i;
	return BIN_CHIP_MAX;
}

const char *bin_chip_name(enum bin_chip chip)
{
	if ((unsigned)chip >= (unsigned)BIN_CHIP_MAX)
		return "?";
	return chips[chip].name;
}

const char *const *bin_chip_names(void)
{
	static const char *names[BIN_CHIP_MAX + 1];
	static int initialised;

	if (!initialised) {
		for (int i = 0; i < BIN_CHIP_MAX; i++)
			names[i] = chips[i].name;
		names[BIN_CHIP_MAX] = NULL;
		initialised = 1;
	}
	return names;
}

uint16_t bin_chip_id(enum bin_chip chip)
{
	if ((unsigned)chip >= (unsigned)BIN_CHIP_MAX)
		return 0xFFFFu;
	return chips[chip].chip_id;
}

enum bin_chip bin_chip_by_id(uint16_t chip_id)
{
	for (int i = 0; i < BIN_CHIP_MAX; i++)
		if (chips[i].chip_id == chip_id)
			return (enum bin_chip)i;
	return BIN_CHIP_MAX;
}

/* ------------------------------------------------------------------ */
/* Segment classification                                             */
/* ------------------------------------------------------------------ */

enum bin_seg_type bin_classify(enum bin_chip chip, uint32_t vaddr)
{
	if ((unsigned)chip >= (unsigned)BIN_CHIP_MAX)
		return BIN_SEG_UNKNOWN;

	const struct seg_range *r = chips[chip].ranges;

	for (; r->type != BIN_SEG_UNKNOWN; r++)
		if (vaddr >= r->vaddr_lo && vaddr < r->vaddr_end)
			return r->type;
	return BIN_SEG_UNKNOWN;
}

const char *bin_seg_type_name(enum bin_seg_type t)
{
	switch (t) {
	case BIN_SEG_DROM:
		return "DROM";
	case BIN_SEG_IROM:
		return "IROM";
	case BIN_SEG_DRAM:
		return "DRAM";
	case BIN_SEG_IRAM:
		return "IRAM";
	case BIN_SEG_RTC_DATA:
		return "RTC";
	case BIN_SEG_UNKNOWN:
		break;
	}
	return "?";
}

/* ------------------------------------------------------------------ */
/* Flash-parameter encoding                                           */
/* ------------------------------------------------------------------ */

static uint8_t kv_encode(const struct kv_u8 *tbl, const char *key,
			 const char *what, const char *chip_name)
{
	if (key == NULL)
		die("binary: %s is not set (chip=%s)", what,
		    chip_name ? chip_name : "?");
	for (; tbl->k != NULL; tbl++)
		if (!strcmp(tbl->k, key))
			return tbl->v;
	die("binary: unsupported %s '%s'%s%s", what, key,
	    chip_name ? " for chip " : "", chip_name ? chip_name : "");
	return 0; /* unreachable */
}

static const char *kv_decode(const struct kv_u8 *tbl, uint8_t v)
{
	/* On a tie (e.g. ESP32-C6 where 80m and 40m both encode to
	 * 0x0), return the first match; the table order puts the
	 * higher-frequency name first which matches what users type. */
	for (; tbl->k != NULL; tbl++)
		if (tbl->v == v)
			return tbl->k;
	return "?";
}

uint8_t bin_flash_mode_byte(const char *mode)
{
	return kv_encode(flash_modes, mode, "flash-mode", NULL);
}

uint8_t bin_flash_size_byte(const char *size)
{
	return kv_encode(flash_sizes, size, "flash-size", NULL);
}

uint8_t bin_flash_freq_byte(enum bin_chip chip, const char *freq)
{
	if ((unsigned)chip >= (unsigned)BIN_CHIP_MAX)
		die("binary: invalid chip (%d)", (int)chip);
	return kv_encode(chips[chip].freqs, freq, "flash-freq",
			 chips[chip].name);
}

const char *bin_flash_mode_str(uint8_t b) { return kv_decode(flash_modes, b); }

const char *bin_flash_size_str(uint8_t high_nibble)
{
	return kv_decode(flash_sizes, high_nibble & 0xF0u);
}

const char *bin_flash_freq_str(enum bin_chip chip, uint8_t low_nibble)
{
	if ((unsigned)chip >= (unsigned)BIN_CHIP_MAX)
		return "?";
	return kv_decode(chips[chip].freqs, low_nibble & 0x0Fu);
}
