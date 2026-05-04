/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/debug/debug.c
 * @brief `ice debug` -- porcelain wrapper around `ice target debug`.
 *
 * Reads the active profile to fill in:
 *
 *   - @c _project.elf            -- gdb symbol file
 *   - @c _project.chip           -- picks default gdb (overridden if
 *                                   @c _project.gdb-prefix yields a
 *                                   binary on PATH)
 *   - @c _project.openocd-args   -- OpenOCD command-line tail
 *   - @c _project.gdb-prefix     -- e.g. "xtensa-esp32s3-elf-"
 *   - @c serial.port             -- UART for the chip
 *   - @c serial.baud             -- baud rate
 *
 * then delegates to @ref cmd_target_debug for the I/O loop.  Same
 * porcelain-mirrors-plumbing shape as @c{ice monitor} ↔
 * @c{ice target monitor}.
 */
#include "esf_port.h"
#include "ice.h"
#include "sbuf.h"
#include "serial.h"

/* Plumbing entry point declared in cmd/target/debug/debug.c. */
int cmd_target_debug(int argc, const char **argv);

int cmd_debug(int argc, const char **argv);

static const char *opt_port;
static int opt_baud = 115200;
static int opt_no_reset;
static const char *opt_openocd_bin;
static const char *opt_openocd_cmd;
static const char *opt_gdb_bin;
static int opt_gdb_port = 3333;

/* clang-format off */
static const struct option cmd_debug_opts[] = {
	OPT_STRING_CFG('p', "port", &opt_port, "path",
		       "serial.port", "ESPPORT",
		       "serial port device path", NULL, serial_complete_port),
	OPT_INT_CFG('b', "baud", &opt_baud, "rate",
		    "serial.baud", "ESPBAUD",
		    "baud rate (default: 115200)", NULL, NULL),
	OPT_BOOL(0, "no-reset", &opt_no_reset,
		 "open the UART without touching DTR/RTS"),
	OPT_STRING(0, "openocd-bin", &opt_openocd_bin, "path",
		   "OpenOCD binary (default: installed under ~/.ice/tools/)",
		   NULL),
	OPT_STRING(0, "openocd-cmd", &opt_openocd_cmd, "args",
		   "OpenOCD command-line tail "
		   "(default: from project_description.json)", NULL),
	OPT_STRING(0, "gdb-bin", &opt_gdb_bin, "path",
		   "gdb binary (default: from project's monitor_toolprefix)",
		   NULL),
	OPT_INT(0, "gdb-port", &opt_gdb_port, "port",
		"TCP port for the OpenOCD gdb stub (default: 3333)", NULL),
	OPT_END(),
};

static const struct cmd_manual manual = {
	.name = "ice debug",
	.summary = "OpenOCD + gdb dual-pane debug session for the active project",

	.description =
	H_PARA("Spawns OpenOCD against the active project's chip and "
	       "attaches gdb in a dual-pane TUI: gdb on top, the chip's "
	       "UART on the bottom.  Per-pane scrollback / regex search / "
	       "yank live on each pane (@b{Ctrl-T ?} on the focused pane); "
	       "@b{Ctrl-T} prefix at the cmd level reserves @b{Tab} "
	       "(focus), @b{r} (reset), @b{h} (help), @b{x} (quit).")
	H_PARA("All defaults come from the active project: ELF path, chip, "
	       "OpenOCD board file (@b{debug_arguments_openocd}), gdb "
	       "binary (@b{monitor_toolprefix}), serial port and baud "
	       "rate.  Use the @b{--openocd-cmd}, @b{--gdb-bin}, "
	       "@b{--port}, @b{-b} flags to override individual pieces.")
	H_PARA("Assumes the firmware is already running on the chip.  Use "
	       "@b{ice flash} first if you need to program it."),

	.examples =
	H_EXAMPLE("ice debug")
	H_EXAMPLE("ice debug -p /dev/ttyUSB0")
	H_EXAMPLE("ice debug --openocd-cmd \"-f board/esp32-wrover-kit-3.3v.cfg\""),

	.extras =
	H_SECTION("KEY BINDINGS")
	H_ITEM("Ctrl-T Tab",    "Toggle focus between gdb and UART panes.")
	H_ITEM("Ctrl-T r",      "Reset the target (@b{monitor reset halt}).")
	H_ITEM("Ctrl-T h",      "Show command help.")
	H_ITEM("Ctrl-T x",      "Exit and shut down OpenOCD + gdb.")
	H_ITEM("Ctrl-T Ctrl-T", "Send a literal Ctrl-T to the focused pane.")

	H_SECTION("CONFIG")
	H_ITEM("serial.port",
	       "Serial device path (@b{/dev/ttyUSB0}, @b{COM3}, ...).")
	H_ITEM("serial.baud",
	       "UART baud rate (default @b{115200}).")

	H_SECTION("ENVIRONMENT")
	H_ITEM("ESPPORT",
	       "Alias for @b{serial.port} (env scope).")
	H_ITEM("ESPBAUD",
	       "Alias for @b{serial.baud} (env scope).")

	H_SECTION("SEE ALSO")
	H_ITEM("ice target debug",
	       "Plumbing: takes openocd args / chip / ELF on the command line."),
};
/* clang-format on */

const struct cmd_desc cmd_debug_desc = {
    .name = "debug",
    .fn = cmd_debug,
    .opts = cmd_debug_opts,
    .manual = &manual,
    .needs = PROJECT_BUILT,
};

int cmd_debug(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_debug_desc);
	if (argc > 0)
		die("too many arguments");

	const char *target = config_get("_project.target");
	if (target && !strcmp(target, "linux"))
		die("ice debug: linux host build cannot be debugged via "
		    "OpenOCD (no JTAG)");

	const char *elf = config_get("_project.elf");
	if (!elf || !*elf)
		die("ice debug: no ELF resolved (run @b{ice build} first)");

	const char *chip = config_get("_project.chip");
	if (!chip || !*chip)
		die("ice debug: no chip resolved (run @b{ice init} first)");

	/* OpenOCD args: --openocd-cmd > _project.openocd-args.  Either
	 * source has to land something or OpenOCD has nothing to run with
	 * (no board file -> no transport). */
	const char *openocd_cmd = opt_openocd_cmd;
	if (!openocd_cmd)
		openocd_cmd = config_get("_project.openocd-args");
	if (!openocd_cmd || !*openocd_cmd)
		die("ice debug: no OpenOCD args; pass @b{--openocd-cmd "
		    "\"-f board/<your-board>.cfg\"} or rebuild the project so "
		    "@b{debug_arguments_openocd} lands in "
		    "project_description.json");

	/* gdb binary: --gdb-bin > <gdb-prefix>gdb (when prefix is in
	 * project_description.json) > plumbing's chip table fallback. */
	struct sbuf gdb_bin = SBUF_INIT;
	const char *gdb_arg = opt_gdb_bin;
	if (!gdb_arg) {
		const char *gdb_prefix = config_get("_project.gdb-prefix");
		if (gdb_prefix && *gdb_prefix) {
			sbuf_addf(&gdb_bin, "%sgdb", gdb_prefix);
			gdb_arg = gdb_bin.buf;
		}
	}

	const char *port = opt_port;
	char *autoport = NULL;
	if (!port) {
		enum ice_chip scan_chip = ice_chip_from_idf_name(chip);

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

	/* ---- build argv for ice target debug ---- */
	char baud_str[32];
	char gdb_port_str[32];
	snprintf(baud_str, sizeof(baud_str), "%u", (unsigned)opt_baud);
	snprintf(gdb_port_str, sizeof(gdb_port_str), "%d", opt_gdb_port);

	const char *dbg_argv[24];
	int fa = 0;
	dbg_argv[fa++] = "ice target debug";
	dbg_argv[fa++] = "--port";
	dbg_argv[fa++] = port;
	dbg_argv[fa++] = "--baud";
	dbg_argv[fa++] = baud_str;
	dbg_argv[fa++] = "--elf";
	dbg_argv[fa++] = elf;
	dbg_argv[fa++] = "--chip";
	dbg_argv[fa++] = chip;
	dbg_argv[fa++] = "--openocd-cmd";
	dbg_argv[fa++] = openocd_cmd;
	dbg_argv[fa++] = "--gdb-port";
	dbg_argv[fa++] = gdb_port_str;
	if (opt_openocd_bin) {
		dbg_argv[fa++] = "--openocd-bin";
		dbg_argv[fa++] = opt_openocd_bin;
	}
	if (gdb_arg) {
		dbg_argv[fa++] = "--gdb-bin";
		dbg_argv[fa++] = gdb_arg;
	}
	if (opt_no_reset)
		dbg_argv[fa++] = "--no-reset";
	dbg_argv[fa] = NULL;

	int rc = cmd_target_debug(fa, dbg_argv);

	sbuf_release(&gdb_bin);
	free(autoport);
	return rc;
}
