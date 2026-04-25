/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf2image.h
 * @brief ELF → ESP flash-image conversion engine.
 *
 * Produces the same binary output as @c esptool @c elf2image,
 * cross-validated against esptool v5.x.  The public surface is a
 * single entry point, @c e2i_build, that takes an ELF executable in
 * memory and writes an ESP flash image to a caller-provided sbuf.
 *
 * The engine is independent of the CLI: @c cmd/image/elf2image.c
 * handles argument parsing, file I/O, and user-facing validation,
 * and calls into this module for the byte-level work.  Chip identity
 * and the per-chip tables live in @ref binary.h so the image reader
 * (@c ice @c image @c info) and writer share one source of truth.
 *
 * Scope: ESP32 family (ESP32, S2, S3, C2, C3, C5, C6, H2, P4).
 * ESP8266 uses a different image format (V1/V2, no extended header)
 * and is not supported by this engine today -- in practice an ESP8266
 * image would be produced from the esp8266 RTOS SDK, not from
 * ESP-IDF's ice build pipeline.
 *
 * Image format (ESP32+):
 *   [common hdr: 8 B]  magic(1) segments(1) flash_mode(1) size|freq(1) entry(4)
 *   [ext hdr:   16 B]  wp(1) spi_drv(3) chip_id(2) min_rev(1)
 *                      min_rev_full(2) max_rev_full(2) pad(4) digest(1)
 *   [segment hdr: 8 B] load_addr(4) data_size(4)
 *   [segment data]     filesz bytes, padded to 4-byte boundary
 *   ... repeated per segment ...
 *   [checksum pad]     zeros up to a 16-byte boundary, last byte = XOR
 *   [SHA-256]          32 B, if cfg->append_sha256
 *
 * Flash-mapped segments (DROM/IROM) are placed at IROM_ALIGN (64 KB)
 * boundaries so the MMU maps them correctly when the image is
 * flashed at any 64 KB-aligned partition offset.  RAM segments fill
 * the gaps between flash segments and may be split across them.
 *
 * Reference implementation: esptool's
 * @c esptool/bin_image.py::ESP32FirmwareImage.save() and the per-chip
 * constants in @c esptool/targets/esp*.py.
 */
#ifndef ELF2IMAGE_H
#define ELF2IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "binary.h"

struct sbuf;

/**
 * Image configuration.  All fields are optional in the sense that
 * @ref E2I_CONFIG_DEFAULT provides sane defaults; callers override
 * individual fields before calling @ref e2i_build.
 *
 * String fields match esptool's CLI vocabulary exactly:
 *   flash_mode: "qio" | "qout" | "dio" | "dout"
 *   flash_size: "1MB" | "2MB" | "4MB" | "8MB" | "16MB" | "32MB"
 *               | "64MB" | "128MB"
 *   flash_freq: chip-dependent; common values include
 *               "80m" | "40m" | "26m" | "20m"
 *               (ESP32-C2:  "60m" | "30m" | "20m" | "15m")
 *               (ESP32-H2:  "48m" | "24m" | "16m" | "12m")
 *
 * e2i_build() dies with a descriptive error if a value is not valid
 * for the target chip.
 */
struct e2i_config {
	const char *flash_mode;
	const char *flash_freq;
	const char *flash_size;
	uint16_t min_rev_full;	    /**< 0 = any (default) */
	uint16_t max_rev_full;	    /**< 0xFFFF = any (default) */
	bool append_sha256;	    /**< write 32-byte digest after checksum */
	bool use_segments;	    /**< walk PT_LOAD program headers instead of
				     *   sections; default is sections, matching
				     *   esptool's default behaviour. */
	uint32_t elf_sha256_offset; /**< patch app_elf_sha256 field; 0 = skip */
};

/**
 * Initialiser producing esptool-compatible defaults: DIO flash mode,
 * undefined flash_freq/flash_size (caller must set), no
 * chip-revision constraints, digest appended (required by ROM
 * bootloader on ESP32+), no ELF hash patching.
 *
 * Usage:
 *   struct e2i_config cfg = E2I_CONFIG_DEFAULT();
 *   cfg.flash_mode = "qio";
 *   cfg.flash_freq = "40m";
 *   cfg.flash_size = "2MB";
 *   ...
 */
#define E2I_CONFIG_DEFAULT()                                                   \
	{                                                                      \
	    .flash_mode = "dio",                                               \
	    .flash_freq = NULL,                                                \
	    .flash_size = NULL,                                                \
	    .min_rev_full = 0,                                                 \
	    .max_rev_full = 0xFFFF,                                            \
	    .append_sha256 = true,                                             \
	    .use_segments = false,                                             \
	    .elf_sha256_offset = 0,                                            \
	}

/**
 * @brief Convert an ELF executable to an ESP flash image.
 *
 * Walks the input ELF's program header table, classifies each
 * PT_LOAD segment as flash-mapped (DROM/IROM) or RAM (DRAM/IRAM/RTC)
 * based on its virtual address, lays them out in file order with
 * IROM_ALIGN padding where necessary, and writes the image header,
 * segments, checksum, and optional SHA-256 digest to @p out.
 *
 * Dies on: malformed ELF (bad magic, not ET_EXEC, etc.), an ELF
 * segment whose virtual address is not mapped by @p chip, invalid
 * flash_mode / flash_freq / flash_size for @p chip, or too many
 * segments (> 16, the ROM bootloader's limit).
 *
 * On success @p out holds the complete flash image; its previous
 * contents are replaced.
 *
 * @param elf      Pointer to the ELF image bytes.
 * @param elf_len  Number of bytes in @p elf.
 * @param chip     Target chip.
 * @param cfg      Image configuration.
 * @param out      sbuf to receive the generated image (grown as needed).
 */
void e2i_build(const void *elf, size_t elf_len, enum bin_chip chip,
	       const struct e2i_config *cfg, struct sbuf *out);

#endif /* ELF2IMAGE_H */
