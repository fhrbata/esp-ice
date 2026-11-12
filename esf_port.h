/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esf_port.h
 * @brief esp-serial-flasher port backed by esp-ice's serial.h API.
 */
#ifndef ESF_PORT_H
#define ESF_PORT_H

#include "chip.h"
#include "esp_loader.h"
#include "esp_loader_io.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — struct serial is opaque, defined per-platform. */
struct serial;

/**
 * @brief Concrete ice serial port instance.
 *
 * Fill @c device and @c baudrate, set @c port.ops = &esf_port_ops,
 * then pass @c &p.port to esp_loader_init_serial().
 *
 * @code
 *   esf_port_t p = {
 *       .port.ops = &esf_port_ops,
 *       .device   = "/dev/ttyUSB0",
 *       .baudrate = 115200,
 *   };
 *   esp_loader_t loader;
 *   esp_loader_init_serial(&loader, &p.port);
 * @endcode
 */
typedef struct {
	esp_loader_port_t
	    port; /*!< Embedded base; pass &port to esp_loader_init_* */

	/* Public configuration — fill before calling esp_loader_init_serial()
	 */
	const char *device; /*!< Serial device path, e.g. "/dev/ttyUSB0" */
	unsigned baudrate;  /*!< Initial baud rate, e.g. 115200 */

	/* Private runtime state — do not access directly */
	struct serial *_serial; /*!< Opened by init, closed by deinit */
	int64_t _time_end;	/*!< Deadline set by start_timer */
	int _is_usb_jtag; /*!< 1 when VID=0x303A PID=0x1001 (USB JTAG Serial) */
} esf_port_t;

/** Operations vtable for esf_port_t. */
extern const esp_loader_port_ops_t esf_port_ops;

/**
 * @brief Translate a target_chip_t (ESP-serial-flasher) to enum ice_chip.
 * Returns ICE_CHIP_UNKNOWN for ESP_UNKNOWN_CHIP or unrecognised values.
 */
enum ice_chip ice_chip_from_esf(target_chip_t chip);

/**
 * @brief Translate an enum ice_chip to target_chip_t (ESP-serial-flasher).
 * Returns ESP_UNKNOWN_CHIP for ICE_CHIP_UNKNOWN or unrecognised values.
 */
target_chip_t ice_chip_to_esf(enum ice_chip chip);

/**
 * @brief Probe available serial ports and return the path of the first
 * one that responds as an ESP device matching @p required.
 *
 * Pass @c ICE_CHIP_UNKNOWN to accept any ESP chip.  The device is reset
 * back to normal run mode before this function returns.
 *
 * Status lines are written to stderr.  Returns a heap-allocated string
 * the caller must free(), or NULL if nothing was found.
 */
char *esf_find_esp_port(enum ice_chip required);

/**
 * @brief Connect to the chip's ROM bootloader and start the flasher stub.
 *
 * The stub is the small program esptool also uploads: it flashes and
 * reads faster than the ROM, verifies writes with MD5, and supports
 * compressed writes on every chip.  With @p no_stub the loader stays
 * on the ROM bootloader, which is the fallback when a stub fails to
 * start (e.g. a chip revision the bundled stub does not handle yet).
 *
 * @p loader must have been initialised with esp_loader_init_serial().
 */
esp_loader_error_t esf_connect(esp_loader_t *loader, int no_stub);

/**
 * @brief Open a device and leave the loader connected and ready.
 *
 * Opens @p port -- or, when it is NULL, the first port that answers as
 * an ESP chip (see esf_find_esp_port()) -- connects through
 * esf_connect() and negotiates @p baud.  A failed baud change is only
 * a warning; the session stays at 115200.
 *
 * On success the caller owns @p loader, @p sport (which must outlive
 * the loader) and the auto-detected port name written to
 * @p autoport_out (free()-able, NULL when @p port was given).
 *
 * @return 0 on success, -1 on failure with the reason printed.
 */
int esf_open(esp_loader_t *loader, esf_port_t *sport, const char *port,
	     int baud, int no_stub, char **autoport_out);

#ifdef __cplusplus
}
#endif

#endif /* ESF_PORT_H */
