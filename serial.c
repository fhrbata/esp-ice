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
#include "serial.h"
#include "ice.h"

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
 * Rank a serial-port path for @ref serial_pick_default_port.  Lower
 * is better; >= 99 is filtered out entirely.  The naming heuristics
 * exploit the fact that USB-UART bridges (CP210x, CH340, FT232) and
 * native USB-Serial/JTAG CDC interfaces use distinct path prefixes
 * on the platforms ice supports:
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
 *                                       (would need SetupAPI VID/PID
 *                                       lookup; future work)
 *
 * The "prefer bridge" rule mirrors esp-idf-monitor's preference for
 * /dev/ttyUSB0, and (more importantly for ice debug) avoids picking
 * the chip's own USB-SJ CDC interface for the UART pane when the
 * physical UART bridge is also plugged in -- the bridge is what the
 * user almost certainly wants for log output and interactive input.
 */
static int rank_port(const char *path)
{
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

char *serial_pick_default_port(void)
{
	char **ports;
	int n = serial_list_ports(&ports);
	if (n <= 0)
		return NULL;

	const char *best = NULL;
	int best_rank = 99;
	for (char **p = ports; *p; p++) {
		int r = rank_port(*p);
		if (r < best_rank) {
			best_rank = r;
			best = *p;
		}
	}

	char *out = NULL;
	if (best_rank < 99 && best)
		out = sbuf_strdup(best);
	serial_free_port_list(ports);
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
