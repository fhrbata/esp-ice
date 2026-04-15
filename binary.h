/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file binary.h
 * @brief Shared knowledge about the ESP flash image format.
 *
 * Everything in here is independent of whether a caller is *writing*
 * an image (elf2image) or *reading* one (image info): chip identity,
 * per-chip memory maps, classification of virtual addresses, flash
 * parameter encoding in both directions, and the handful of format
 * constants (magic bytes, header lengths, IROM alignment, app-desc
 * layout) that never change.
 *
 * Scope is the ESP32 family (ESP32, S2, S3, C2, C3, C5, C6, H2, P4).
 * ESP8266 uses a different image format without an extended header
 * and is not covered.
 *
 * References:
 *   - esptool/bin_image.py            (image layout, checksum, digest)
 *   - esptool/targets/esp*.py         (per-chip address ranges,
 *                                      flash-param byte encodings)
 */
#ifndef BINARY_H
#define BINARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Image format constants -------------------------------------- */

#define BIN_IMAGE_MAGIC 0xE9u	 /**< first byte of every image */
#define BIN_CHECKSUM_MAGIC 0xEFu /**< XOR seed for the trailer byte */
#define BIN_IROM_ALIGN 0x10000u	 /**< 64 KB MMU page (default) */
#define BIN_SEG_HDR_LEN 8u	 /**< load_addr(4) + data_size(4) */
#define BIN_HDR_LEN 8u		 /**< common header size */
#define BIN_EXT_HDR_LEN 16u	 /**< extended header size (ESP32+) */
#define BIN_DIGEST_LEN 32u	 /**< SHA-256 output */
#define BIN_MAX_SEGS 16u	 /**< ROM bootloader cap */

/* Offset of the append_digest flag within the extended header. */
#define BIN_EXT_APPEND_DIGEST_OFF 15u

/* esp_app_desc_t: a 256-byte descriptor the linker places at the
 * start of the DROM segment.  Magic identifies it; elf_sha256 at
 * offset 144 is the 32-byte field patched when --elf-sha256-offset
 * is in effect. */
#define BIN_APP_DESC_MAGIC 0xABCD5432u
#define BIN_APP_DESC_ELF_SHA256_OFF 144u

/* --- Chip identity ----------------------------------------------- */

/**
 * Supported target chips.  Order is arbitrary; callers should not
 * depend on the numeric value of these enumerators beyond
 * using them as indices into internal tables through the helpers
 * below.
 */
enum bin_chip {
	BIN_CHIP_ESP32,
	BIN_CHIP_ESP32S2,
	BIN_CHIP_ESP32S3,
	BIN_CHIP_ESP32C2,
	BIN_CHIP_ESP32C3,
	BIN_CHIP_ESP32C5,
	BIN_CHIP_ESP32C6,
	BIN_CHIP_ESP32H2,
	BIN_CHIP_ESP32P4,
	BIN_CHIP_MAX
};

/**
 * @brief Look up a chip by its canonical short name.
 *
 * Accepts lower-case IDF-style identifiers ("esp32", "esp32s2",
 * "esp32c3", ...).  Returns @c BIN_CHIP_MAX on unknown input so the
 * caller can format its own error message.
 */
enum bin_chip bin_chip_by_name(const char *name);

/**
 * @brief Canonical short name for @p chip.
 * Returns "?" when @p chip is out of range.
 */
const char *bin_chip_name(enum bin_chip chip);

/**
 * @brief NULL-terminated list of every supported chip's short name.
 *
 * The array and the strings it points at have static storage
 * duration; callers must not free them.  Useful for help text and
 * shell completion.
 */
const char *const *bin_chip_names(void);

/**
 * @brief IMAGE_CHIP_ID written into the extended header.
 *
 * Looks up the 16-bit chip ID assigned by Espressif for the extended
 * header's chip_id field (e.g. 0 for ESP32, 5 for ESP32-C3).
 * Returns 0xFFFF when @p chip is out of range.
 */
uint16_t bin_chip_id(enum bin_chip chip);

/**
 * @brief Reverse of @ref bin_chip_id -- look up a chip by its
 * IMAGE_CHIP_ID byte value.
 *
 * Used by readers that decode the extended header to auto-detect
 * the target chip from the image file.  Returns @c BIN_CHIP_MAX if
 * @p chip_id is not recognised.
 */
enum bin_chip bin_chip_by_id(uint16_t chip_id);

/* --- Segment classification -------------------------------------- */

/**
 * Memory-region types used during image construction and inspection.
 * Flash-mapped types (DROM, IROM) go through the MMU and must be
 * IROM_ALIGN-aligned in the file; the rest are RAM-resident and are
 * loaded directly by the bootloader.
 */
enum bin_seg_type {
	BIN_SEG_DROM,	  /**< flash-mapped read-only data */
	BIN_SEG_IROM,	  /**< flash-mapped code */
	BIN_SEG_DRAM,	  /**< internal RAM (data) */
	BIN_SEG_IRAM,	  /**< internal RAM (code) */
	BIN_SEG_RTC_DATA, /**< RTC slow / fast memory */
	BIN_SEG_UNKNOWN	  /**< not mapped / not classifiable */
};

/**
 * @brief Classify a virtual address against @p chip's memory map.
 * @return The segment type, or @c BIN_SEG_UNKNOWN if @p vaddr falls
 *         outside every mapped range.
 */
enum bin_seg_type bin_classify(enum bin_chip chip, uint32_t vaddr);

/**
 * @brief Short human-readable label for a segment type, used in
 * listings produced by @c ice @c image @c info.
 */
const char *bin_seg_type_name(enum bin_seg_type t);

/* --- Flash-parameter encoding ------------------------------------ */

/*
 * flash_mode (image byte 2) is chip-independent; flash_size and
 * flash_freq (the high and low nibbles of image byte 3) are mostly
 * chip-independent for flash_size but chip-specific for flash_freq.
 *
 * Encoders (string -> byte) die() on an invalid value so the caller
 * gets a descriptive error at argument-parse time.  Decoders
 * (byte -> string) never die -- they return "?" for unknown byte
 * values so the reader can still produce useful output from a
 * corrupted or unfamiliar image.
 */

/**
 * @brief Encode a flash_mode name ("qio"|"qout"|"dio"|"dout") into
 * the byte written at image offset 2.
 */
uint8_t bin_flash_mode_byte(const char *mode);

/**
 * @brief Encode a flash_size name ("1MB"|...|"128MB") into the high
 * nibble of the byte at image offset 3.
 */
uint8_t bin_flash_size_byte(const char *size);

/**
 * @brief Encode a flash_freq name into the low nibble of the byte at
 * image offset 3.  The accepted set depends on @p chip -- see
 * esptool/targets/esp*.py FLASH_FREQUENCY for the list.
 */
uint8_t bin_flash_freq_byte(enum bin_chip chip, const char *freq);

/** Reverse of @ref bin_flash_mode_byte; returns "?" on unknown. */
const char *bin_flash_mode_str(uint8_t b);
/** Reverse of @ref bin_flash_size_byte (high nibble only). */
const char *bin_flash_size_str(uint8_t high_nibble);
/** Reverse of @ref bin_flash_freq_byte (low nibble only). */
const char *bin_flash_freq_str(enum bin_chip chip, uint8_t low_nibble);

#endif /* BINARY_H */
