/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/flash/flash.c
 * @brief The "ice flash" subcommand -- invoke the cmake "flash" target.
 */
#include "ice.h"

/* clang-format off */
static const struct cmd_manual manual = {
	.name = "ice flash",
	.summary = "flash firmware to the device",

	.description =
	H_PARA("Invokes cmake's @b{flash} target to program the "
	       "compiled firmware -- application, bootloader, partition "
	       "table -- to a connected ESP device over serial.  The "
	       "underlying flasher reads @b{ESPPORT} and @b{ESPBAUD} "
	       "from the environment for the serial port and baud rate.")
	H_PARA("Missing build artifacts cause cmake to build them as a "
	       "side effect, so a separate @b{ice build} is only needed "
	       "when you want the captured progress display."),

	.examples =
	H_EXAMPLE("ice flash")
	H_EXAMPLE("ESPPORT=/dev/ttyUSB1 ice flash"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Build before flashing, with captured progress output.")
	H_ITEM("ice cmake erase-flash",
	       "Wipe flash before reprogramming."),
};
/* clang-format on */

int cmd_flash(int argc, const char **argv)
{
	struct option opts[] = {OPT_END()};

	parse_options(argc, argv, opts, &manual);
	return run_cmake_target("flash", "flash", 0);
}
