/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/partition/read/read.c
 * @brief "ice target partition read" -- pull bytes off the chip.
 *
 * Replaces @b{parttool.py read_partition}.  Resolves the selected
 * partition (via --name / --type+--subtype / --boot-default) and
 * dumps its full size to @b{--output}.
 */
#include "../partition.h"
#include "esp_loader.h"
#include "ice.h"

/* clang-format off */
static const struct cmd_manual target_partition_read_manual = {
	.name = "ice target partition read",
	.summary = "read a partition's contents from the device into a file",

	.description =
	H_PARA("Drop-in replacement for @b{parttool.py read_partition}.  "
	       "Reads the matched partition's full size starting at its "
	       "flash offset and writes the bytes to @b{--output}.  When "
	       "@b{--partition-table-file} is omitted the table is read "
	       "from the device first."),

	.examples =
	H_EXAMPLE("ice target partition read --port /dev/ttyUSB0 --type data --subtype nvs --output nvs.bin")
	H_EXAMPLE("ice target partition read -p /dev/ttyUSB0 --name storage --output storage.bin"),
};
/* clang-format on */

static const char *opt_output;

static const struct option cmd_target_partition_read_opts[] = {
    OPT_STRING('p', "port", &pt_opt_port, "dev",
	       "serial port (omit to auto-detect)", serial_complete_port),
    OPT_INT('b', "baud", &pt_opt_baud, "rate",
	    "negotiated baud rate (default 460800)", NULL),
    OPT_STRING(0, "partition-table-file", &pt_opt_pt_file, "path",
	       "use a local PT instead of reading from device", NULL),
    OPT_STRING(0, "partition-table-offset", &pt_opt_pt_offset, "hex",
	       "PT offset in flash (default 0x8000)", NULL),
    OPT_STRING('n', "name", &pt_opt_name, "name", "select by partition name",
	       NULL),
    OPT_STRING('t', "type", &pt_opt_type, "T", "select by type", NULL),
    OPT_STRING('s', "subtype", &pt_opt_subtype, "S", "subtype filter", NULL),
    OPT_BOOL('d', "boot-default", &pt_opt_boot_default,
	     "select the default-boot app partition"),
    OPT_STRING('o', "output", &opt_output, "path",
	       "destination file (required)", NULL),
    OPT_STRING(0, "primary-bootloader-offset", &pt_opt_primary_boot_offset,
	       "hex", "primary bootloader offset (CSV partition tables only)",
	       NULL),
    OPT_STRING(0, "recovery-bootloader-offset", &pt_opt_recovery_boot_offset,
	       "hex", "recovery bootloader offset (CSV partition tables only)",
	       NULL),
    OPT_STRING_LIST(0, "extra-partition-subtypes", &pt_opt_extra_subtypes,
		    "T,N,V", "register custom subtypes (e.g. data,foo,0x80)",
		    NULL),
    OPT_BOOL('q', "quiet", &pt_opt_quiet,
	     "suppress progress chatter (parttool.py compat; no-op for now)"),
    OPT_STRING_LIST(0, "esptool-args", &pt_opt_esptool_args, "arg",
		    "rejected; ice does not delegate to esptool", NULL),
    OPT_STRING_LIST(0, "esptool-write-args", &pt_opt_esptool_write_args, "arg",
		    "rejected; ice does not delegate to esptool", NULL),
    OPT_STRING_LIST(0, "esptool-read-args", &pt_opt_esptool_read_args, "arg",
		    "rejected; ice does not delegate to esptool", NULL),
    OPT_STRING_LIST(0, "esptool-erase-args", &pt_opt_esptool_erase_args, "arg",
		    "rejected; ice does not delegate to esptool", NULL),
    OPT_END(),
};

int cmd_target_partition_read(int argc, const char **argv);

const struct cmd_desc cmd_target_partition_read_desc = {
    .name = "read",
    .fn = cmd_target_partition_read,
    .opts = cmd_target_partition_read_opts,
    .manual = &target_partition_read_manual,
};

int cmd_target_partition_read(int argc, const char **argv)
{
	struct pt_entry entries[PT_MAX_ENTRIES];
	int count = 0;
	struct pt_target_selector sel = {0};
	const struct pt_entry *e;
	uint32_t table_offset;
	char *end;
	esp_loader_t loader;
	esf_port_t sport;
	char *autoport = NULL;
	int rc = 1;
	uint8_t *buf = NULL;
	FILE *out = NULL;
	esp_loader_error_t lerr;

	pt_target_reset_opts();
	opt_output = NULL;

	argc = parse_options(argc, argv, &cmd_target_partition_read_desc);
	(void)argc;
	(void)argv;

	pt_target_finalize_opts();

	if (!opt_output)
		die("--output is required");

	sel.name = pt_opt_name;
	sel.type = pt_opt_type;
	sel.subtype = pt_opt_subtype;
	sel.boot_default = pt_opt_boot_default;

	table_offset = (uint32_t)strtoul(pt_opt_pt_offset, &end, 0);
	if (*end)
		die("invalid --partition-table-offset: %s", pt_opt_pt_offset);

	if (pt_target_connect(&loader, &sport, pt_opt_port, pt_opt_baud,
			      &autoport) != 0)
		goto out;

	if (pt_target_load(entries, &count, &loader, table_offset) != 0)
		goto out;

	e = pt_target_select(entries, count, &sel, &loader);
	if (!e)
		goto out;

	buf = malloc(e->size);
	if (!buf) {
		err_errno("malloc %u bytes", e->size);
		goto out;
	}

	printf("Reading partition '%s' (0x%x bytes at 0x%x)...\n", e->name,
	       e->size, e->offset);
	fflush(stdout);

	lerr = esp_loader_flash_read(&loader, buf, e->offset, e->size);
	if (lerr != ESP_LOADER_SUCCESS) {
		err("flash_read failed (esp-loader err %d)", lerr);
		goto out;
	}

	mkdirp_for_file(opt_output);
	out = fopen(opt_output, "wb");
	if (!out) {
		err_errno("cannot write '%s'", opt_output);
		goto out;
	}
	if (fwrite(buf, 1, e->size, out) != e->size) {
		err_errno("write error on '%s'", opt_output);
		goto out;
	}

	printf("Read partition '%s' contents from device at offset 0x%x to "
	       "file '%s'\n",
	       e->name, e->offset, opt_output);

	rc = 0;
out:
	if (out)
		fclose(out);
	free(buf);
	free(autoport);
	return rc;
}
