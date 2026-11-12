/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/efuse/efuse.h
 * @brief Shared helpers for `ice target efuse <verb>`.
 *
 * Every verb starts the same way: connect, then read the whole eFuse
 * image once together with the chip's named field table.  Field
 * values and protection bits are then decoded from that image on the
 * host, so listing ninety fields costs one read of the chip, not
 * ninety.
 */
#ifndef CMD_TARGET_EFUSE_H
#define CMD_TARGET_EFUSE_H

#include <stddef.h>
#include <stdint.h>

#include "esf_port.h"
#include "esp_loader.h"
#include "esp_loader_efuse.h"

/** Options shared by every verb; see @ref EFUSE_COMMON_OPTS. */
extern const char *efuse_opt_port;
extern int efuse_opt_baud;
extern int efuse_opt_no_stub;

/* clang-format off */
/** Option-table entries every verb lists first. */
#define EFUSE_COMMON_OPTS                                                      \
	OPT_STRING('p', "port", &efuse_opt_port, "dev",                        \
		   "serial port (omit to auto-detect)", serial_complete_port), \
	OPT_INT('b', "baud", &efuse_opt_baud, "rate",                          \
		"baud rate after the handshake (default 115200)", NULL),       \
	OPT_BOOL_CFG(0, "no-stub", &efuse_opt_no_stub, "serial.no-stub", NULL, \
		     "talk to the ROM bootloader; do not upload the flasher "  \
		     "stub", NULL)
/* clang-format on */

/** A connected chip and everything read from its eFuses. */
struct efuse_session {
	int connected; /**< loader connected; close() has work to do */
	esp_loader_t loader;
	esf_port_t sport;
	char *autoport; /**< auto-detected port name, or NULL */
	target_chip_t chip;
	uint16_t revision; /**< major * 100 + minor */
	const esp_loader_efuse_field_set_t *fields;
	esp_loader_efuse_image_t image;
};

/**
 * @brief Connect and read the chip's eFuse image and field table.
 *
 * Uses the @ref EFUSE_COMMON_OPTS values.  Fails when the chip has no
 * eFuse field table in esp-serial-flasher (ESP8266 and the newest
 * chips).
 *
 * @return 0 on success, -1 on failure with the reason printed.
 */
int efuse_session_open(struct efuse_session *s);

/** Reset the chip back into its application and release the port.
 *  Safe to call after a failed efuse_session_open(). */
void efuse_session_close(struct efuse_session *s);

/** Total width of @p f in bits. */
size_t efuse_field_bits(const esp_loader_efuse_field_info_t *f);

/**
 * @brief Decode a field's value from the session's image.
 *
 * Bits are packed LSB-first into @p dst, the order
 * esp_loader_efuse_read_field() uses.  @p dst is zeroed first and must
 * hold at least (efuse_field_bits(f) + 7) / 8 bytes.
 */
void efuse_field_value(const struct efuse_session *s,
		       const esp_loader_efuse_field_info_t *f, uint8_t *dst);

/** Non-zero when the WR_DIS bit guarding @p f is burned. */
int efuse_field_write_protected(const struct efuse_session *s,
				const esp_loader_efuse_field_info_t *f);

/** Non-zero when the RD_DIS bit guarding @p f is burned. */
int efuse_field_read_protected(const struct efuse_session *s,
			       const esp_loader_efuse_field_info_t *f);

#endif /* CMD_TARGET_EFUSE_H */
