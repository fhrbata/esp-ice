/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/elf2image/elf2image.c
 * @brief "ice elf2image" subcommand.
 *
 * Drop-in replacement for `esptool elf2image`.  Accepts the same flags
 * that ESP-IDF's cmake build system passes to esptool so that the
 * build.ninja patcher in cmake.c can transparently redirect the call.
 *
 * Usage (mirrors esptool elf2image):
 *   ice elf2image --chip <chip>
 *                 [--flash-mode <mode>]
 *                 [--flash-freq <freq>]
 *                 [--flash-size <size>]
 *                 [--elf-sha256-offset <hex>]
 *                 [--min-rev-full <n>]
 *                 [--max-rev-full <n>]
 *                 [--secure-pad-v2]
 *                 -o <output.bin>
 *                 <input.elf>
 */
#include "esp_loader_elf.h"
#include "esp_loader_error.h"
#include "fs.h"
#include "ice.h"

static const char *e2i_usage[] = {
    "ice elf2image --chip <chip> -o <output.bin> [options] <input.elf>",
    NULL,
};

/* clang-format off */
static const struct cmd_manual manual = {
	.description =
	H_PARA("Convert an ELF executable to an ESP flash binary image.  "
	       "Produces the same output as @b{esptool elf2image} and "
	       "accepts the same flags so it can be used as a transparent "
	       "replacement in ESP-IDF build.ninja files.")
	H_PARA("All PT_LOAD segments with non-zero file size are included "
	       "(DROM, DRAM, IRAM, IROM, RTC).  Flash-mapped segments are "
	       "placed at 64 KB-aligned boundaries for correct MMU mapping."),

	.examples =
	H_EXAMPLE("ice elf2image --chip esp32 --flash-mode dio "
	          "--flash-freq 40m --flash-size 2MB "
	          "-o firmware.bin firmware.elf")
	H_EXAMPLE("ice elf2image --chip esp32s3 --elf-sha256-offset 0xb0 "
	          "-o app.bin app.elf"),

	.extras =
	H_SECTION("CHIPS")
	H_ITEM("esp8266, esp32, esp32s2, esp32s3",   "Xtensa targets")
	H_ITEM("esp32c2, esp32c3, esp32c5",           "RISC-V targets")
	H_ITEM("esp32c6, esp32h2, esp32p4",           "RISC-V targets")

	H_SECTION("FLASH MODE")
	H_ITEM("qio",  "Quad I/O (mode 0)")
	H_ITEM("qout", "Quad output (mode 1)")
	H_ITEM("dio",  "Dual I/O (mode 2, default)")
	H_ITEM("dout", "Dual output (mode 3)")

	H_SECTION("FLASH FREQUENCY (chip-family encoding)")
	H_ITEM("80m",  "80 MHz")
	H_ITEM("40m",  "40 MHz (default)")
	H_ITEM("26m",  "26 MHz")
	H_ITEM("20m",  "20 MHz")

	H_SECTION("FLASH SIZE")
	H_ITEM("detect / keep", "Auto-detect at boot (default)")
	H_ITEM("256KB, 512KB, 1MB, 2MB, 4MB, 8MB, 16MB", "Fixed sizes"),
};
/* clang-format on */

/* -------------------------------------------------------------------------
 * Chip-independent string → enum parsers (no chip knowledge required)
 * ---------------------------------------------------------------------- */

static int parse_chip(const char *name, target_chip_t *out)
{
	static const struct {
		const char *name;
		target_chip_t chip;
	} table[] = {
	    {"esp8266", ESP8266_CHIP},
	    {"esp32", ESP32_CHIP},
	    {"esp32s2", ESP32S2_CHIP},
	    {"esp32c3", ESP32C3_CHIP},
	    {"esp32s3", ESP32S3_CHIP},
	    {"esp32c2", ESP32C2_CHIP},
	    {"esp32c5", ESP32C5_CHIP},
	    {"esp32h2", ESP32H2_CHIP},
	    {"esp32c6", ESP32C6_CHIP},
	    {"esp32p4", ESP32P4_CHIP},
	    {NULL, 0},
	};
	for (int i = 0; table[i].name; i++)
		if (!strcmp(name, table[i].name)) {
			*out = table[i].chip;
			return 0;
		}
	return -1;
}

static int parse_flash_mode(const char *s, esp_flash_mode_t *out)
{
	if (!strcmp(s, "qio")) {
		*out = ESP_FLASH_MODE_QIO;
		return 0;
	}
	if (!strcmp(s, "qout")) {
		*out = ESP_FLASH_MODE_QOUT;
		return 0;
	}
	if (!strcmp(s, "dio")) {
		*out = ESP_FLASH_MODE_DIO;
		return 0;
	}
	if (!strcmp(s, "dout")) {
		*out = ESP_FLASH_MODE_DOUT;
		return 0;
	}
	if (s[0] >= '0' && s[0] <= '3' && s[1] == '\0') {
		*out = (esp_flash_mode_t)(s[0] - '0');
		return 0;
	}
	return -1;
}

static int parse_flash_freq(const char *s, esp_flash_freq_t *out)
{
	static const struct {
		const char *name;
		esp_flash_freq_t val;
	} tbl[] = {
	    {"80m", ESP_FLASH_FREQ_80M},
	    {"60m", ESP_FLASH_FREQ_60M},
	    {"48m", ESP_FLASH_FREQ_48M},
	    {"40m", ESP_FLASH_FREQ_40M},
	    {"30m", ESP_FLASH_FREQ_30M},
	    {"26m", ESP_FLASH_FREQ_26M},
	    {"24m", ESP_FLASH_FREQ_24M},
	    {"20m", ESP_FLASH_FREQ_20M},
	    {"16m", ESP_FLASH_FREQ_16M},
	    {"15m", ESP_FLASH_FREQ_15M},
	    {"12m", ESP_FLASH_FREQ_12M},
	    {"keep", ESP_FLASH_FREQ_KEEP},
	    {NULL, 0},
	};
	for (int i = 0; tbl[i].name; i++)
		if (!strcmp(s, tbl[i].name)) {
			*out = tbl[i].val;
			return 0;
		}
	return -1;
}

static int parse_flash_size(const char *s, esp_flash_size_t *out)
{
	static const struct {
		const char *name;
		esp_flash_size_t val;
	} tbl[] = {
	    {"256KB", ESP_FLASH_SIZE_256KB},
	    {"256kb", ESP_FLASH_SIZE_256KB},
	    {"512KB", ESP_FLASH_SIZE_512KB},
	    {"512kb", ESP_FLASH_SIZE_512KB},
	    {"1MB", ESP_FLASH_SIZE_1MB},
	    {"1mb", ESP_FLASH_SIZE_1MB},
	    {"2MB", ESP_FLASH_SIZE_2MB},
	    {"2mb", ESP_FLASH_SIZE_2MB},
	    {"4MB", ESP_FLASH_SIZE_4MB},
	    {"4mb", ESP_FLASH_SIZE_4MB},
	    {"8MB", ESP_FLASH_SIZE_8MB},
	    {"8mb", ESP_FLASH_SIZE_8MB},
	    {"16MB", ESP_FLASH_SIZE_16MB},
	    {"16mb", ESP_FLASH_SIZE_16MB},
	    {"32MB", ESP_FLASH_SIZE_32MB},
	    {"32mb", ESP_FLASH_SIZE_32MB},
	    {"64MB", ESP_FLASH_SIZE_64MB},
	    {"64mb", ESP_FLASH_SIZE_64MB},
	    {"128MB", ESP_FLASH_SIZE_128MB},
	    {"128mb", ESP_FLASH_SIZE_128MB},
	    {"detect", ESP_FLASH_SIZE_DETECT},
	    {"keep", ESP_FLASH_SIZE_DETECT},
	    {NULL, 0},
	};
	for (int i = 0; tbl[i].name; i++)
		if (!strcmp(s, tbl[i].name)) {
			*out = tbl[i].val;
			return 0;
		}
	return -1;
}

/* -------------------------------------------------------------------------
 * Options table (file-scope so completion.c can reference it via extern)
 * ---------------------------------------------------------------------- */

static const char *e2i_chip_str;
static const char *e2i_flash_mode_str;
static const char *e2i_flash_freq_str;
static const char *e2i_flash_size_str;
static const char *e2i_sha256_offset_str;
static const char *e2i_min_rev_str;
static const char *e2i_max_rev_str;
static const char *e2i_output_path;
static int e2i_secure_pad_v2;

/* clang-format off */
const struct option cmd_elf2image_opts[] = {
    OPT_STRING(0,   "chip",             &e2i_chip_str,          "chip",
	       "target chip (esp32, esp32s3, ...)"),
    OPT_STRING(0,   "flash-mode",       &e2i_flash_mode_str,    "mode",
	       "flash mode: qio|qout|dio|dout (default: dio)"),
    OPT_STRING(0,   "flash_mode",       &e2i_flash_mode_str,    "mode",
	       NULL),
    OPT_STRING(0,   "flash-freq",       &e2i_flash_freq_str,    "freq",
	       "flash frequency: 80m|40m|26m|20m (default: 40m)"),
    OPT_STRING(0,   "flash_freq",       &e2i_flash_freq_str,    "freq",
	       NULL),
    OPT_STRING(0,   "flash-size",       &e2i_flash_size_str,    "size",
	       "flash size: detect|2MB|4MB|... (default: detect)"),
    OPT_STRING(0,   "flash_size",       &e2i_flash_size_str,    "size",
	       NULL),
    OPT_STRING(0,   "elf-sha256-offset",&e2i_sha256_offset_str, "hex",
	       "embed ELF SHA256 in image; enables append_sha256"),
    OPT_STRING(0,   "min-rev-full",     &e2i_min_rev_str,       "n",
	       "minimum chip revision (0 = any)"),
    OPT_STRING(0,   "max-rev-full",     &e2i_max_rev_str,       "n",
	       "maximum chip revision (0xFFFF = any)"),
    OPT_STRING('o', "output",           &e2i_output_path,       "file",
	       "output binary path"),
    OPT_BOOL(0,     "secure-pad-v2",    &e2i_secure_pad_v2,
	     "pad image for secure boot v2 (accepted, ignored)"),
    OPT_END(),
};
/* clang-format on */

/* -------------------------------------------------------------------------
 * Command entry point
 * ---------------------------------------------------------------------- */

int cmd_elf2image(int argc, const char **argv)
{
	/* Reset to defaults (opts targets are file-scope). */
	e2i_chip_str = NULL;
	e2i_flash_mode_str = "dio";
	e2i_flash_freq_str = "40m";
	e2i_flash_size_str = "detect";
	e2i_sha256_offset_str = NULL;
	e2i_min_rev_str = NULL;
	e2i_max_rev_str = NULL;
	e2i_output_path = NULL;
	e2i_secure_pad_v2 = 0;

	argc = parse_options_manual(argc, argv, cmd_elf2image_opts, e2i_usage,
				    &manual);

	if (!e2i_chip_str)
		die("--chip is required");
	if (!e2i_output_path)
		die("-o / --output is required");
	if (argc < 1)
		die("usage: ice elf2image [options] <input.elf>");

	const char *input_path = argv[0];

	/* Parse chip. */
	target_chip_t chip;
	if (parse_chip(e2i_chip_str, &chip) != 0)
		die("unknown --chip '%s'", e2i_chip_str);

	/* Build elf cfg from flags. */
	esp_loader_elf_cfg_t cfg = ESP_LOADER_ELF_CFG_DEFAULT();

	if (parse_flash_mode(e2i_flash_mode_str, &cfg.flash_mode) != 0)
		die("unknown --flash-mode '%s'", e2i_flash_mode_str);

	if (parse_flash_freq(e2i_flash_freq_str, &cfg.flash_freq) != 0)
		die("unknown --flash-freq '%s'", e2i_flash_freq_str);

	if (parse_flash_size(e2i_flash_size_str, &cfg.flash_size) != 0)
		die("unknown --flash-size '%s'", e2i_flash_size_str);

	/* --elf-sha256-offset: non-zero → append image-level SHA256. */
	if (e2i_sha256_offset_str) {
		char *end;
		unsigned long v = strtoul(e2i_sha256_offset_str, &end, 0);
		if (*end)
			die("invalid --elf-sha256-offset '%s'",
			    e2i_sha256_offset_str);
		cfg.append_sha256 = (v != 0);
	}

	if (e2i_min_rev_str) {
		char *end;
		cfg.min_chip_rev_full =
		    (uint16_t)strtoul(e2i_min_rev_str, &end, 0);
		if (*end)
			die("invalid --min-rev-full '%s'", e2i_min_rev_str);
	}

	if (e2i_max_rev_str) {
		char *end;
		cfg.max_chip_rev_full =
		    (uint16_t)strtoul(e2i_max_rev_str, &end, 0);
		if (*end)
			die("invalid --max-rev-full '%s'", e2i_max_rev_str);
	}

	/* Read input ELF. */
	struct sbuf elf_buf = SBUF_INIT;
	if (sbuf_read_file(&elf_buf, input_path) < 0)
		die_errno("cannot read '%s'", input_path);

	/* Size query. */
	size_t img_size = 0;
	esp_loader_error_t rc = esp_loader_elf_to_flash_image(
	    (const uint8_t *)elf_buf.buf, elf_buf.len, chip, &cfg, NULL,
	    &img_size);
	if (rc != ESP_LOADER_SUCCESS) {
		sbuf_release(&elf_buf);
		err("elf2image: conversion failed (error %d) for '%s'", (int)rc,
		    input_path);
		return 1;
	}

	uint8_t *out_buf = (uint8_t *)malloc(img_size);
	if (!out_buf)
		die_errno("malloc");

	rc = esp_loader_elf_to_flash_image((const uint8_t *)elf_buf.buf,
					   elf_buf.len, chip, &cfg, out_buf,
					   &img_size);
	sbuf_release(&elf_buf);
	if (rc != ESP_LOADER_SUCCESS) {
		free(out_buf);
		err("elf2image: image write failed (error %d) for '%s'",
		    (int)rc, input_path);
		return 1;
	}

	mkdirp_for_file(e2i_output_path);

	FILE *fp = fopen(e2i_output_path, "wb");
	if (!fp) {
		free(out_buf);
		err_errno("cannot write '%s'", e2i_output_path);
		return 1;
	}
	if (fwrite(out_buf, 1, img_size, fp) != img_size) {
		free(out_buf);
		fclose(fp);
		err_errno("write error on '%s'", e2i_output_path);
		return 1;
	}
	fclose(fp);
	free(out_buf);

	fprintf(stderr, "Written %zu bytes to %s\n", img_size, e2i_output_path);
	return 0;
}
