/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file serial.c
 * @brief Application-layer helpers built on top of @ref serial.h.
 *
 * Platform-specific code (open, read, write, modem lines, port
 * enumeration) lives in platform/{posix,win}/serial.c.  This file
 * contains only the OS-agnostic logic that sits above those primitives
 * and may call into ice.h (e.g. complete_emit).
 */
#include <ctype.h>

#include "ice.h"
#include "serial.h"

void serial_complete_port(void)
{
	char **ports;
	int n = serial_list_ports(&ports);

	if (n <= 0)
		return;
	for (char **p = ports; *p; p++)
		complete_emit(*p, NULL);
	serial_free_port_list(ports);
}

/*
 * Rank a serial port for @ref serial_pick_default_port and
 * @ref serial_sort_ports.  Lower is better; >= 99 is never picked
 * automatically.
 *
 * The USB vendor ID decides when the OS reports one: a USB-UART
 * bridge (CP210x, CH340, FT232, PL2303) beats Espressif's own
 * USB-Serial/JTAG CDC interface.  Otherwise the naming heuristics
 * exploit the fact that bridges and native CDC interfaces use
 * distinct path prefixes:
 *
 *   - Linux       /dev/ttyUSB*       -> bridge   (preferred)
 *                 /dev/ttyACM*       -> CDC      (fallback)
 *
 *   - macOS       /dev/cu.usbserial*           -> bridge (preferred)
 *                 /dev/cu.SLAB_USBtoUART       -> bridge (preferred, CP210x)
 *                 /dev/cu.wchusbserial*        -> bridge (preferred, CH340)
 *                 /dev/cu.usbmodem*            -> CDC    (fallback)
 *                 /dev/cu.Bluetooth*,
 *                 /dev/cu.debug-console,
 *                 /dev/cu.wlan-debug           -> dropped
 *
 *   - Windows     COM*               -> accepted, no naming-based hint
 *
 * The "prefer bridge" rule mirrors esp-idf-monitor's preference for
 * /dev/ttyUSB0, and (more importantly for ice debug) avoids picking
 * the chip's own USB-SJ CDC interface for the UART pane when the
 * physical UART bridge is also plugged in -- the bridge is what the
 * user almost certainly wants for log output and interactive input.
 */
static int rank_port(const struct serial_port_info *pi)
{
	const char *path = pi->path;

	if (pi->usb) {
		switch (pi->vid) {
		case 0x10c4: /* Silicon Labs CP210x */
		case 0x1a86: /* WCH CH340 / CH343 */
		case 0x0403: /* FTDI */
		case 0x067b: /* Prolific PL2303 */
			return 0;
		case 0x303a: /* Espressif USB-Serial/JTAG, TinyUSB CDC */
			return 1;
		}
	}

	/* Linux / macOS USB-UART bridges. */
	if (!strncmp(path, "/dev/ttyUSB", 11))
		return 0;
	if (!strncmp(path, "/dev/cu.usbserial", 17))
		return 0;
	if (!strcmp(path, "/dev/cu.SLAB_USBtoUART"))
		return 0;
	if (!strncmp(path, "/dev/cu.wchusbserial", 20))
		return 0;

	/* Native USB-Serial/JTAG CDC: usable but second-class for monitor. */
	if (!strncmp(path, "/dev/ttyACM", 11))
		return 1;
	if (!strncmp(path, "/dev/cu.usbmodem", 16))
		return 1;

	/* macOS system / Bluetooth ports -- never an ESP. */
	if (!strncmp(path, "/dev/cu.Bluetooth", 17))
		return 99;
	if (!strcmp(path, "/dev/cu.debug-console"))
		return 99;
	if (!strcmp(path, "/dev/cu.wlan-debug"))
		return 99;

	/* Windows COM*, anything else: accept, no preference. */
	return 50;
}

/* Natural order for paths: "COM9" before "COM10", "ttyUSB2" before
 * "ttyUSB10". */
static int path_cmp(const char *a, const char *b)
{
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			unsigned long na = strtoul(a, (char **)&a, 10);
			unsigned long nb = strtoul(b, (char **)&b, 10);
			if (na != nb)
				return na < nb ? -1 : 1;
			continue;
		}
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static int port_info_cmp(const void *va, const void *vb)
{
	const struct serial_port_info *a = va, *b = vb;
	int ra = rank_port(a), rb = rank_port(b);

	if (ra != rb)
		return ra - rb;
	return path_cmp(a->path, b->path);
}

void serial_sort_ports(struct serial_port_info *ports, int n)
{
	if (n > 1)
		qsort(ports, (size_t)n, sizeof *ports, port_info_cmp);
}

char *serial_pick_default_port(void)
{
	struct serial_port_info *ports;
	int n = serial_list_port_info(&ports);
	char *out = NULL;

	if (n <= 0)
		return NULL;
	serial_sort_ports(ports, n);
	if (rank_port(&ports[0]) < 99)
		out = sbuf_strdup(ports[0].path);
	serial_free_port_info(ports, n);
	return out;
}

void serial_complete_baud(void)
{
	/* Standard rates ESP-IDF / esptool support.  Users can still type
	 * any value -- this is just suggestions. */
	static const char *const rates[] = {
	    "9600",   "19200",	"38400",  "57600",   "74880",	"115200",
	    "230400", "460800", "921600", "1500000", "2000000", NULL,
	};
	for (const char *const *r = rates; *r; r++)
		complete_emit(*r, NULL);
}

int serial_list_port_info(struct serial_port_info **out)
{
	struct serial_port_info *ports;
	char **paths;
	int n = serial_list_ports(&paths);

	*out = NULL;
	if (n <= 0) {
		serial_free_port_list(paths); /* may be an empty array */
		return n;
	}

	ports = calloc((size_t)n, sizeof(*ports));
	if (!ports) {
		serial_free_port_list(paths);
		return -ENOMEM;
	}
	/* Hand the path strings over; only the array itself is freed. */
	for (int i = 0; i < n; i++)
		ports[i].path = paths[i];
	free(paths);

	serial_fill_usb_info(ports, n);
	*out = ports;
	return n;
}

void serial_free_port_info(struct serial_port_info *ports, int n)
{
	if (!ports)
		return;
	for (int i = 0; i < n; i++) {
		free(ports[i].path);
		free(ports[i].product);
		free(ports[i].manufacturer);
		free(ports[i].serial);
		free(ports[i].location);
		free(ports[i].usb_interface);
	}
	free(ports);
}

/* Windows accepts "\\.\COM10" as well as "COM10"; compare without it. */
static const char *strip_device_prefix(const char *path)
{
	return strncmp(path, "\\\\.\\", 4) ? path : path + 4;
}

int serial_get_usb_id(const char *device, unsigned int *vid, unsigned int *pid)
{
	/* Look up this one port only: every connect lands here, and
	 * auto-detect connects to each candidate in turn. */
	struct serial_port_info pi = {0};
	int rc = -1;

	pi.path = sbuf_strdup(strip_device_prefix(device));
	serial_fill_usb_info(&pi, 1);
	if (pi.usb) {
		*vid = pi.vid;
		*pid = pi.pid;
		rc = 0;
	}
	free(pi.path);
	free(pi.product);
	free(pi.manufacturer);
	free(pi.serial);
	free(pi.location);
	free(pi.usb_interface);
	return rc;
}
