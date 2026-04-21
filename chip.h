/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file chip.h
 * @brief ice chip identity — canonical enum and metadata table.
 *
 * Single source of truth for: chip enumeration, IDF target names,
 * human-readable names, one-line summaries, and which chips are
 * supported vs. preview.
 *
 * The ESP-serial-flasher boundary translation
 * (target_chip_t ↔ enum ice_chip) lives in esf_port.c/h only, so
 * nothing here depends on the esp-serial-flasher headers.
 */
#ifndef CHIP_H
#define CHIP_H

/**
 * ice's canonical chip identifier.
 *
 * ICE_CHIP_UNKNOWN is returned / accepted for anything unrecognised;
 * all valid chip values are strictly greater than zero.
 */
enum ice_chip {
	ICE_CHIP_UNKNOWN = 0,
	ICE_CHIP_ESP8266,
	ICE_CHIP_ESP32,
	ICE_CHIP_ESP32S2,
	ICE_CHIP_ESP32S3,
	ICE_CHIP_ESP32C2,
	ICE_CHIP_ESP32C3,
	ICE_CHIP_ESP32C5,
	ICE_CHIP_ESP32C6,
	ICE_CHIP_ESP32C61,
	ICE_CHIP_ESP32H2,
	ICE_CHIP_ESP32H21,
	ICE_CHIP_ESP32H4,
	ICE_CHIP_ESP32P4,
	ICE_CHIP_LINUX,
	ICE_CHIP_ESP32S31,
};

/**
 * @brief Human-readable chip name, e.g. "ESP32-C6".
 * Returns "unknown" for ICE_CHIP_UNKNOWN or unrecognised values.
 */
const char *ice_chip_name(enum ice_chip chip);

/**
 * @brief IDF target name for a chip, e.g. "esp32c6".
 * Returns NULL for ICE_CHIP_UNKNOWN or unrecognised values.
 */
const char *ice_chip_idf_name(enum ice_chip chip);

/**
 * @brief Translate an IDF target name to enum ice_chip.
 * Returns ICE_CHIP_UNKNOWN for NULL, empty, or unrecognised strings.
 */
enum ice_chip ice_chip_from_idf_name(const char *name);

/**
 * @brief One-line summary for a chip (e.g. "RISC-V, WiFi 6 + BLE + 802.15.4").
 *
 * Accepts an IDF name string to match the calling pattern used by
 * callers that iterate ice_supported_targets[].
 * Returns NULL for unrecognised chips.
 */
const char *ice_chip_summary(const char *idf_name);

/**
 * NULL-terminated arrays of IDF target names.
 *
 * ice_supported_targets — chips enabled by default in `ice init`.
 * ice_preview_targets   — chips that require --preview.
 *
 * Both mirror esp-idf/tools/idf_py_actions/constants.py.
 */
extern const char *const ice_supported_targets[];
extern const char *const ice_preview_targets[];

#endif /* CHIP_H */
