/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for partition_table.c -- CSV parser and binary writer.
 */
#include "partition_table.h"
#include "tap.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Write CSV text to a uniquely-named temp file; return the path. */
static const char *write_csv(int n, const char *content)
{
	static char path[64];
	FILE *fp;

	snprintf(path, sizeof(path), "test_pt_%d.csv", n);
	fp = fopen(path, "w");
	if (!fp)
		return NULL;
	fputs(content, fp);
	fclose(fp);
	return path;
}

static struct pt_options default_opts(void)
{
	struct pt_options o;
	memset(&o, 0, sizeof(o));
	o.table_offset = 0x8000;
	o.md5sum = 1;
	return o;
}

int main(void)
{
	/* Parse the canonical IDF singleapp CSV: explicit offsets and sizes. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		const char *csv = write_csv(
		    1, "nvs,      data, nvs,     0x9000,  0x6000,\n"
		       "phy_init, data, phy,     0xf000,  0x1000,\n"
		       "factory,  app,  factory, 0x10000, 0x100000,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == 0);
		tap_check(n == 3);

		tap_check(strcmp(e[0].name, "nvs") == 0);
		tap_check(e[0].type == PT_TYPE_DATA);
		tap_check(e[0].subtype == 0x02); /* nvs */
		tap_check(e[0].offset == 0x9000);
		tap_check(e[0].size == 0x6000);

		tap_check(strcmp(e[1].name, "phy_init") == 0);
		tap_check(e[1].subtype == 0x01); /* phy */
		tap_check(e[1].offset == 0xf000);
		tap_check(e[1].size == 0x1000);

		tap_check(strcmp(e[2].name, "factory") == 0);
		tap_check(e[2].type == PT_TYPE_APP);
		tap_check(e[2].subtype == 0x00); /* factory */
		tap_check(e[2].offset == 0x10000);
		tap_check(e[2].size == 0x100000);

		tap_done("parse singleapp CSV: names, types, offsets, sizes");
	}

	/* K and M suffixes are expanded correctly. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		const char *csv =
		    write_csv(2, "nvs,     data, nvs,     0x9000, 24K,\n"
				 "factory, app,  factory, 0x10000, 1M,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == 0);
		tap_check(e[0].size == 24 * 1024);
		tap_check(e[1].size == 1024 * 1024);
		tap_done("K and M size suffixes are expanded");
	}

	/* ota_N subtypes map to 0x10 + N. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		const char *csv =
		    write_csv(3, "ota_0, app, ota_0, 0x10000, 0x100000,\n"
				 "ota_1, app, ota_1, 0x110000, 0x100000,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == 0);
		tap_check(e[0].subtype == 0x10);
		tap_check(e[1].subtype == 0x11);
		tap_done("ota_N subtype maps to 0x10 + N");
	}

	/* Auto-fill: missing offsets are assigned using IDF alignment rules.
	 * data partitions align to 0x1000, app partitions to 0x10000. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		/* No explicit offsets — all auto-filled. */
		const char *csv =
		    write_csv(4, "nvs,     data, nvs,     , 0x6000,\n"
				 "factory, app,  factory, , 0x100000,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == 0);
		/* nvs starts right after the partition table sector */
		tap_check(e[0].offset == o.table_offset + PT_TABLE_SIZE);
		/* factory (app) must be 0x10000-aligned; 0x9000+0x6000=0xf000
		 * rounds up to 0x10000 */
		tap_check(e[1].offset == 0x10000);
		tap_done("offsets auto-filled with correct alignment");
	}

	/* Flags: encrypted and readonly set the right bits. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		const char *csv = write_csv(
		    5, "nvs, data, nvs, 0x9000, 0x6000, encrypted:readonly\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == 0);
		tap_check(e[0].encrypted == 1);
		tap_check(e[0].readonly == 1);
		tap_done("encrypted and readonly flags parsed");
	}

	/* Binary: entry magic, type/subtype, offset, size, name. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		uint8_t bin[PT_DATA_SIZE];
		const char *csv =
		    write_csv(6, "nvs, data, nvs, 0x9000, 0x6000,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == 0);
		tap_check(pt_to_binary(e, n, &o, bin) == 0);

		/* First entry starts at offset 0. */
		tap_check(bin[0] == 0xAA);
		tap_check(bin[1] == 0x50);
		tap_check(bin[2] == PT_TYPE_DATA);
		tap_check(bin[3] == 0x02); /* nvs subtype */

		/* Offset 0x9000 little-endian at bytes 4-7. */
		tap_check(bin[4] == 0x00);
		tap_check(bin[5] == 0x90);
		tap_check(bin[6] == 0x00);
		tap_check(bin[7] == 0x00);

		/* Size 0x6000 little-endian at bytes 8-11. */
		tap_check(bin[8] == 0x00);
		tap_check(bin[9] == 0x60);
		tap_check(bin[10] == 0x00);
		tap_check(bin[11] == 0x00);

		/* Name at bytes 12-27. */
		tap_check(memcmp(bin + 12, "nvs", 3) == 0);
		tap_check(bin[15] == 0x00); /* null-padded */

		tap_done("binary entry: magic, type, offset, size, name");
	}

	/* Binary: MD5 sentinel entry has 0xEB 0xEB magic. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		uint8_t bin[PT_DATA_SIZE];
		const char *csv =
		    write_csv(7, "nvs, data, nvs, 0x9000, 0x6000,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == 0);
		tap_check(pt_to_binary(e, n, &o, bin) == 0);

		/* MD5 entry immediately follows the one data entry. */
		tap_check(bin[PT_ENTRY_SIZE + 0] == 0xEB);
		tap_check(bin[PT_ENTRY_SIZE + 1] == 0xEB);

		tap_done("MD5 sentinel entry has 0xEB 0xEB magic");
	}

	/* Binary: rest of buffer after entries+MD5 is 0xFF. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		uint8_t bin[PT_DATA_SIZE];
		int ok = 1;
		size_t i;
		const char *csv =
		    write_csv(8, "nvs, data, nvs, 0x9000, 0x6000,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == 0);
		tap_check(pt_to_binary(e, n, &o, bin) == 0);

		/* 1 data entry + 1 MD5 entry = 2 * PT_ENTRY_SIZE used. */
		for (i = 2 * PT_ENTRY_SIZE; i < PT_DATA_SIZE; i++) {
			if (bin[i] != 0xFF) {
				ok = 0;
				break;
			}
		}
		tap_check(ok);
		tap_done("padding after entries is 0xFF");
	}

	return tap_result();
}
