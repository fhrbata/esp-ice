/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/flash/flash.c
 * @brief The "ice flash" subcommand -- porcelain wrapper around
 * `ice target flash`.
 *
 * Split into a user-facing porcelain (@c cmd_flash) that spawns
 * @c ice @c __flash under process_run_progress so the full flasher
 * output is captured in a log, and a hidden plumbing command
 * (@c cmd___flash) that does the actual port resolution, chip
 * handshake, and bit-banging.  @c ice @c target @c flash remains the
 * low-level entry for scripted / externally-driven flashing.
 */
#include "cmake.h"
#include "esf_port.h"
#include "ice.h"

/* Plumbing entry point declared in cmd/target/flash/flash.c. */
int cmd_target_flash(int argc, const char **argv);

static const char *opt_port;
static int opt_baud = 460800;

/* clang-format off */
static const struct option cmd_flash_opts[] = {
	OPT_STRING_CFG('p', "port", &opt_port, "dev",
		       "serial.port", "ESPPORT",
		       "serial port device", NULL, NULL),
	OPT_INT_CFG('b', "baud", &opt_baud, "rate",
		    "serial.baud", "ESPBAUD",
		    "baud rate", NULL, NULL),
	OPT_END(),
};

static const struct cmd_manual manual = {
	.name = "ice flash",
	.summary = "flash firmware to the device",
	.description =
	H_PARA("Programs the compiled firmware -- application, bootloader, "
	       "partition table -- directly to a connected ESP device over "
	       "serial using the ESP ROM bootloader protocol.")
	H_PARA("Partition offsets and binary paths are read from "
	       "@b{flasher_args.json} in the build directory, which is "
	       "produced automatically by the IDF cmake build.  The target "
	       "chip is also read from that file and used to auto-detect the "
	       "correct port when none is specified.  The baud rate comes "
	       "from @b{serial.baud} in config; the legacy @b{ESPPORT} / "
	       "@b{ESPBAUD} environment variables are mapped to the same "
	       "keys at env scope."),

	.examples =
	H_EXAMPLE("ice flash")
	H_EXAMPLE("ice --profile production flash")
	H_EXAMPLE("ice flash --port /dev/ttyUSB0")
	H_EXAMPLE("ice flash --port /dev/ttyUSB0 --baud 921600")
	H_EXAMPLE("ice config serial.port /dev/ttyUSB0 && ice flash")
	H_EXAMPLE("ESPPORT=/dev/ttyUSB1 ice flash"),

	.extras =
	H_SECTION("CONFIG")
	H_ITEM("serial.port",
	       "Serial device path (@b{/dev/ttyUSB0}, @b{COM3}, ...).")
	H_ITEM("serial.baud",
	       "Flasher baud rate (e.g. @b{115200}, @b{460800}).  "
	       "Connection always starts at 115200; this rate is negotiated "
	       "after the ROM handshake.")

	H_SECTION("ENVIRONMENT")
	H_ITEM("ESPPORT",
	       "Alias for @b{serial.port} (env scope).")
	H_ITEM("ESPBAUD",
	       "Alias for @b{serial.baud} (env scope).")

	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Build the firmware before flashing.")
	H_ITEM("ice target flash",
	       "Plumbing: flash with explicit port and file list.")
	H_ITEM("ice cmake erase-flash",
	       "Wipe flash before reprogramming."),
};
/* clang-format on */

int cmd_flash(int argc, const char **argv);
int cmd___flash(int argc, const char **argv);

const struct cmd_desc cmd_flash_desc = {
    .name = "flash",
    .fn = cmd_flash,
    .opts = cmd_flash_opts,
    .manual = &manual,
    .needs = PROJECT_BUILT,
};

/* Hidden plumbing: does the actual work.  The porcelain cmd_flash
 * above invokes this via process_run_progress so the combined output
 * lands in a single log under <build>/.ice/logs/. */
const struct cmd_desc cmd___flash_desc = {
    .name = "__flash",
    .fn = cmd___flash,
    .opts = cmd_flash_opts,
    .manual = &manual,
    .needs = PROJECT_BUILT,
};

/* Porcelain wrapper -- forwards to __flash with progress display. */
int cmd_flash(int argc, const char **argv)
{
	struct svec cmd = SVEC_INIT;
	struct process proc = PROCESS_INIT;
	const char *exe = process_exe();
	const char *env_items[2] = {NULL, NULL};
	struct sbuf env_profile = SBUF_INIT;
	int rc;

	/* Capture argv BEFORE parse_options runs -- it compacts argv in
	 * place and we need the original option tokens to forward. */
	svec_push(&cmd, exe ? exe : "ice");
	svec_push(&cmd, "__flash");
	for (int i = 1; i < argc; i++)
		svec_push(&cmd, argv[i]);

	/* Parse for --help / -h / --ice-complete handling and for early
	 * rejection of bad flags, but discard the mutated argv. */
	parse_options(argc, argv, &cmd_flash_desc);

	/* Propagate --profile to the child via ICE_PROFILE; plain env
	 * inheritance already covers the ICE_PROFILE / config / default
	 * cases, so only the CLI-set value needs an explicit push. */
	if (global_profile && *global_profile) {
		sbuf_addf(&env_profile, "ICE_PROFILE=%s", global_profile);
		env_items[0] = env_profile.buf;
		proc.env = env_items;
	}

	proc.argv = cmd.v;
	rc = process_run_progress(&proc, "Flashing", "flash", NULL);

	sbuf_release(&env_profile);
	svec_clear(&cmd);
	return rc;
}

int cmd___flash(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd___flash_desc);
	if (argc > 0)
		die("too many arguments");

	const char *chip_str = config_get("_project.chip");

	struct config_entry **flash_files;
	int n_files = config_get_all("_project.flash-file", &flash_files);
	if (n_files == 0) {
		fprintf(stderr,
			"ice flash: no flash files in flasher_args.json\n"
			"  Run 'ice build' first to generate build "
			"artifacts.\n");
		return 1;
	}

	/* ---- resolve serial port ---- */
	enum ice_chip required_chip = ice_chip_from_idf_name(chip_str);
	const char *port_path = opt_port;
	char *autoport = NULL;

	if (!port_path) {
		if (required_chip != ICE_CHIP_UNKNOWN)
			printf("Scanning for %s...\n",
			       ice_chip_name(required_chip));
		else
			printf("Scanning for ESP device...\n");
		fflush(stdout);

		autoport = esf_find_esp_port(required_chip);
		if (!autoport) {
			fprintf(stderr, "ice flash: no matching device found.\n"
					"  Use --port to specify a port "
					"explicitly.\n");
			free(flash_files);
			return 1;
		}
		port_path = autoport;
	}

	/* ---- build argv for ice target flash ---- */
	/*
	 * Maximum argv size: argv[0] + --port <p> + --chip <c> +
	 * --baud <b> + n_files entries + NULL sentinel.
	 */
	int max_argc = 8 + n_files;
	const char **flash_argv =
	    malloc((size_t)(max_argc + 1) * sizeof(*flash_argv));
	if (!flash_argv)
		die_errno("malloc");

	char baud_str[32];
	snprintf(baud_str, sizeof(baud_str), "%u", (unsigned)opt_baud);

	int fa = 0;
	flash_argv[fa++] = "ice target flash";
	flash_argv[fa++] = "--port";
	flash_argv[fa++] = port_path;
	if (chip_str) {
		flash_argv[fa++] = "--chip";
		flash_argv[fa++] = chip_str;
	}
	flash_argv[fa++] = "--baud";
	flash_argv[fa++] = baud_str;
	for (int i = 0; i < n_files; i++)
		flash_argv[fa++] = flash_files[i]->value; /* "offset=path" */
	flash_argv[fa] = NULL;

	/* ---- delegate to plumbing ---- */
	int rc = cmd_target_flash(fa, flash_argv);

	free(flash_files);
	free(autoport);
	free(flash_argv);
	return rc;
}
