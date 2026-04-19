/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/monitor/monitor.c
 * @brief The "ice idf monitor" subcommand -- serial port monitor.
 *
 * Connects to a serial port, displays device output in real time,
 * and forwards keyboard input to the device.  Press Ctrl-] to exit.
 */
#include "ice.h"
#include "serial.h"

static const char *opt_port;
static int opt_baud = 115200;

/* clang-format off */
static const struct option cmd_monitor_opts[] = {
	OPT_STRING_CFG('p', "port", &opt_port, "path",
		       "serial.port", "ESPPORT",
		       "serial port device path",
		       "Serial device path "
		       "(@b{/dev/ttyUSB0}, @b{COM3}, ...).", NULL),
	OPT_INT_CFG('b', "baud", &opt_baud, "rate",
		    "serial.baud", "ESPBAUD",
		    "baud rate",
		    "Monitor baud rate (default @b{115200}).", NULL),
	OPT_END(),
};

static const struct cmd_manual monitor_manual = {
	.name = "ice idf monitor",
	.summary = "display serial output from the device",

	.description =
	H_PARA("Connects to a serial port and displays device output "
	       "in real time.  Keyboard input is forwarded to the "
	       "device.  Press @b{Ctrl-]} to exit."),

	.examples =
	H_EXAMPLE("ice idf monitor")
	H_EXAMPLE("ice idf monitor -p /dev/ttyUSB0")
	H_EXAMPLE("ice idf monitor -p /dev/ttyUSB0 -b 460800"),

	.extras =
	H_SECTION("KEY BINDINGS")
	H_ITEM("Ctrl-]", "Exit the monitor."),
};
/* clang-format on */

const struct cmd_desc cmd_monitor_desc = {
    .name = "monitor",
    .fn = cmd_monitor,
    .opts = cmd_monitor_opts,
    .manual = &monitor_manual,
};

int cmd_monitor(int argc, const char **argv)
{
	struct serial *s;
	unsigned char buf[1024];
	int rc;

	parse_options(argc, argv, &cmd_monitor_desc);

	if (!opt_port)
		die("serial.port is not set; use -p or "
		    "'ice config serial.port <path>'");

	rc = serial_open(&s, opt_port);
	if (rc)
		die("cannot open %s: %s", opt_port, strerror(-rc));

	rc = serial_set_baud(s, (unsigned)opt_baud);
	if (rc) {
		serial_close(s);
		die("cannot set baud rate %d on %s: %s", opt_baud, opt_port,
		    strerror(-rc));
	}

	fprintf(stderr, "--- ice idf monitor on %s at %d baud ---" EOL,
		opt_port, opt_baud);
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
