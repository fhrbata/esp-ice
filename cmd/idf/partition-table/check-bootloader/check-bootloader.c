/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/partition-table/check-bootloader/check-bootloader.c
 * @brief "ice idf partition-table check-bootloader" -- size guard.
 *
 * Replaces @b{check_sizes.py bootloader}.  Verifies that the
 * bootloader binary fits in the gap between its base offset and
 * the partition table offset; emits a free-space summary on
 * success, an error (or warning, with --allow-failures) on
 * overflow.
 */
#include "ice.h"
#include "partition_table.h"

/* clang-format off */
static const struct cmd_manual idf_pt_check_bl_manual = {
	.name = "ice idf partition-table check-bootloader",
	.summary = "verify bootloader binary fits before the partition table",

	.description =
	H_PARA("Drop-in replacement for ESP-IDF's @b{check_sizes.py "
	       "bootloader}.  Reports the binary size and remaining free "
	       "space (the gap between @b{--offset} and the bootloader "
	       "offset) on stdout in the same format the IDF build "
	       "system prints.")
	H_PARA("On overflow exits with status 1 and an @b{Error:} line on "
	       "stderr; with @b{--allow-failures} the same message is "
	       "downgraded to a @b{Warning:} on stdout and the process "
	       "exits 0."),

	.examples =
	H_EXAMPLE("ice idf partition-table check-bootloader 0x0 build/bootloader/bootloader.bin")
	H_EXAMPLE("ice idf partition-table check-bootloader --offset 0x10000 0x1000 bl.bin"),
};
/* clang-format on */

static const char *offset_str = "0x8000";
static int allow_failures;
static const char *target_unused; /* accepted for compat with check_sizes.py */

static const struct option cmd_idf_pt_check_bl_opts[] = {
    OPT_STRING(0, "offset", &offset_str, "hex",
	       "partition table offset (default 0x8000)", NULL),
    OPT_BOOL(0, "allow-failures", &allow_failures,
	     "downgrade overflow errors to warnings (still exits 0)"),
    OPT_STRING(0, "target", &target_unused, "name",
	       "ignored; accepted for check_sizes.py compatibility", NULL),
    OPT_END(),
};

int cmd_idf_pt_check_bootloader(int argc, const char **argv);

const struct cmd_desc cmd_idf_pt_check_bootloader_desc = {
    .name = "check-bootloader",
    .fn = cmd_idf_pt_check_bootloader,
    .opts = cmd_idf_pt_check_bl_opts,
    .manual = &idf_pt_check_bl_manual,
};

static long file_size(FILE *fp)
{
	long pos;

	if (fseek(fp, 0, SEEK_END) != 0)
		return -1;
	pos = ftell(fp);
	if (fseek(fp, 0, SEEK_SET) != 0)
		return -1;
	return pos;
}

/* Round half-to-even, matching Python's int(round(x)). */
static unsigned percent_round(uint64_t numerator, uint64_t denominator)
{
	uint64_t q = numerator / denominator;
	uint64_t r = numerator % denominator;
	uint64_t twice_r = r * 2;

	if (twice_r < denominator)
		return (unsigned)q;
	if (twice_r > denominator)
		return (unsigned)(q + 1);
	/* exact half: round to even */
	return (unsigned)((q & 1) ? q + 1 : q);
}

int cmd_idf_pt_check_bootloader(int argc, const char **argv)
{
	uint32_t pt_offset, bl_offset, max_size;
	const char *bl_path;
	FILE *fp;
	long bl_size;
	char *end;

	argc = parse_options(argc, argv, &cmd_idf_pt_check_bootloader_desc);

	if (argc < 2)
		die("usage: ice idf partition-table check-bootloader "
		    "<bootloader_offset> <bootloader_binary>");

	pt_offset = (uint32_t)strtoul(offset_str, &end, 0);
	if (*end)
		die("invalid --offset value: %s", offset_str);

	bl_offset = (uint32_t)strtoul(argv[0], &end, 0);
	if (*end)
		die("invalid bootloader_offset: %s", argv[0]);

	if (pt_offset <= bl_offset)
		die("partition table offset 0x%x is not after bootloader "
		    "offset 0x%x",
		    pt_offset, bl_offset);

	max_size = pt_offset - bl_offset;

	bl_path = argv[1];
	fp = fopen(bl_path, "rb");
	if (!fp) {
		err_errno("cannot open '%s'", bl_path);
		return 1;
	}
	bl_size = file_size(fp);
	fclose(fp);
	if (bl_size < 0) {
		err("cannot determine size of '%s'", bl_path);
		return 1;
	}

	if ((uint64_t)bl_size > max_size) {
		const char *prefix = allow_failures ? "Warning" : "Error";
		FILE *out = allow_failures ? stdout : stderr;

		fprintf(out,
			"%s: Bootloader binary size 0x%lx bytes is too large "
			"for partition table offset 0x%x. Bootloader binary "
			"can be maximum 0x%x (%u) bytes unless the partition "
			"table offset is increased in the Partition Table "
			"section of the project configuration menu.\n",
			prefix, bl_size, pt_offset, max_size, max_size);
		return allow_failures ? 0 : 1;
	}

	{
		uint32_t free = max_size - (uint32_t)bl_size;
		unsigned pct = percent_round((uint64_t)free * 100, max_size);

		printf("Bootloader binary size 0x%lx bytes. 0x%x bytes (%u%%) "
		       "free.\n",
		       bl_size, free, pct);
	}

	return 0;
}
