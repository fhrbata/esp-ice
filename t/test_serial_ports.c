/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for serial_sort_ports() -- the order `ice target probe`
 * prints and serial_pick_default_port() picks from.
 */
#include "ice.h"
#include "serial.h"
#include "tap.h"

/* Build a port array from path / VID pairs; vid 0 means "not USB". */
static struct serial_port_info *make_ports(const char *const *paths,
					   const unsigned int *vids, int n)
{
	struct serial_port_info *p = calloc((size_t)n, sizeof *p);

	for (int i = 0; i < n; i++) {
		p[i].path = sbuf_strdup(paths[i]);
		p[i].usb = vids[i] != 0;
		p[i].vid = vids[i];
	}
	return p;
}

static int order_is(const struct serial_port_info *p, const char *const *want,
		    int n)
{
	for (int i = 0; i < n; i++)
		if (strcmp(p[i].path, want[i]) != 0)
			return 0;
	return 1;
}

int main(void)
{
	/* The USB vendor ID wins over the path: bridge, then Espressif,
	 * then a port the OS knows nothing about. */
	{
		const char *paths[] = {"COM7", "COM3", "COM5"};
		const unsigned int vids[] = {0, 0x303a, 0x10c4};
		const char *want[] = {"COM5", "COM3", "COM7"};
		struct serial_port_info *p = make_ports(paths, vids, 3);

		serial_sort_ports(p, 3);
		tap_check(order_is(p, want, 3));
		serial_free_port_info(p, 3);
		tap_done("bridge VID, then Espressif VID, then unknown");
	}

	/* Every known bridge vendor ranks as a bridge. */
	{
		const char *paths[] = {"COM1", "COM2", "COM3", "COM4", "COM5"};
		const unsigned int vids[] = {0x303a, 0x1a86, 0x0403, 0x067b,
					     0x10c4};
		struct serial_port_info *p = make_ports(paths, vids, 5);

		serial_sort_ports(p, 5);
		tap_check(!strcmp(p[4].path, "COM1"));
		serial_free_port_info(p, 5);
		tap_done("CH340, FTDI, PL2303, CP210x all rank as bridges");
	}

	/* Without USB metadata the path decides: ttyUSB before ttyACM. */
	{
		const char *paths[] = {"/dev/ttyACM0", "/dev/ttyUSB1"};
		const unsigned int vids[] = {0, 0};
		const char *want[] = {"/dev/ttyUSB1", "/dev/ttyACM0"};
		struct serial_port_info *p = make_ports(paths, vids, 2);

		serial_sort_ports(p, 2);
		tap_check(order_is(p, want, 2));
		serial_free_port_info(p, 2);
		tap_done("path fallback prefers ttyUSB over ttyACM");
	}

	/* Equal rank: natural order, so COM10 follows COM9. */
	{
		const char *paths[] = {"COM10", "COM2", "COM9"};
		const unsigned int vids[] = {0, 0, 0};
		const char *want[] = {"COM2", "COM9", "COM10"};
		struct serial_port_info *p = make_ports(paths, vids, 3);

		serial_sort_ports(p, 3);
		tap_check(order_is(p, want, 3));
		serial_free_port_info(p, 3);
		tap_done("equal rank sorts paths naturally");
	}

	/* macOS system ports go last. */
	{
		const char *paths[] = {"/dev/cu.Bluetooth-Incoming-Port",
				       "/dev/cu.usbmodem1101"};
		const unsigned int vids[] = {0, 0};
		const char *want[] = {"/dev/cu.usbmodem1101",
				      "/dev/cu.Bluetooth-Incoming-Port"};
		struct serial_port_info *p = make_ports(paths, vids, 2);

		serial_sort_ports(p, 2);
		tap_check(order_is(p, want, 2));
		serial_free_port_info(p, 2);
		tap_done("macOS Bluetooth port sorts last");
	}

	/* Freeing an empty list is safe. */
	serial_free_port_info(NULL, 0);
	tap_done("serial_free_port_info(NULL, 0) is safe");

	return tap_result();
}
