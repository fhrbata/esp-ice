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
static const struct cmd_manual flash_manual = {
	.name = "ice flash",
	.summary = "flash firmware to the device",

	.description =
	H_PARA("Invokes cmake's @b{flash} target to program the "
	       "compiled firmware -- application, bootloader, partition "
	       "table -- to a connected ESP device over serial.  The "
	       "underlying flasher reads @b{ESPPORT} and @b{ESPBAUD} "
	       "from the environment for the serial port and baud rate.")
	H_PARA("@b{[<name>]} selects the project profile (default: "
	       "@b{default}).  Missing build artifacts cause cmake to "
	       "build them as a side effect, so a separate @b{ice build} "
	       "is only needed when you want the captured progress display."),

	.examples =
	H_EXAMPLE("ice flash")
	H_EXAMPLE("ice flash production")
	H_EXAMPLE("ESPPORT=/dev/ttyUSB1 ice flash"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Build before flashing, with captured progress output."),
};
/* clang-format on */

static const struct option cmd_flash_opts[] = {
    OPT_POSITIONAL("[<name>]", complete_profile_names),
    OPT_END(),
};

const struct cmd_desc cmd_flash_desc = {
    .name = "flash",
    .fn = cmd_flash,
    .opts = cmd_flash_opts,
    .manual = &flash_manual,
};

int cmd_flash(int argc, const char **argv)
{
	const char *name;
	const char *build_dir;
	struct process proc = PROCESS_INIT;
	const char *cmake_argv[6];

	argc = parse_options(argc, argv, &cmd_flash_desc);
	if (argc > 1)
		die("too many arguments");
	name = argc >= 1 ? argv[0] : "default";

	load_profile(name);
	require_project_initialized();

	build_dir = config_get("project.build-dir");
	cmake_argv[0] = "cmake";
	cmake_argv[1] = "--build";
	cmake_argv[2] = build_dir;
	cmake_argv[3] = "--target";
	cmake_argv[4] = "flash";
	cmake_argv[5] = NULL;

	proc.argv = cmake_argv;
	return process_run(&proc);
}
