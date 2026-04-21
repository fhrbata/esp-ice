/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/flash/flash.c
 * @brief `ice target flash` -- plumbing flash command.
 *
 * Operates on an explicitly specified serial port; has no knowledge of
 * project profiles, build directories, or config files.  Intended to
 * be called directly by the user for raw flashing, or invoked by the
 * porcelain `ice flash` after it resolves those details from project
 * state.
 *
 * Usage:
 *   ice target flash --port <dev> [--chip <name>] [--baud <rate>]
 *                    <addr>=<file> [<addr>=<file> ...]
 *
 * Each positional is an <addr>=<file> pair where <addr> is a hex flash
 * offset (0x prefix optional) and <file> is the path to the binary.
 */
#include "esf_port.h"
#include "ice.h"
#include <inttypes.h>

static const char *opt_port;
static const char *opt_chip;
static int opt_baud = 460800;

/* clang-format off */
static const struct option cmd_target_flash_opts[] = {
	OPT_STRING('p', "port", &opt_port, "dev",
		   "serial port device (required)", NULL),
	OPT_STRING('c', "chip", &opt_chip, "name",
		   "expected chip name for verification (e.g. esp32c6)", NULL),
	OPT_INT('b', "baud", &opt_baud, "rate",
		"negotiated baud rate after ROM handshake (default: 460800)",
		NULL),
	OPT_END(),
};

static const struct cmd_manual target_flash_manual = {
	.name = "ice target flash",
	.summary = "flash binary images to an explicitly specified device",

	.description =
	H_PARA("Low-level flash command.  Connects to the serial port given "
	       "by @b{--port}, optionally verifies the connected chip against "
	       "@b{--chip}, then writes each @b{addr=file} image at the "
	       "specified flash offset.  Resets the device when done.")
	H_PARA("This command operates on raw arguments only -- it reads no "
	       "project configuration.  It is the plumbing behind "
	       "@b{ice flash}, which resolves the port, chip, and file list "
	       "from the project build directory."),

	.examples =
	H_EXAMPLE("ice target flash --port /dev/ttyUSB0 "
		  "0x0=build/bootloader/bootloader.bin "
		  "0x8000=build/partition_table/partition-table.bin "
		  "0x10000=build/hello_world.bin")
	H_EXAMPLE("ice target flash -p /dev/ttyACM0 --chip esp32s3 "
		  "--baud 921600 "
		  "0x0=bootloader.bin 0x8000=partition-table.bin "
		  "0x10000=app.bin"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice flash",
	       "Porcelain wrapper: resolves port and file list from the "
	       "project build directory."),
};
/* clang-format on */

int cmd_target_flash(int argc, const char **argv);

const struct cmd_desc cmd_target_flash_desc = {
    .name = "flash",
    .fn = cmd_target_flash,
    .opts = cmd_target_flash_opts,
    .manual = &target_flash_manual,
};

#define FLASH_BLOCK_SIZE 4096u
#define BAR_WIDTH 24

/*
 * Print (or update) a one-line progress entry.
 *
 * On a color-capable tty: emits \r so repeated calls rewrite the same
 * line; done=1 appends \n to commit it.  On plain output (pipe, CI):
 * prints a single line only when done=1.
 */
static void print_progress(const char *label, uint32_t offset, uint32_t written,
			   uint32_t total, int done)
{
	int pct = total ? (int)(written * 100u / total) : 100;
	int filled = BAR_WIDTH * pct / 100;
	char bar[BAR_WIDTH + 1];

	for (int i = 0; i < BAR_WIDTH; i++)
		bar[i] = (i < filled) ? '=' : ' ';
	bar[BAR_WIDTH] = '\0';

	if (use_color_for(stdout)) {
		printf("\r  %-24s  @c{0x%05" PRIx32 "}  %6.1f KB"
		       "  [@g{%.*s}%s]  %3d%%",
		       label, offset, total / 1024.0, filled, bar, bar + filled,
		       pct);
		if (done)
			printf("  @G{done}\n");
		fflush(stdout);
	} else if (done) {
		printf("  %-24s  0x%05" PRIx32 "  %6.1f KB\n", label, offset,
		       total / 1024.0);
	}
}

/* Emit a bare newline to terminate a partial progress line on error. */
static void progress_abort(void)
{
	if (use_color_for(stdout)) {
		printf("\n");
		fflush(stdout);
	}
}

static int flash_one(esp_loader_t *loader, const char *path, uint32_t offset)
{
	struct sbuf data = SBUF_INIT;
	int rc = 1;

	if (sbuf_read_file(&data, path) < 0) {
		fprintf(stderr, "ice target flash: cannot read %s: %s\n", path,
			strerror(errno));
		goto out;
	}

	uint32_t image_size = (uint32_t)data.len;
	while (image_size % 4)
		image_size++;

	const char *label = strrchr(path, '/');
	label = label ? label + 1 : path;

	esp_loader_flash_cfg_t cfg = {
	    .offset = offset,
	    .image_size = image_size,
	    .block_size = FLASH_BLOCK_SIZE,
	};

	esp_loader_error_t err = esp_loader_flash_start(loader, &cfg);
	if (err != ESP_LOADER_SUCCESS) {
		fprintf(stderr, "ice target flash: flash_start failed (%d)\n",
			err);
		goto out;
	}

	print_progress(label, offset, 0, image_size, 0);

	const uint8_t *p = (const uint8_t *)data.buf;
	uint32_t remaining = image_size;
	uint32_t written = 0;

	while (remaining > 0) {
		uint32_t chunk =
		    remaining < FLASH_BLOCK_SIZE ? remaining : FLASH_BLOCK_SIZE;

		uint8_t block[FLASH_BLOCK_SIZE];
		uint32_t real =
		    (uint32_t)(data.len -
			       (size_t)(p - (const uint8_t *)data.buf));
		if (real > chunk)
			real = chunk;
		memcpy(block, p, real);
		if (real < chunk)
			memset(block + real, 0xff, chunk - real);

		err = esp_loader_flash_write(loader, &cfg, block, chunk);
		if (err != ESP_LOADER_SUCCESS) {
			progress_abort();
			fprintf(stderr, "ice target flash: write failed (%d)\n",
				err);
			goto out;
		}

		p += real;
		written += chunk;
		remaining -= chunk;
		print_progress(label, offset, written, image_size, 0);
	}

	err = esp_loader_flash_finish(loader, &cfg);
	if (err != ESP_LOADER_SUCCESS) {
		progress_abort();
		fprintf(stderr, "ice target flash: MD5 verify failed (%d)\n",
			err);
		goto out;
	}

	print_progress(label, offset, image_size, image_size, 1);
	rc = 0;
out:
	sbuf_release(&data);
	return rc;
}

int cmd_target_flash(int argc, const char **argv)
{
	opt_port = NULL;
	opt_chip = NULL;
	opt_baud = 460800;

	argc = parse_options(argc, argv, &cmd_target_flash_desc);

	if (!opt_port)
		die("--port is required");
	if (argc < 1)
		die("at least one addr=file argument is required");

	/* ---- parse addr=file positionals ---- */
	uint32_t *offsets = malloc((size_t)argc * sizeof(*offsets));
	const char **paths = malloc((size_t)argc * sizeof(*paths));
	if (!offsets || !paths)
		die_errno("malloc");

	for (int i = 0; i < argc; i++) {
		const char *eq = strchr(argv[i], '=');
		if (!eq)
			die("invalid argument '%s': expected addr=file",
			    argv[i]);
		offsets[i] = (uint32_t)strtoul(argv[i], NULL, 16);
		paths[i] = eq + 1;
	}

	/* ---- resolve optional chip filter ---- */
	enum ice_chip required_chip = ice_chip_from_idf_name(opt_chip);

	/* ---- open port and connect ---- */
	unsigned baud = (unsigned)opt_baud;

	esf_port_t sport = {
	    .port.ops = &esf_port_ops,
	    .device = opt_port,
	    .baudrate = 115200,
	};

	printf("Connecting to @b{%s}...\n", opt_port);
	fflush(stdout);

	esp_loader_t loader;
	esp_loader_error_t err = esp_loader_init_uart(&loader, &sport.port);
	if (err != ESP_LOADER_SUCCESS) {
		fprintf(stderr, "ice target flash: failed to open %s\n",
			opt_port);
		free(offsets);
		free(paths);
		return 1;
	}

	esp_loader_connect_args_t connect = ESP_LOADER_CONNECT_DEFAULT();
	err = esp_loader_connect(&loader, &connect);
	if (err != ESP_LOADER_SUCCESS) {
		fprintf(stderr,
			"ice target flash: connect failed — is the device in "
			"bootloader mode?\n"
			"  (Hold BOOT, tap RESET, then release BOOT)\n");
		esp_loader_deinit(&loader);
		free(offsets);
		free(paths);
		return 1;
	}

	enum ice_chip chip = ice_chip_from_esf(esp_loader_get_target(&loader));

	if (required_chip != ICE_CHIP_UNKNOWN && chip != required_chip) {
		fprintf(stderr,
			"ice target flash: chip mismatch — connected to "
			"@b{%s}, expected @b{%s}\n",
			ice_chip_name(chip), ice_chip_name(required_chip));
		esp_loader_deinit(&loader);
		free(offsets);
		free(paths);
		return 1;
	}

	printf("Connected  @G{%s}", ice_chip_name(chip));

	if (baud != 115200) {
		err = esp_loader_change_transmission_rate(&loader, baud);
		if (err != ESP_LOADER_SUCCESS) {
			printf("  @y{(baud change to %u failed, staying at "
			       "115200)}",
			       baud);
		} else {
			printf("  @c{%u baud}", baud);
		}
	}
	printf("\n\n");

	/* ---- flash each image ---- */
	int rc = 0;
	int n_flashed = 0;

	for (int i = 0; i < argc; i++) {
		if (flash_one(&loader, paths[i], offsets[i]) != 0) {
			rc = 1;
			break;
		}
		n_flashed++;
	}

	/* ---- summary and reset ---- */
	printf("\n");
	if (rc == 0) {
		printf("@G{Flashed %d image%s.}  Resetting device...\n",
		       n_flashed, n_flashed == 1 ? "" : "s");
		esp_loader_reset_target(&loader);
	} else {
		printf("@R{Flash failed after %d image%s.}\n", n_flashed,
		       n_flashed == 1 ? "" : "s");
	}

	esp_loader_deinit(&loader);
	free(offsets);
	free(paths);
	return rc;
}
