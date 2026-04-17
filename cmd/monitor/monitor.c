/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/monitor/monitor.c
 * @brief The "ice monitor" subcommand -- serial port monitor.
 *
 * Connects to a serial port, displays device output in real time,
 * and forwards keyboard input to the device.  Press Ctrl-] to exit.
 */
#include "ice.h"
#include "serial.h"

/* clang-format off */
static const struct option cmd_monitor_opts[] = {
	OPT_CONFIG('p', "port", "serial.port", "path",
		   "serial port device path", NULL),
	OPT_CONFIG('b', "baud", "serial.baud", "rate",
		   "baud rate (default: 115200)", NULL),
	OPT_END(),
};

static const struct cmd_manual manual = {
	.name = "ice monitor",
	.summary = "display serial output from the device",

	.description =
	H_PARA("Connects to a serial port and displays device output "
	       "in real time.  Keyboard input is forwarded to the "
	       "device.  Press @b{Ctrl-]} to exit.")
	H_PARA("The serial port and baud rate are read from "
	       "@b{serial.port} and @b{serial.baud} in config; the "
	       "legacy @b{ESPPORT} / @b{ESPBAUD} environment variables "
	       "are mapped to the same keys at env scope."),

	.examples =
	H_EXAMPLE("ice monitor")
	H_EXAMPLE("ice monitor -p /dev/ttyUSB0")
	H_EXAMPLE("ice monitor -p /dev/ttyUSB0 -b 460800"),

	.extras =
	H_SECTION("KEY BINDINGS")
	H_ITEM("Ctrl-]", "Exit the monitor.")

	H_SECTION("CONFIG")
	H_ITEM("serial.port",
	       "Serial device path (@b{/dev/ttyUSB0}, @b{COM3}, ...).")
	H_ITEM("serial.baud",
	       "Monitor baud rate (default @b{115200}).")

	H_SECTION("ENVIRONMENT")
	H_ITEM("ESPPORT",
	       "Alias for @b{serial.port} (env scope).")
	H_ITEM("ESPBAUD",
	       "Alias for @b{serial.baud} (env scope)."),
};
/* clang-format on */

int cmd_monitor(int argc, const char **argv)
{
	const char *port;
	const char *baud_str;
	unsigned baud = 115200;
	struct serial *s;
	unsigned char buf[1024];
	int rc;

	parse_options(argc, argv, cmd_monitor_opts, &manual);

	port = config_get("serial.port");
	if (!port)
		die("serial.port is not set; use -p or "
		    "'ice config serial.port <path>'");

	baud_str = config_get("serial.baud");
	if (baud_str)
		baud = (unsigned)atoi(baud_str);

	rc = serial_open(&s, port);
	if (rc)
		die("cannot open %s: %s", port, strerror(-rc));

	rc = serial_set_baud(s, baud);
	if (rc) {
		serial_close(s);
		die("cannot set baud rate %u on %s: %s", baud, port,
		    strerror(-rc));
	}

	fprintf(stderr, "--- ice monitor on %s at %u baud ---" EOL, port, baud);
	fprintf(stderr, "--- Quit: Ctrl-] ---" EOL);

	rc = console_raw_enter();
	if (rc) {
		serial_close(s);
		die("cannot set terminal to raw mode: %s", strerror(-rc));
	}

	serial_flush_input(s);

	for (;;) {
		ssize_t n, i;

		n = serial_read(s, buf, sizeof(buf), 30);
		if (n > 0) {
			fwrite(buf, 1, (size_t)n, stdout);
			fflush(stdout);
		}
		if (n < 0)
			break;

		n = console_read(buf, sizeof(buf), 0);
		if (n < 0)
			break;
		for (i = 0; i < n; i++) {
			if (buf[i] == 0x1D) { /* Ctrl-] */
				if (i > 0)
					serial_write(s, buf, (size_t)i);
				goto done;
			}
		}
		if (n > 0)
			serial_write(s, buf, (size_t)n);
	}

done:
	console_raw_leave();
	serial_close(s);
	fprintf(stderr, EOL "--- exit ---" EOL);
	return 0;
}
