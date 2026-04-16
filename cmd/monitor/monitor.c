/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/monitor/monitor.c
 * @brief The "ice monitor" subcommand -- porcelain wrapper around
 * `ice target monitor`.
 *
 * Calls project_load() to resolve port and chip from the active
 * profile, then delegates to cmd_target_monitor() for the I/O loop.
 */
#include "esf_port.h"
#include "ice.h"

/* Plumbing entry point declared in cmd/target/monitor/monitor.c. */
int cmd_target_monitor(int argc, const char **argv);

int cmd_monitor(int argc, const char **argv);

static const char *opt_port;
static int opt_baud = 115200;
static int opt_no_reset;

/* clang-format off */
static const struct option cmd_monitor_opts[] = {
	OPT_POSITIONAL_OPT("name", complete_profile_names),
	OPT_STRING_CFG('p', "port", &opt_port, "path",
		       "serial.port", "ESPPORT",
		       "serial port device path", NULL, NULL),
	OPT_INT_CFG('b', "baud", &opt_baud, "rate",
		    "serial.baud", "ESPBAUD",
		    "baud rate (default: 115200)", NULL, NULL),
	OPT_BOOL(0, "no-reset", &opt_no_reset,
		 "skip ESP reset/detection; requires -p"),
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
	H_EXAMPLE("ice monitor -p /dev/ttyUSB0 -b 460800")
	H_EXAMPLE("ice monitor -p /dev/ttyUSB0 --no-reset"),

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
	       "Alias for @b{serial.baud} (env scope).")

	H_SECTION("SEE ALSO")
	H_ITEM("ice target monitor",
	       "Plumbing: open a specific port directly."),
};
/* clang-format on */

const struct cmd_desc cmd_monitor_desc = {
    .name = "monitor",
    .fn = cmd_monitor,
    .opts = cmd_monitor_opts,
    .manual = &manual,
};

int cmd_monitor(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_monitor_desc);
	if (argc > 1)
		die("too many arguments");

	project_load(argc >= 1 ? argv[0] : "default");

	if (opt_no_reset && !opt_port)
		die("--no-reset requires -p/--port");

	const char *chip_str = config_get("project.chip");
	const char *port = opt_port;
	unsigned baud = (unsigned)opt_baud;
	char *autoport = NULL;

	if (!port) {
		enum ice_chip scan_chip = ice_chip_from_idf_name(chip_str);

		if (scan_chip != ICE_CHIP_UNKNOWN)
			fprintf(stderr, "Scanning for %s...\n",
				ice_chip_name(scan_chip));
		else
			fprintf(stderr, "Scanning for ESP device...\n");

		autoport = esf_find_esp_port(scan_chip);
		if (!autoport)
			die("no ESP device found; use -p to specify a port");
		port = autoport;
	}

	/* ---- build argv for ice target monitor ---- */
	char baud_str[32];
	snprintf(baud_str, sizeof(baud_str), "%u", baud);

	const char *mon_argv[8];
	int fa = 0;
	mon_argv[fa++] = "ice target monitor";
	mon_argv[fa++] = "--port";
	mon_argv[fa++] = port;
	mon_argv[fa++] = "--baud";
	mon_argv[fa++] = baud_str;
	if (opt_no_reset)
		mon_argv[fa++] = "--no-reset";
	mon_argv[fa] = NULL;

	int rc = cmd_target_monitor(fa, mon_argv);

	free(autoport);
	return rc;
}
