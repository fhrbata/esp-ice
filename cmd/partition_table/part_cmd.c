/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/partition_table/part_cmd.c
 * @brief "ice partition-table" subcommand.
 *
 * Drop-in replacement for gen_esp32part.py (CSV → binary).
 * Accepts the same flags that IDF's partition_table cmake component
 * passes to gen_esp32part.py.
 *
 * Usage (matches gen_esp32part.py):
 *   ice partition-table [-q]
 *                       [--offset <hex>]
 *                       [--primary-bootloader-offset <hex>]
 *                       [--recovery-bootloader-offset <hex>]
 *                       [--disable-md5sum]
 *                       [--no-verify]
 *                       [--flash-size <N>MB]
 *                       [--secure v1|v2]
 *                       [--extra-partition-subtypes ...]
 *                       [--]
 *                       <input.csv> <output.bin>
 */
#include "../../ice.h"
#include "../../partition_table.h"

static const char *pt_usage[] = {
    "ice partition-table [options] [--] <input.csv> <output.bin>",
    NULL,
};

int cmd_partition_table(int argc, const char **argv)
{
	const char *offset_str = "0x8000";
	const char *pbl_offset_str = NULL;
	const char *rbl_offset_str = NULL;
	const char *flash_size_str = NULL;
	const char *secure_str = NULL;
	int disable_md5 = 0;
	int no_verify = 0;
	int quiet = 0;
	/* --extra-partition-subtypes: accepted, ignored (numeric always works)
	 */
	const char *extra_subtypes = NULL;

	struct option opts[] = {
	    OPT_BOOL('q', "quiet", &quiet, "suppress non-error output"),
	    OPT_STRING(0, "offset", &offset_str, "hex",
		       "partition table offset in flash (default 0x8000)"),
	    OPT_STRING(0, "primary-bootloader-offset", &pbl_offset_str, "hex",
		       "primary bootloader offset in flash"),
	    OPT_STRING(0, "recovery-bootloader-offset", &rbl_offset_str, "hex",
		       "recovery bootloader offset in flash"),
	    OPT_BOOL(0, "disable-md5sum", &disable_md5,
		     "disable MD5 checksum entry"),
	    OPT_BOOL(0, "no-verify", &no_verify,
		     "skip partition table validation"),
	    OPT_STRING(0, "flash-size", &flash_size_str, "NMB",
		       "flash size for validation (e.g. 4MB)"),
	    OPT_STRING(0, "secure", &secure_str, "v1|v2",
		       "secure boot version"),
	    OPT_STRING(0, "extra-partition-subtypes", &extra_subtypes, "...",
		       "extra subtype definitions (ignored)"),
	    OPT_END(),
	};

	struct pt_options pt_opts;
	struct pt_entry entries[PT_MAX_ENTRIES];
	uint8_t binary[PT_DATA_SIZE];
	int count = 0;
	const char *input_path;
	const char *output_path;
	FILE *fp;

	argc = parse_options(argc, argv, opts, pt_usage);

	if (argc < 2)
		die("usage: ice partition-table [options] <input.csv> "
		    "<output.bin>");

	input_path = argv[0];
	output_path = argv[1];

	/* Build options struct */
	memset(&pt_opts, 0, sizeof(pt_opts));
	pt_opts.md5sum = !disable_md5;

	/* --offset */
	{
		uint32_t v;
		char *end;
		v = (uint32_t)strtoul(offset_str, &end, 0);
		if (*end)
			die("invalid --offset value: %s", offset_str);
		pt_opts.table_offset = v;
	}

	/* --primary-bootloader-offset */
	if (pbl_offset_str) {
		char *end;
		pt_opts.primary_boot_offset =
		    (uint32_t)strtoul(pbl_offset_str, &end, 0);
		if (*end)
			die("invalid --primary-bootloader-offset: %s",
			    pbl_offset_str);
		pt_opts.has_primary_boot = 1;
	}

	/* --recovery-bootloader-offset */
	if (rbl_offset_str) {
		char *end;
		pt_opts.recovery_boot_offset =
		    (uint32_t)strtoul(rbl_offset_str, &end, 0);
		if (*end)
			die("invalid --recovery-bootloader-offset: %s",
			    rbl_offset_str);
		pt_opts.has_recovery_boot = 1;
	}

	/* --flash-size (e.g. "4MB") */
	if (flash_size_str) {
		uint32_t mb = (uint32_t)strtoul(flash_size_str, NULL, 10);
		pt_opts.flash_size = mb * 1024 * 1024;
	}

	/* --secure */
	if (secure_str) {
		if (!strcmp(secure_str, "v1"))
			pt_opts.secure = PT_SECURE_V1;
		else if (!strcmp(secure_str, "v2"))
			pt_opts.secure = PT_SECURE_V2;
		else
			die("invalid --secure value: %s (use v1 or v2)",
			    secure_str);
	}

	/* Parse CSV */
	if (pt_parse_csv(input_path, entries, &count, &pt_opts) != 0)
		return 1;

	/* Validate (unless --no-verify) */
	if (!no_verify && count == 0) {
		err("empty partition table");
		return 1;
	}

	/* Print table to stderr (unless -q) */
	if (!quiet) {
		fprintf(stderr, "Partition table:\n");
		fprintf(stderr, "%-16s %-12s %-12s %-10s %-10s %s\n", "Name",
			"Type", "SubType", "Offset", "Size", "Flags");
		for (int i = 0; i < count; i++) {
			const struct pt_entry *e = &entries[i];
			fprintf(stderr,
				"%-16s 0x%02x         0x%02x         "
				"0x%08x 0x%08x %s%s\n",
				e->name, e->type, e->subtype, e->offset,
				e->size, e->encrypted ? "encrypted " : "",
				e->readonly ? "readonly" : "");
		}
	}

	/* Serialise */
	if (pt_to_binary(entries, count, &pt_opts, binary) != 0)
		return 1;

	/* Create output directory */
	mkdirp_for_file(output_path);

	/* Write output */
	fp = fopen(output_path, "wb");
	if (!fp) {
		err_errno("cannot write '%s'", output_path);
		return 1;
	}
	if (fwrite(binary, 1, PT_DATA_SIZE, fp) != PT_DATA_SIZE) {
		err_errno("write error on '%s'", output_path);
		fclose(fp);
		return 1;
	}
	fclose(fp);

	return 0;
}
