/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/partition/erase/erase.c
 * @brief "ice target partition erase" -- wipe a partition's flash region.
 *
 * Replaces @b{parttool.py erase_partition}.  Resolves the selected
 * partition and calls @c esp_loader_flash_erase_region for its
 * @c offset / @c size.
 */
#include "../partition.h"
#include "esp_loader.h"
#include "ice.h"

/* clang-format off */
static const struct cmd_manual target_partition_erase_manual = {
	.name = "ice target partition erase",
	.summary = "erase a partition's flash region on the device",

	.description =
	H_PARA("Drop-in replacement for @b{parttool.py "
	       "erase_partition}.  Selector and PT source are the same as "
	       "for the other @b{ice target partition} verbs."),

	.examples =
	H_EXAMPLE("ice target partition erase --port /dev/ttyUSB0 --type data --subtype ota")
	H_EXAMPLE("ice target partition erase -p /dev/ttyUSB0 --name nvs"),
};
/* clang-format on */

static const struct option cmd_target_partition_erase_opts[] = {
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

int cmd_target_partition_erase(int argc, const char **argv);

const struct cmd_desc cmd_target_partition_erase_desc = {
    .name = "erase",
    .fn = cmd_target_partition_erase,
    .opts = cmd_target_partition_erase_opts,
    .manual = &target_partition_erase_manual,
};

int cmd_target_partition_erase(int argc, const char **argv)
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
	esp_loader_error_t lerr;

	pt_target_reset_opts();

	argc = parse_options(argc, argv, &cmd_target_partition_erase_desc);
	(void)argc;
	(void)argv;

	pt_target_finalize_opts();

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

	printf("Erasing partition '%s' at 0x%x (0x%x bytes)...\n", e->name,
	       e->offset, e->size);
	fflush(stdout);

	lerr = esp_loader_flash_erase_region(&loader, e->offset, e->size);
	if (lerr != ESP_LOADER_SUCCESS) {
		err("flash_erase_region failed (esp-loader err %d)", lerr);
		goto out;
	}

	printf("Erased partition '%s' at offset 0x%x\n", e->name, e->offset);

	rc = 0;
out:
	free(autoport);
	return rc;
}
