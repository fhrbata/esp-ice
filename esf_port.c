/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esf_port.c
 * @brief esp-serial-flasher port backed by esp-ice's serial.h API.
 *
 * All timing uses mono_ms() / delay_ms() from platform.h.
 * USB JTAG Serial detection delegates to serial_is_usb_jtag() in
 * the platform serial layer, so this file contains no #ifdef guards.
 */
#include "esf_port.h"
#include "chip.h"
#include "esp_loader.h"	   /* RETURN_ON_ERROR, ESP_LOADER_* */
#include "esp_loader_io.h" /* container_of, esp_loader_port_t */
#include "json.h"
#include "platform.h" /* mono_ms(), delay_ms() */
#include "sbuf.h"
#include "serial.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  USB JTAG Serial port re-enumeration                               */

/*
 * After a USB JTAG Serial reset the device re-enumerates and the port
 * disappears briefly.  Close the current handle and poll serial_open()
 * until the port reappears or the timeout expires.
 */
static esp_loader_error_t esf_wait_port_reopen(esf_port_t *p,
					       uint32_t timeout_ms)
{
	serial_close(p->_serial);
	p->_serial = NULL;

	int64_t deadline = (int64_t)mono_ms() + (int64_t)timeout_ms;

	while ((int64_t)mono_ms() < deadline) {
		delay_ms(100);
		if (serial_open(&p->_serial, p->device) == 0 &&
		    serial_set_baud(p->_serial, p->baudrate) == 0)
			return ESP_LOADER_SUCCESS;
		if (p->_serial) {
			serial_close(p->_serial);
			p->_serial = NULL;
		}
	}
	fprintf(stderr, "esf_port: timed out waiting for %s to reappear\n",
		p->device);
	return ESP_LOADER_ERROR_TIMEOUT;
}

/* Timing constants — normally set by the flasher library's cmake config.
 * Define defaults here so the port compiles stand-alone. */
#ifndef SERIAL_FLASHER_RESET_HOLD_TIME_MS
#define SERIAL_FLASHER_RESET_HOLD_TIME_MS 100
#endif
#ifndef SERIAL_FLASHER_BOOT_HOLD_TIME_MS
#define SERIAL_FLASHER_BOOT_HOLD_TIME_MS 50
#endif

/* ------------------------------------------------------------------ */
/*  init / deinit                                                      */

static esp_loader_error_t ice_port_init(esp_loader_port_t *port)
{
	esf_port_t *p = container_of(port, esf_port_t, port);

	p->_time_end = 0;
	p->_is_usb_jtag = 0;

	if (serial_open(&p->_serial, p->device) != 0) {
		fprintf(stderr, "ice flash: cannot open %s\n", p->device);
		return ESP_LOADER_ERROR_FAIL;
	}

	if (serial_set_baud(p->_serial, p->baudrate) != 0) {
		fprintf(stderr, "ice flash: cannot set baud %u on %s\n",
			p->baudrate, p->device);
		serial_close(p->_serial);
		p->_serial = NULL;
		return ESP_LOADER_ERROR_FAIL;
	}

	unsigned int vid = 0, pid = 0;
	p->_is_usb_jtag = (serial_get_usb_id(p->device, &vid, &pid) == 0 &&
			   vid == 0x303A && pid == 0x1001);
	return ESP_LOADER_SUCCESS;
}

static void ice_port_deinit(esp_loader_port_t *port)
{
	esf_port_t *p = container_of(port, esf_port_t, port);
	serial_close(p->_serial);
	p->_serial = NULL;
}

/* ------------------------------------------------------------------ */
/*  I/O                                                                */

static esp_loader_error_t ice_port_write(esp_loader_port_t *port,
					 const uint8_t *data, uint16_t size,
					 uint32_t timeout)
{
	esf_port_t *p = container_of(port, esf_port_t, port);
	(void)timeout; /* serial_write is already blocking */

	if (serial_write(p->_serial, data, size) < 0)
		return ESP_LOADER_ERROR_FAIL;

	return ESP_LOADER_SUCCESS;
}

static esp_loader_error_t ice_port_read(esp_loader_port_t *port, uint8_t *data,
					uint16_t size, uint32_t timeout)
{
	esf_port_t *p = container_of(port, esf_port_t, port);
	(void)timeout; /* deadline is tracked via _time_end / remaining_time */

	for (uint16_t i = 0; i < size; i++) {
		int64_t remaining = p->_time_end - (int64_t)mono_ms();
		unsigned ms = (remaining > 0) ? (unsigned)remaining : 0;

		ssize_t n = serial_read(p->_serial, &data[i], 1, ms);
		if (n == 0)
			return ESP_LOADER_ERROR_TIMEOUT;
		if (n < 0)
			return ESP_LOADER_ERROR_FAIL;
	}

	return ESP_LOADER_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Timer                                                              */

static void ice_start_timer(esp_loader_port_t *port, uint32_t ms)
{
	esf_port_t *p = container_of(port, esf_port_t, port);
	p->_time_end = (int64_t)mono_ms() + (int64_t)ms;
}

static uint32_t ice_remaining_time(esp_loader_port_t *port)
{
	esf_port_t *p = container_of(port, esf_port_t, port);
	int64_t remaining = p->_time_end - (int64_t)mono_ms();
	return (remaining > 0) ? (uint32_t)remaining : 0;
}

static void ice_delay_ms(esp_loader_port_t *port, uint32_t ms)
{
	(void)port;
	delay_ms(ms);
}

/* ------------------------------------------------------------------ */
/*  Baud rate                                                          */

static esp_loader_error_t ice_change_rate(esp_loader_port_t *port,
					  uint32_t rate)
{
	esf_port_t *p = container_of(port, esf_port_t, port);
	return (serial_set_baud(p->_serial, (unsigned)rate) == 0)
		   ? ESP_LOADER_SUCCESS
		   : ESP_LOADER_ERROR_FAIL;
}

/* ------------------------------------------------------------------ */
/*  Reset sequences                                                    */

/*
 * Standard esptool auto-reset circuit (CP2102/CH340/FT232):
 *   DTR asserted (1) drives BOOT  LOW  via inverting transistor
 *   RTS asserted (1) drives RESET LOW  via inverting transistor
 */

static void ice_reset_target(esp_loader_port_t *port)
{
	esf_port_t *p = container_of(port, esf_port_t, port);

	/*
	 * Deassert DTR (BOOT high) BEFORE pulsing RESET so the chip boots
	 * into the application, not the bootloader.  For USB JTAG Serial
	 * devices the port re-enumerates after reset; wait for it to reappear.
	 */
	serial_set_dtr(p->_serial, 0);
	serial_set_rts(p->_serial, 1);
	delay_ms(SERIAL_FLASHER_RESET_HOLD_TIME_MS);
	serial_set_rts(p->_serial, 0);

	if (p->_is_usb_jtag)
		esf_wait_port_reopen(p, 3000);
}

static void ice_enter_bootloader(esp_loader_port_t *port)
{
	esf_port_t *p = container_of(port, esf_port_t, port);

	if (p->_is_usb_jtag) {
		/*
		 * USBJTAGSerialReset — required for chips connected via their
		 * built-in USB JTAG Serial peripheral (ESP32-S3, C3, C6, H2,
		 * P4).
		 *
		 * BOOT must be asserted before RESET goes low.  Transition
		 * from (DTR=1,RTS=1) → (DTR=0,RTS=1) passes through (1,1)
		 * to avoid the (0,0) glitch.  After RESET is released the
		 * device re-enumerates; wait for the port to reappear.
		 *
		 * Step 1: DTR=0 RTS=0 — idle
		 * Step 2: DTR=1 RTS=0 — assert BOOT (GPIO0 low)
		 * Step 3: DTR=1 RTS=1 — assert RESET (chip in reset with BOOT
		 * low) Step 4: DTR=0 RTS=1 — release BOOT via (1,1) state Step
		 * 5: DTR=0 RTS=0 — release RESET → boots into bootloader
		 */
		serial_set_dtr(p->_serial, 0);
		serial_set_rts(p->_serial, 0); /* idle */
		delay_ms(SERIAL_FLASHER_RESET_HOLD_TIME_MS);
		serial_set_dtr(p->_serial, 1);
		serial_set_rts(p->_serial, 0); /* assert BOOT */
		delay_ms(SERIAL_FLASHER_RESET_HOLD_TIME_MS);
		serial_set_dtr(p->_serial, 1);
		serial_set_rts(p->_serial, 1); /* assert RESET */
		serial_set_dtr(p->_serial, 0);
		serial_set_rts(p->_serial, 1); /* release BOOT (through 1,1) */
		delay_ms(SERIAL_FLASHER_RESET_HOLD_TIME_MS);
		serial_set_dtr(p->_serial, 0);
		serial_set_rts(p->_serial, 0); /* release RESET → bootloader */
		esf_wait_port_reopen(p, 3000);
	} else {
		/*
		 * UnixTightReset — matches esptool's sequence for CP2102/CH340/
		 * FT232 boards with the standard inverting auto-reset circuit.
		 *
		 * Step 1: DTR=0 RTS=0 — idle
		 * Step 2: DTR=1 RTS=1 — through (1,1) to avoid (0,0)→(0,1)
		 * glitch Step 3: DTR=0 RTS=1 — RESET low, BOOT high → chip held
		 * in reset Step 4: DTR=1 RTS=0 — RESET released while BOOT is
		 * low → bootloader Step 5: DTR=1 RTS=1 — release BOOT, chip
		 * running in bootloader
		 */
		serial_set_dtr(p->_serial, 1);
		serial_set_rts(p->_serial, 1); /* idle / through (1,1) */

		serial_set_dtr(p->_serial, 0);
		serial_set_rts(p->_serial, 1); /* hold RESET, assert BOOT */
		delay_ms(SERIAL_FLASHER_RESET_HOLD_TIME_MS);

		serial_set_dtr(p->_serial, 1);
		serial_set_rts(p->_serial,
			       0); /* release RESET, BOOT still asserted */
		delay_ms(SERIAL_FLASHER_BOOT_HOLD_TIME_MS);

		serial_set_dtr(p->_serial, 1);
		serial_set_rts(p->_serial, 1); /* release BOOT */

		serial_flush_input(p->_serial); /* discard ROM boot noise */
	}
}

/* ------------------------------------------------------------------ */
/*  ESF ↔ ice_chip boundary translation                               */

enum ice_chip ice_chip_from_esf(target_chip_t chip)
{
	switch (chip) {
	case ESP8266_CHIP:
		return ICE_CHIP_ESP8266;
	case ESP32_CHIP:
		return ICE_CHIP_ESP32;
	case ESP32S2_CHIP:
		return ICE_CHIP_ESP32S2;
	case ESP32C3_CHIP:
		return ICE_CHIP_ESP32C3;
	case ESP32S3_CHIP:
		return ICE_CHIP_ESP32S3;
	case ESP32C2_CHIP:
		return ICE_CHIP_ESP32C2;
	case ESP32C5_CHIP:
		return ICE_CHIP_ESP32C5;
	case ESP32H2_CHIP:
		return ICE_CHIP_ESP32H2;
	case ESP32C6_CHIP:
		return ICE_CHIP_ESP32C6;
	case ESP32P4_CHIP:
		return ICE_CHIP_ESP32P4;
	default:
		return ICE_CHIP_UNKNOWN;
	}
}

target_chip_t ice_chip_to_esf(enum ice_chip chip)
{
	switch (chip) {
	case ICE_CHIP_ESP8266:
		return ESP8266_CHIP;
	case ICE_CHIP_ESP32:
		return ESP32_CHIP;
	case ICE_CHIP_ESP32S2:
		return ESP32S2_CHIP;
	case ICE_CHIP_ESP32C3:
		return ESP32C3_CHIP;
	case ICE_CHIP_ESP32S3:
		return ESP32S3_CHIP;
	case ICE_CHIP_ESP32C2:
		return ESP32C2_CHIP;
	case ICE_CHIP_ESP32C5:
		return ESP32C5_CHIP;
	case ICE_CHIP_ESP32H2:
		return ESP32H2_CHIP;
	case ICE_CHIP_ESP32C6:
		return ESP32C6_CHIP;
	case ICE_CHIP_ESP32P4:
		return ESP32P4_CHIP;
	default:
		return ESP_UNKNOWN_CHIP;
	}
}

char *esf_find_esp_port(enum ice_chip required)
{
	char **ports;
	int n = serial_list_ports(&ports);

	if (n <= 0)
		return NULL;

	char *found = NULL;
	for (int i = 0; ports[i] && !found; i++) {
		fprintf(stderr, "  Trying %s... ", ports[i]);
		fflush(stderr);

		esf_port_t sport = {
		    .port.ops = &esf_port_ops,
		    .device = ports[i],
		    .baudrate = 115200,
		};

		esp_loader_t probe;
		if (esp_loader_init_uart(&probe, &sport.port) !=
		    ESP_LOADER_SUCCESS) {
			fprintf(stderr, "open failed\n");
			continue;
		}

		esp_loader_connect_args_t ca = ESP_LOADER_CONNECT_DEFAULT();
		if (esp_loader_connect(&probe, &ca) == ESP_LOADER_SUCCESS) {
			target_chip_t esf_chip = esp_loader_get_target(&probe);
			enum ice_chip chip = ice_chip_from_esf(esf_chip);
			if (required == ICE_CHIP_UNKNOWN || chip == required) {
				fprintf(stderr, "%s\n", ice_chip_name(chip));
				esp_loader_reset_target(&probe);
				size_t len = strlen(ports[i]);
				found = malloc(len + 1);
				if (found)
					memcpy(found, ports[i], len + 1);
			} else {
				fprintf(stderr, "%s (want %s)\n",
					ice_chip_name(chip),
					ice_chip_name(required));
			}
		} else {
			fprintf(stderr, "no response\n");
		}
		esp_loader_deinit(&probe);
	}

	serial_free_port_list(ports);
	return found;
}

/* ------------------------------------------------------------------ */
/*  Vtable                                                             */

const esp_loader_port_ops_t esf_port_ops = {
    .init = ice_port_init,
    .deinit = ice_port_deinit,
    .enter_bootloader = ice_enter_bootloader,
    .reset_target = ice_reset_target,
    .start_timer = ice_start_timer,
    .remaining_time = ice_remaining_time,
    .delay_ms = ice_delay_ms,
    .debug_print = NULL, /* set to ice_debug_print to enable trace */
    .change_transmission_rate = ice_change_rate,
    .write = ice_port_write,
    .read = ice_port_read,
    .spi_set_cs = NULL,
    .sdio_write = NULL,
    .sdio_read = NULL,
    .sdio_card_init = NULL,
};
