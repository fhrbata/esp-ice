/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/monitor/monitor.c
 * @brief `ice target monitor` -- plumbing serial port monitor.
 *
 * Opens an explicit serial port and bridges it to the terminal.
 * Has no knowledge of project profiles, build directories, or
 * config files.  The porcelain wrapper `ice monitor` resolves those
 * details and calls into this command with the resulting port path.
 *
 * Usage:
 *   ice target monitor --port <dev> [--baud <rate>] [--no-reset]
 */
#include "ice.h"
#include "serial.h"

static const char *opt_port;
static int opt_baud = 115200;
static int opt_no_reset;

/* clang-format off */
static const struct option cmd_target_monitor_opts[] = {
	OPT_STRING('p', "port", &opt_port, "dev",
		   "serial port device (required)", NULL),
	OPT_INT('b', "baud", &opt_baud, "rate",
		"baud rate (default: 115200)", NULL),
	OPT_BOOL(0, "no-reset", &opt_no_reset,
		 "open port without touching DTR/RTS"),
	OPT_END(),
};

static const struct cmd_manual target_monitor_manual = {
	.name = "ice target monitor",
	.summary = "open a serial port for monitoring",

	.description =
	H_PARA("Low-level serial port monitor.  Opens @b{--port}, "
	       "optionally deasserts DTR/RTS to prevent an accidental "
	       "reset, then bridges the port to the terminal.  "
	       "Press @b{Ctrl-]} to exit.")
	H_PARA("This command reads no project configuration.  It is the "
	       "plumbing behind @b{ice monitor}, which resolves the port "
	       "and baud rate from the project profile."),

	.examples =
	H_EXAMPLE("ice target monitor --port /dev/ttyUSB0")
	H_EXAMPLE("ice target monitor -p /dev/ttyACM0 -b 460800")
	H_EXAMPLE("ice target monitor -p /dev/ttyUSB0 --no-reset"),

	.extras =
	H_SECTION("KEY BINDINGS")
	H_ITEM("Ctrl-]", "Exit the monitor.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice monitor",
	       "Porcelain wrapper: resolves port from project profile."),
};
/* clang-format on */

int cmd_target_monitor(int argc, const char **argv);

const struct cmd_desc cmd_target_monitor_desc = {
    .name = "monitor",
    .fn = cmd_target_monitor,
    .opts = cmd_target_monitor_opts,
    .manual = &target_monitor_manual,
};

int cmd_target_monitor(int argc, const char **argv)
{
	opt_port = NULL;
	opt_baud = 115200;
	opt_no_reset = 0;

	argc = parse_options(argc, argv, &cmd_target_monitor_desc);
	if (argc > 0)
		die("unexpected argument '%s'", argv[0]);

	if (!opt_port)
		die("--port is required");

	unsigned baud = (unsigned)opt_baud;
	struct serial *s;
	unsigned char buf[1024];
	int rc;

	rc = serial_open(&s, opt_port);
	if (rc)
		die("cannot open %s: %s", opt_port, strerror(-rc));

	/*
	 * Deassert DTR and RTS so the auto-reset circuit is not triggered.
	 * The OS may assert DTR on open, which on ESP32 dev boards drives
	 * BOOT low and can reset the chip into bootloader mode.
	 * --no-reset skips this entirely so the device is not disturbed.
	 */
	if (!opt_no_reset) {
		serial_set_dtr(s, 0);
		serial_set_rts(s, 0);
	}

	rc = serial_set_baud(s, baud);
	if (rc) {
		serial_close(s);
		die("cannot set baud rate %u on %s: %s", baud, opt_port,
		    strerror(-rc));
	}

	fprintf(stderr, "--- ice monitor on %s at %u baud ---" EOL, opt_port,
		baud);
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
