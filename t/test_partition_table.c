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

	/* Negative size: "-2M" fills from offset up to end-address 2M. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		const char *csv =
		    write_csv(9, "first,  app,  factory, 0x10000, -2M,\n"
				 "second, data, 0x15,    ,        1M,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == 0);
		tap_check(n == 2);
		tap_check(e[0].offset == 0x10000);
		tap_check(e[0].size == 0x200000 - 0x10000);
		tap_check(e[1].offset == 0x200000);
		tap_check(e[1].size == 0x100000);
		tap_done("negative size resolves to end_addr - offset");
	}

	/* Negative size with target address <= offset is rejected. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		const char *csv =
		    write_csv(10, "bad, data, spiffs, 0x400000, -0x10000,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == -1);
		tap_done("negative size with end_addr <= offset errors out");
	}

	/* Overlapping explicit offsets are rejected (mirrors IDF's test). */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		const char *csv =
		    write_csv(11, "first,  app, factory, 0x100000, 2M,\n"
				  "second, app, ota_0,   0x200000, 1M,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == -1);
		tap_done("overlapping explicit offsets are rejected");
	}

	/* Empty data subtype defaults to PT_SUBTYPE_DATA_UNDEFINED. */
	{
		struct pt_entry e[16];
		int n = 0;
		struct pt_options o = default_opts();
		const char *csv =
		    write_csv(12, "misc, data, , 0x9000, 0x1000,\n");

		tap_check(pt_parse_csv(csv, e, &n, &o) == 0);
		tap_check(e[0].subtype == PT_SUBTYPE_DATA_UNDEFINED);
		tap_done("empty data subtype defaults to 'undefined' (0x06)");
	}

	/* ------------------------------------------------------------------ */
	/* New surface: parse helpers, binary parser, CSV emitter, runtime */
	/* extras, format helpers, auto-detecting loader. */
	/* ------------------------------------------------------------------ */

	/* pt_parse_type accepts both symbolic names and numerics. */
	{
		uint8_t v;
		tap_check(pt_parse_type("app", &v) == 0 && v == PT_TYPE_APP);
		tap_check(pt_parse_type("data", &v) == 0 && v == PT_TYPE_DATA);
		tap_check(pt_parse_type("0x42", &v) == 0 && v == 0x42);
		tap_check(pt_parse_type("99", &v) == 0 && v == 99);
		tap_check(pt_parse_type("nope", &v) == -1);
		tap_check(pt_parse_type("0x100", &v) == -1); /* > 0xFF */
		tap_done("pt_parse_type: names, numerics, error cases");
	}

	/* pt_parse_subtype handles synthetic ranges, names, and numerics. */
	{
		uint8_t v;
		tap_check(pt_parse_subtype(PT_TYPE_APP, "ota_0", &v) == 0 &&
			  v == 0x10);
		tap_check(pt_parse_subtype(PT_TYPE_APP, "ota_15", &v) == 0 &&
			  v == 0x1F);
		tap_check(pt_parse_subtype(PT_TYPE_APP, "tee_1", &v) == 0 &&
			  v == 0x31);
		tap_check(pt_parse_subtype(PT_TYPE_DATA, "nvs", &v) == 0 &&
			  v == 0x02);
		tap_check(pt_parse_subtype(PT_TYPE_DATA, "0x83", &v) == 0 &&
			  v == 0x83);
		tap_check(pt_parse_subtype(PT_TYPE_APP, "ota_16", &v) == -1);
		tap_check(pt_parse_subtype(PT_TYPE_APP, "tee_2", &v) == -1);
		tap_done("pt_parse_subtype: ota_N, tee_N, names, numerics");
	}

	/* pt_register_subtype: extras are visible to pt_parse_subtype. */
	{
		uint8_t v;
		tap_check(pt_parse_subtype(PT_TYPE_DATA, "myfs", &v) == -1);
		tap_check(pt_register_subtype(PT_TYPE_DATA, "myfs", 0x80) == 0);
		tap_check(pt_parse_subtype(PT_TYPE_DATA, "myfs", &v) == 0 &&
			  v == 0x80);
		/* Idempotent re-register with same value succeeds. */
		tap_check(pt_register_subtype(PT_TYPE_DATA, "myfs", 0x80) == 0);
		/* Conflicting re-register fails. */
		tap_check(pt_register_subtype(PT_TYPE_DATA, "myfs", 0x81) ==
			  -1);
		/* Conflict with the static table is also rejected. */
		tap_check(pt_register_subtype(PT_TYPE_DATA, "nvs", 0x70) == -1);
		pt_clear_extras();
		tap_check(pt_parse_subtype(PT_TYPE_DATA, "myfs", &v) == -1);
		tap_done("pt_register_subtype: visibility, conflicts, clear");
	}

	/* pt_format_type / pt_format_subtype / pt_format_size. */
	{
		char buf[16];
		tap_check(strcmp(pt_format_type(PT_TYPE_APP, buf, sizeof(buf)),
				 "app") == 0);
		tap_check(strcmp(pt_format_type(PT_TYPE_DATA, buf, sizeof(buf)),
				 "data") == 0);
		tap_check(
		    strcmp(pt_format_type(0x42, buf, sizeof(buf)), "66") == 0);

		tap_check(strcmp(pt_format_subtype(PT_TYPE_APP, 0x10, buf,
						   sizeof(buf)),
				 "ota_0") == 0);
		tap_check(strcmp(pt_format_subtype(PT_TYPE_APP, 0x1F, buf,
						   sizeof(buf)),
				 "ota_15") == 0);
		tap_check(strcmp(pt_format_subtype(PT_TYPE_APP, 0x30, buf,
						   sizeof(buf)),
				 "tee_0") == 0);
		tap_check(strcmp(pt_format_subtype(PT_TYPE_DATA, 0x02, buf,
						   sizeof(buf)),
				 "nvs") == 0);
		tap_check(strcmp(pt_format_subtype(PT_TYPE_DATA, 0xFE, buf,
						   sizeof(buf)),
				 "254") == 0);

		tap_check(strcmp(pt_format_size(0x100000, buf, sizeof(buf)),
				 "1M") == 0);
		tap_check(strcmp(pt_format_size(0x6000, buf, sizeof(buf)),
				 "24K") == 0);
		tap_check(strcmp(pt_format_size(0x55, buf, sizeof(buf)),
				 "0x55") == 0);
		tap_done(
		    "pt_format_{type,subtype,size}: names, ota_N, K/M/hex");
	}

	/* Round-trip: pt_parse_csv -> pt_to_binary -> pt_from_binary. */
	{
		struct pt_entry in[16], out[16];
		int n_in = 0, n_out = 0;
		struct pt_options o = default_opts();
		uint8_t bin[PT_DATA_SIZE];
		const char *csv = write_csv(
		    100, "nvs,      data, nvs,     0x9000,  0x6000,\n"
			 "phy_init, data, phy,     0xf000,  0x1000,\n"
			 "factory,  app,  factory, 0x10000, 0x100000,\n");

		tap_check(pt_parse_csv(csv, in, &n_in, &o) == 0);
		tap_check(pt_to_binary(in, n_in, &o, bin) == 0);
		tap_check(pt_from_binary(bin, sizeof(bin), out, &n_out, 1) ==
			  0);
		tap_check(n_out == n_in);
		for (int i = 0; i < n_in; i++) {
			tap_check(strcmp(in[i].name, out[i].name) == 0);
			tap_check(in[i].type == out[i].type);
			tap_check(in[i].subtype == out[i].subtype);
			tap_check(in[i].offset == out[i].offset);
			tap_check(in[i].size == out[i].size);
			tap_check(in[i].encrypted == out[i].encrypted);
			tap_check(in[i].readonly == out[i].readonly);
			tap_check(out[i].offset_set == 1);
			tap_check(out[i].size_is_end_addr == 0);
		}
		tap_done("pt_to_binary -> pt_from_binary round-trip");
	}

	/* pt_from_binary: tampered MD5 is detected with verify=1. */
	{
		struct pt_entry in[16], out[16];
		int n_in = 0, n_out = 0;
		struct pt_options o = default_opts();
		uint8_t bin[PT_DATA_SIZE];
		const char *csv =
		    write_csv(101, "nvs, data, nvs, 0x9000, 0x6000,\n");

		tap_check(pt_parse_csv(csv, in, &n_in, &o) == 0);
		tap_check(pt_to_binary(in, n_in, &o, bin) == 0);

		/* Find MD5 entry, flip a digest byte. */
		for (size_t off = 0; off + PT_ENTRY_SIZE <= PT_DATA_SIZE;
		     off += PT_ENTRY_SIZE) {
			if (bin[off] == 0xEB && bin[off + 1] == 0xEB) {
				bin[off + 16] ^= 0x01;
				break;
			}
		}
		tap_check(pt_from_binary(bin, sizeof(bin), out, &n_out, 1) ==
			  -1);
		/* With verify_md5=0, the tampered binary still parses. */
		tap_check(pt_from_binary(bin, sizeof(bin), out, &n_out, 0) ==
			  0);
		tap_done("pt_from_binary: MD5 verification toggle");
	}

	/* pt_from_binary: invalid magic is rejected. */
	{
		struct pt_entry out[16];
		int n_out = 0;
		uint8_t bin[PT_DATA_SIZE];
		memset(bin, 0xFF, sizeof(bin));
		bin[0] = 0xCA; /* not 0xAA50, not 0xEBEB, not all-FF */
		bin[1] = 0xFE;
		tap_check(pt_from_binary(bin, sizeof(bin), out, &n_out, 0) ==
			  -1);
		tap_done("pt_from_binary: rejects invalid entry magic");
	}

	/* pt_to_csv: matches gen_esp32part.py output verbatim for the
	 * canonical singleapp table (header + 3 rows + trailing newline). */
	{
		struct pt_entry in[16];
		int n_in = 0;
		struct pt_options o = default_opts();
		FILE *fp;
		char buf[1024];
		size_t len;
		const char *expected =
		    "# ESP-IDF Partition Table\n"
		    "# Name, Type, SubType, Offset, Size, Flags\n"
		    "nvs,data,nvs,0x9000,24K,\n"
		    "phy_init,data,phy,0xf000,4K,\n"
		    "factory,app,factory,0x10000,1M,\n";

		const char *csv = write_csv(
		    102, "nvs,      data, nvs,     0x9000,  0x6000,\n"
			 "phy_init, data, phy,     0xf000,  0x1000,\n"
			 "factory,  app,  factory, 0x10000, 0x100000,\n");

		tap_check(pt_parse_csv(csv, in, &n_in, &o) == 0);

		fp = fopen("test_pt_csv.out", "w");
		tap_check(fp != NULL);
		tap_check(pt_to_csv(in, n_in, fp) == 0);
		fclose(fp);

		fp = fopen("test_pt_csv.out", "r");
		tap_check(fp != NULL);
		len = fread(buf, 1, sizeof(buf) - 1, fp);
		buf[len] = '\0';
		fclose(fp);

		tap_check(strcmp(buf, expected) == 0);
		tap_done("pt_to_csv: byte-exact match against expected CSV");
	}

	/* pt_load: auto-detect CSV vs binary by first two bytes. */
	{
		struct pt_entry in[16], out[16];
		int n_in = 0, n_out = 0;
		struct pt_options o = default_opts();
		uint8_t bin[PT_DATA_SIZE];
		FILE *fp;
		const char *csv =
		    write_csv(103, "nvs, data, nvs, 0x9000, 0x6000,\n"
				   "factory, app, factory, 0x10000, 1M,\n");

		/* Sanity: pt_load on a CSV path defers to pt_parse_csv. */
		tap_check(pt_load(csv, out, &n_out, &o) == 0);
		tap_check(n_out == 2);

		/* Same logical table written as binary. */
		tap_check(pt_parse_csv(csv, in, &n_in, &o) == 0);
		tap_check(pt_to_binary(in, n_in, &o, bin) == 0);
		fp = fopen("test_pt_load.bin", "wb");
		tap_check(fp != NULL);
		tap_check(fwrite(bin, 1, sizeof(bin), fp) == sizeof(bin));
		fclose(fp);

		n_out = 0;
		tap_check(pt_load("test_pt_load.bin", out, &n_out, &o) == 0);
		tap_check(n_out == 2);
		tap_check(strcmp(out[0].name, "nvs") == 0);
		tap_check(strcmp(out[1].name, "factory") == 0);
		tap_done("pt_load: dispatches by magic to CSV or binary path");
	}

	return tap_result();
}
