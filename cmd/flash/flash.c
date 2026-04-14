/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/flash/flash.c
 * @brief The "ice flash" subcommand -- invoke the cmake "flash" target.
 */
#include "../../ice.h"

static const struct cmd_manual manual = {
    .description =
	H_PARA("Invokes cmake's @b{flash} target to program the "
	       "compiled firmware -- application, bootloader, partition "
	       "table -- to a connected ESP device over serial.  The "
	       "serial port and baud rate come from @b{serial.port} / "
	       "@b{serial.baud} in config; the legacy @b{ESPPORT} / "
	       "@b{ESPBAUD} environment variables are mapped to the "
	       "same keys at env scope.")
	    H_PARA("Missing build artifacts cause cmake to build them as a "
		   "side effect, so a separate @b{ice build} is only needed "
		   "when you want the captured progress display."),

    .examples = H_EXAMPLE("ice flash")
	H_EXAMPLE("ice config serial.port /dev/ttyUSB0 && ice flash")
	    H_EXAMPLE("ESPPORT=/dev/ttyUSB1 ice flash"),

    .extras = H_SECTION("CONFIG") H_ITEM(
	"serial.port", "Serial device path (@b{/dev/ttyUSB0}, @b{COM3}, ...).")
	H_ITEM("serial.baud",
	       "Flasher baud rate (e.g. @b{115200}, @b{460800}).")

	    H_SECTION("ENVIRONMENT") H_ITEM(
		"ESPPORT", "Alias for @b{serial.port} (env scope).")
		H_ITEM("ESPBAUD", "Alias for @b{serial.baud} (env scope).")

		    H_SECTION("SEE ALSO") H_ITEM(
			"ice build",
			"Build before flashing, with captured progress output.")
			H_ITEM("ice cmake erase-flash",
			       "Wipe flash before reprogramming."),
};

int cmd_flash(int argc, const char **argv)
{
	const char *usage[] = {"ice flash", NULL};
	struct option opts[] = {OPT_END()};

	parse_options_manual(argc, argv, opts, usage, &manual);
	return run_cmake_target("flash", "flash", 0);
}
