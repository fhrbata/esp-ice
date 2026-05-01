/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/partition-table/empty/empty.c
 * @brief "ice idf partition-table empty" -- 0xFF-filled blob.
 *
 * Replaces @b{gen_empty_partition.py}.  Writes @p size bytes of
 * 0xFF (flash erased state) to a file or stdout.  Used by the
 * build system to produce a blank @c otadata partition image.
 */
#include "ice.h"
#include "partition_table.h"

/* clang-format off */
static const struct cmd_manual idf_pt_empty_manual = {
	.name = "ice idf partition-table empty",
	.summary = "write a 0xFF-filled binary of the requested size",

	.description =
	H_PARA("Drop-in replacement for ESP-IDF's "
	       "@b{gen_empty_partition.py}.  Output goes to stdout when "
	       "the path is omitted or set to @b{-}; otherwise the parent "
	       "directory of the output path is created on demand.")
	H_PARA("Size is parsed by @b{strtoul(_, _, 0)}, so decimal "
	       "(@b{4096}) and hex (@b{0x1000}) are both accepted."),

	.examples =
	H_EXAMPLE("ice idf partition-table empty 0x2000 build/otadata.bin")
	H_EXAMPLE("ice idf partition-table empty 4096 - > blank.bin"),
};
/* clang-format on */

static const struct option cmd_idf_pt_empty_opts[] = {
    OPT_END(),
};

int cmd_idf_pt_empty(int argc, const char **argv);

const struct cmd_desc cmd_idf_pt_empty_desc = {
    .name = "empty",
    .fn = cmd_idf_pt_empty,
    .opts = cmd_idf_pt_empty_opts,
    .manual = &idf_pt_empty_manual,
};

/* Write @p size bytes of 0xFF to @p out. */
static int write_blank(FILE *out, uint32_t size, const char *label)
{
	uint8_t buf[4096];

	memset(buf, 0xFF, sizeof(buf));
	for (uint32_t remaining = size; remaining > 0;) {
		size_t chunk =
		    remaining > sizeof(buf) ? sizeof(buf) : remaining;
		if (fwrite(buf, 1, chunk, out) != chunk) {
			err_errno("write error on '%s'", label);
			return -1;
		}
		remaining -= (uint32_t)chunk;
	}
	return 0;
}

int cmd_idf_pt_empty(int argc, const char **argv)
{
	const char *output_path;
	uint32_t size;
	char *end;

	argc = parse_options(argc, argv, &cmd_idf_pt_empty_desc);

	if (argc < 1)
		die("usage: ice idf partition-table empty <size> [<output>]");

	size = (uint32_t)strtoul(argv[0], &end, 0);
	if (*end || end == argv[0])
		die("invalid size: %s", argv[0]);

	output_path = (argc >= 2) ? argv[1] : "-";

	/* Two distinct paths so the static analyzer can prove no fclose
	 * is missed: stdout is never closed, the fopen'd file always is. */
	if (!strcmp(output_path, "-"))
		return write_blank(stdout, size, output_path) < 0 ? 1 : 0;

	mkdirp_for_file(output_path);
	{
		FILE *fp = fopen(output_path, "wb");
		int rc;

		if (!fp) {
			err_errno("cannot write '%s'", output_path);
			return 1;
		}
		rc = write_blank(fp, size, output_path);
		fclose(fp);
		return rc < 0 ? 1 : 0;
	}
}
