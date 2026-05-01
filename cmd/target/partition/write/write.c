/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/partition/write/write.c
 * @brief "ice target partition write" -- push bytes to the chip.
 *
 * Replaces @b{parttool.py write_partition}.  Resolves the selected
 * partition, validates the input fits, and writes it via the
 * standard flash_write path.  Honours the @c readonly partition
 * flag unless @b{--ignore-readonly} is set.
 */
#include "../partition.h"
#include "esp_loader.h"
#include "ice.h"

/* clang-format off */
static const struct cmd_manual target_partition_write_manual = {
	.name = "ice target partition write",
	.summary = "write file contents to a partition on the device",

	.description =
	H_PARA("Drop-in replacement for @b{parttool.py write_partition}.  "
	       "The input file is padded with 0xFF up to the next 4-byte "
	       "boundary before being written; partitions flagged as "
	       "@c readonly are refused unless @b{--ignore-readonly} is "
	       "set."),

	.examples =
	H_EXAMPLE("ice target partition write --port /dev/ttyUSB0 --name storage --input storage.bin")
	H_EXAMPLE("ice target partition write --port /dev/ttyUSB0 --type data --subtype nvs --input nvs.bin"),
};
/* clang-format on */

static const char *opt_input;
static int opt_ignore_readonly;

#define WRITE_BLOCK_SIZE 4096u

static const struct option cmd_target_partition_write_opts[] = {
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
    OPT_STRING('i', "input", &opt_input, "path",
	       "source file to write (required)", NULL),
    OPT_BOOL(0, "ignore-readonly", &opt_ignore_readonly,
	     "permit writes to readonly partitions"),
    OPT_STRING(0, "primary-bootloader-offset", &pt_opt_primary_boot_offset,
	       "hex", "primary bootloader offset (CSV partition tables only)",
	       NULL),
    OPT_STRING(0, "recovery-bootloader-offset", &pt_opt_recovery_boot_offset,
	       "hex", "recovery bootloader offset (CSV partition tables only)",
	       NULL),
    OPT_STRING_LIST(0, "extra-partition-subtypes", &pt_opt_extra_subtypes,
		    "T,N,V", "register custom subtypes (e.g. data,foo,0x80)",
		    NULL),
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

int cmd_target_partition_write(int argc, const char **argv);

const struct cmd_desc cmd_target_partition_write_desc = {
    .name = "write",
    .fn = cmd_target_partition_write,
    .opts = cmd_target_partition_write_opts,
    .manual = &target_partition_write_manual,
};

int cmd_target_partition_write(int argc, const char **argv)
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
	struct sbuf data = SBUF_INIT;
	esp_loader_error_t lerr;
	uint32_t image_size;

	pt_target_reset_opts();
	opt_input = NULL;
	opt_ignore_readonly = 0;

	argc = parse_options(argc, argv, &cmd_target_partition_write_desc);
	(void)argc;
	(void)argv;

	pt_target_finalize_opts();

	if (!opt_input)
		die("--input is required");

	sel.name = pt_opt_name;
	sel.type = pt_opt_type;
	sel.subtype = pt_opt_subtype;
	sel.boot_default = pt_opt_boot_default;

	table_offset = (uint32_t)strtoul(pt_opt_pt_offset, &end, 0);
	if (*end)
		die("invalid --partition-table-offset: %s", pt_opt_pt_offset);

	if (sbuf_read_file(&data, opt_input) < 0) {
		err_errno("cannot read '%s'", opt_input);
		return 1;
	}

	if (pt_target_connect(&loader, &sport, pt_opt_port, pt_opt_baud,
			      &autoport) != 0)
		goto out;

	if (pt_target_load(entries, &count, &loader, table_offset) != 0)
		goto out;

	e = pt_target_select(entries, count, &sel, &loader);
	if (!e)
		goto out;

	if (e->readonly && !opt_ignore_readonly) {
		err("partition '%s' is marked readonly; pass "
		    "--ignore-readonly to override",
		    e->name);
		goto out;
	}

	image_size = (uint32_t)data.len;
	while (image_size % 4)
		image_size++;
	if (image_size > e->size) {
		err("input '%s' (0x%zx bytes) does not fit in partition '%s' "
		    "(size 0x%x)",
		    opt_input, data.len, e->name, e->size);
		goto out;
	}

	{
		esp_loader_flash_cfg_t cfg = {
		    .offset = e->offset,
		    .image_size = image_size,
		    .block_size = WRITE_BLOCK_SIZE,
		};

		lerr = esp_loader_flash_start(&loader, &cfg);
		if (lerr != ESP_LOADER_SUCCESS) {
			err("flash_start failed (esp-loader err %d)", lerr);
			goto out;
		}

		const uint8_t *p = (const uint8_t *)data.buf;
		uint32_t remaining = image_size;
		printf("Writing partition '%s' at 0x%x (0x%x bytes)...\n",
		       e->name, e->offset, image_size);
		fflush(stdout);
		while (remaining > 0) {
			uint32_t chunk = remaining < WRITE_BLOCK_SIZE
					     ? remaining
					     : WRITE_BLOCK_SIZE;
			uint8_t block[WRITE_BLOCK_SIZE];
			uint32_t real =
			    (uint32_t)(data.len -
				       (size_t)(p - (const uint8_t *)data.buf));
			if (real > chunk)
				real = chunk;
			memcpy(block, p, real);
			if (real < chunk)
				memset(block + real, 0xFF, chunk - real);

			lerr =
			    esp_loader_flash_write(&loader, &cfg, block, chunk);
			if (lerr != ESP_LOADER_SUCCESS) {
				err("flash_write failed (esp-loader err %d)",
				    lerr);
				goto out;
			}
			p += real;
			remaining -= chunk;
		}

		lerr = esp_loader_flash_finish(&loader, &cfg);
		if (lerr != ESP_LOADER_SUCCESS) {
			err("flash MD5 verify failed (esp-loader err %d)",
			    lerr);
			goto out;
		}
	}

	printf("Written contents of file '%s' at offset 0x%x\n", opt_input,
	       e->offset);

	rc = 0;
out:
	sbuf_release(&data);
	free(autoport);
	return rc;
}
